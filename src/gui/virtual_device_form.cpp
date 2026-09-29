#include "virtual_device_form.hpp"
#include "setup_model.hpp"
#include "discovery.hpp"
#include "control_browser.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <linux/input-event-codes.h>
#include <limits>

using namespace joystick_penguin;
namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
QSpinBox* number(int value, int min, int max, QWidget* parent) {
    auto* box = new QSpinBox(parent);
    box->setRange(min, max);
    box->setValue(value);
    return box;
}
}

VirtualDeviceForm::VirtualDeviceForm(const Config& config, std::string key, Commit commit, Error error, QWidget* parent)
    : QWidget(parent), config_(config), key_(std::move(key)), commit_(std::move(commit)), error_(std::move(error)) {
    const auto& device = config_.devices.at(key_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    buildProfileFields(layout, device);
    buildAdvancedFields(layout, device);
    layout->addStretch();
    connectEdits();
}

void VirtualDeviceForm::buildProfileFields(QVBoxLayout* layout, const Device& device) {
    auto* fields = new QWidget(this);
    auto* form = new QFormLayout(fields);
    name_ = new QLineEdit(qs(key_), fields);
    name_->setObjectName("setupName");
    form->addRow("Profile identifier", name_);
    display_ = new QLineEdit(qs(device.virtual_name), fields);
    display_->setObjectName("virtualDeviceName");
    display_->setPlaceholderText("JP " + qs(key_));
    form->addRow("Joystick display name", display_);
    form->addRow("Capabilities", new QLabel("Joystick preset · 79 buttons · 4 hats · declared axes", fields));
    auto* refresh = new QPushButton("Refresh from controller…", fields);
    refresh->setObjectName("refreshVirtualDevice");
    refresh->setToolTip("Update matching axis ranges and replace preset button labels from a configured controller. Other axes are retained.");
    QStringList sources;
    for (const auto& [name, source] : config_.devices)
        if (source.kind == DeviceKind::Evdev) sources << qs(name);
    refresh->setEnabled(!sources.empty());
    form->addRow(refresh);
    connect(refresh, &QPushButton::clicked, this, [this, sources] {
        bool ok = false;
        const QString selected = QInputDialog::getItem(this, "Refresh virtual joystick", "Controller", sources, 0, false, &ok);
        if (!ok || selected.isEmpty()) return;
        const auto source = selected.toStdString();
        const auto detected = profile_setup::inspect_device(config_.devices.at(source).path);
        if (!detected.issue.empty()) {
            if (error_) error_("Cannot read controller '" + selected + "': " + qs(detected.issue));
            return;
        }
        auto next = proposed();
        profile_setup::refresh_virtual_device(next, source, key_, detected.axis_ranges);
        if (commit_(std::move(next), true, {})) setEnabled(false);
    });
    layout->addWidget(fields);
}

void VirtualDeviceForm::buildAdvancedFields(QVBoxLayout* layout, const Device& device) {
    auto* advanced = new QGroupBox("Advanced identity and axis ranges", this);
    auto* advancedLayout = new QVBoxLayout(advanced);
    auto* advancedFields = new QWidget(advanced);
    auto* extra = new QFormLayout(advancedFields);
    advancedLayout->addWidget(advancedFields);
    layout->addWidget(advanced);
    bus_ = new QComboBox(advancedFields);
    bus_->setObjectName("virtualBus");
    bus_->addItems({"virtual", "usb"});
    bus_->setCurrentIndex(device.bus == VirtualBus::Usb ? 1 : 0);
    vendor_ = number(device.vendor_id, 0, 65535, advancedFields);
    vendor_->setObjectName("vendorId");
    product_ = number(device.product_id, 0, 65535, advancedFields);
    product_->setObjectName("productId");
    for (auto* id : {vendor_, product_}) {
        id->setDisplayIntegerBase(16);
        id->setPrefix("0x");
    }
    auto* busRow = new QWidget(advancedFields);
    auto* busLayout = new QHBoxLayout(busRow);
    busLayout->setContentsMargins(0, 0, 0, 0);
    busLayout->addWidget(bus_);
    auto* busInfo = new QToolButton(busRow);
    busInfo->setText("ⓘ");
    busInfo->setAutoRaise(true);
    busInfo->setAccessibleName("About virtual bus identity");
    busLayout->addWidget(busInfo);
    busLayout->addStretch();
    extra->addRow("Bus", busRow);
    extra->addRow("Vendor ID", vendor_);
    extra->addRow("Product ID", product_);
    connect(busInfo, &QToolButton::clicked, this, [busInfo] {
        QToolTip::showText(busInfo->mapToGlobal(busInfo->rect().bottomLeft()),
            "<table width='320'><tr><td>Bus identity reported to applications: Virtual or USB. Event behavior is unchanged.</td></tr></table>", busInfo);
    });
    buildAxisTable(extra, advancedFields, device);
}

void VirtualDeviceForm::buildAxisTable(QFormLayout* extra, QWidget* advancedFields, const Device& device) {
    axes_ = new QTableWidget(advancedFields);
    axes_->setObjectName("virtualAxes");
    axes_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    axes_->setColumnCount(5);
    axes_->setHorizontalHeaderLabels({"Axis", "Resolution", "Neutral", "Min / max / neutral", "Advanced mode"});
    axes_->verticalHeader()->hide();
    auto append = [this](int code, AxisRange range) {
        const int row = axes_->rowCount();
        axes_->insertRow(row);
        auto* item = new QTableWidgetItem(inputName({"", ControlKind::AbsoluteAxis, code}) + QString(" · EV_ABS %1").arg(code));
        item->setData(Qt::UserRole, code);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        axes_->setItem(row, 0, item);
        const auto settings = axis_resolution_settings(range);
        auto* resolution = new QComboBox(axes_);
        for (int bits = 8; bits <= 16; ++bits) resolution->addItem(QString::number(bits) + " bits", bits);
        resolution->setCurrentIndex(settings ? settings->first - 8 : 4);
        axes_->setCellWidget(row, 1, resolution);
        auto* neutral = new QComboBox(axes_);
        neutral->addItems({"Middle", "Zero"});
        neutral->setCurrentIndex(settings && settings->second ? 1 : 0);
        axes_->setCellWidget(row, 2, neutral);
        auto* advanced = new QCheckBox(axes_);
        advanced->setAccessibleName(QString("Advanced mode for EV_ABS %1").arg(code));
        advanced->setChecked(!settings);
        axes_->setCellWidget(row, 4, advanced);
        auto* custom = new QWidget(axes_);
        auto* fields = new QHBoxLayout(custom);
        fields->setContentsMargins(0, 0, 0, 0);
        for (auto [label, value] : {std::pair{"min ", range.minimum}, {"max ", range.maximum}, {"neutral ", range.neutral}}) {
            auto* box = number(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), custom);
            box->setPrefix(label);
            box->setFixedWidth(box->fontMetrics().horizontalAdvance("neutral -2147483648") + 30);
            fields->addWidget(box);
        }
        axes_->setCellWidget(row, 3, custom);
        updateAxisRow(row);
    };
    for (const auto& [code, range] : device.axes) append(code, range);
    resizeAxisTable();
    extra->addRow(axes_);
    auto* addAxis = new QPushButton("Add axis…", advancedFields);
    extra->addRow(addAxis);
    auto* removeAxis = new QPushButton("Reset / remove selected axis", advancedFields);
    extra->addRow(removeAxis);

    // Adding or removing an axis rebuilds the form after the transaction.
    connect(addAxis, &QPushButton::clicked, this, [this] {
        bool ok;
        const int code = QInputDialog::getInt(this, "Add axis", "EV_ABS code (excluding hats 16–23)", 8, 0, ABS_MAX, 1, &ok);
        if (!ok || (code >= ABS_HAT0X && code <= ABS_HAT3Y)) return;
        for (int row = 0; row < axes_->rowCount(); ++row)
            if (axes_->item(row, 0)->data(Qt::UserRole).toInt() == code) return;
        auto next = proposed();
        next.devices.at(key_).axes[code] = axis_resolution(12, false);
        if (commit_(std::move(next), true, {})) setEnabled(false);
    });
    connect(removeAxis, &QPushButton::clicked, this, [this] {
        const int row = axes_->currentRow();
        if (row < 0) return;
        const int code = axes_->item(row, 0)->data(Qt::UserRole).toInt();
        auto next = proposed();
        try { profile_setup::remove_axis(next, key_, code); }
        catch (const ConfigError& failure) { if (error_) error_(qs(failure.what())); return; }
        if (commit_(std::move(next), true, {})) setEnabled(false);
    });
}

void VirtualDeviceForm::connectEdits() {
    // Field edits stay in this form; each proposal starts from the current config.
    connect(display_, &QLineEdit::editingFinished, this, [this] {
        if (display_->text().trimmed().toStdString() != config_.devices.at(key_).virtual_name)
            commit_(proposed(), false, {});
    });
    connect(bus_, &QComboBox::currentIndexChanged, this, [this] { commit_(proposed(), false, {}); });
    connect(vendor_, &QSpinBox::valueChanged, this, [this] { commit_(proposed(), false, {}); });
    connect(product_, &QSpinBox::valueChanged, this, [this] { commit_(proposed(), false, {}); });
    for (int row = 0; row < axes_->rowCount(); ++row) {
        auto changed = [this, row] { updateAxisRow(row); resizeAxisTable(); commit_(proposed(), false, {}); };
        connect(static_cast<QComboBox*>(axes_->cellWidget(row, 1)), &QComboBox::currentIndexChanged, this, changed);
        connect(static_cast<QComboBox*>(axes_->cellWidget(row, 2)), &QComboBox::currentIndexChanged, this, changed);
        connect(static_cast<QCheckBox*>(axes_->cellWidget(row, 4)), &QCheckBox::toggled, this, changed);
        for (auto* box : axes_->cellWidget(row, 3)->findChildren<QSpinBox*>())
            connect(box, &QSpinBox::valueChanged, this, [this, row] {
                if (static_cast<QCheckBox*>(axes_->cellWidget(row, 4))->isChecked()) {
                    updateAxisRow(row); resizeAxisTable(); commit_(proposed(), false, {});
                }
            });
    }
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const auto newName = name_->text().trimmed().toStdString();
        if (newName == key_) return;
        auto next = proposed();
        profile_setup::rename_device(next, key_, newName);
        if (commit_(std::move(next), true, qs(newName))) setEnabled(false);
    });
}

Config VirtualDeviceForm::proposed() const {
    Config next = config_;
    auto& device = next.devices.at(key_);
    device.virtual_name = display_->text().trimmed().toStdString();
    device.bus = bus_->currentIndex() ? VirtualBus::Usb : VirtualBus::Virtual;
    device.vendor_id = vendor_->value();
    device.product_id = product_->value();
    device.axes.clear();
    for (int row = 0; row < axes_->rowCount(); ++row)
        device.axes[axes_->item(row, 0)->data(Qt::UserRole).toInt()] = axisRowRange(row);
    return next;
}

AxisRange VirtualDeviceForm::axisRowRange(int row) const {
    if (!static_cast<QCheckBox*>(axes_->cellWidget(row, 4))->isChecked())
        return axis_resolution(static_cast<QComboBox*>(axes_->cellWidget(row, 1))->currentData().toInt(),
                               static_cast<QComboBox*>(axes_->cellWidget(row, 2))->currentIndex() == 1);
    const auto boxes = axes_->cellWidget(row, 3)->findChildren<QSpinBox*>();
    return {boxes.at(0)->value(), boxes.at(1)->value(), boxes.at(2)->value()};
}

void VirtualDeviceForm::updateAxisRow(int row) {
    const bool advanced = static_cast<QCheckBox*>(axes_->cellWidget(row, 4))->isChecked();
    axes_->cellWidget(row, 1)->setEnabled(!advanced);
    axes_->cellWidget(row, 2)->setEnabled(!advanced);
    auto* custom = axes_->cellWidget(row, 3);
    if (!advanced) {
        const auto range = axisRowRange(row);
        const auto boxes = custom->findChildren<QSpinBox*>();
        for (int i = 0; i < 3; ++i) {
            const QSignalBlocker blocker(boxes.at(i));
            boxes.at(i)->setValue(i == 0 ? range.minimum : i == 1 ? range.maximum : range.neutral);
        }
    }
    custom->setEnabled(advanced);
}

void VirtualDeviceForm::resizeAxisTable() {
    axes_->resizeColumnsToContents();
    axes_->resizeRowsToContents();
    int height = axes_->horizontalHeader()->height() + 2 * axes_->frameWidth();
    for (int row = 0; row < axes_->rowCount(); ++row) height += axes_->rowHeight(row);
    axes_->setFixedSize(axes_->horizontalHeader()->length() + 2 * axes_->frameWidth() + 2, height + 2);
}
