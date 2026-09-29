#include "setup_workspace.hpp"
#include "setup_model.hpp"
#include "discovery.hpp"
#include "control_browser.hpp"
#include "mapping_model.hpp"
#include "physical_device_form.hpp"
#include "virtual_device_form.hpp"
#include "condition_forms.hpp"
#include "joystick_penguin/joystick_preset.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QAbstractButton>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace joystick_penguin;
namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}

SetupWorkspace::SetupWorkspace(const Config& config, bool devices, QWidget* parent)
    : QWidget(parent), config_(config), devices_(devices) {
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);
    auto* left = new QWidget(splitter);
    auto* column = new QVBoxLayout(left);
    list_ = new QListWidget(left);
    list_->setObjectName(devices ? "workspaceDevices" : "workspaceModesModifiers");
    column->addWidget(list_, 1);

    // Only the list and usage scroll position outlive a selected property form.
    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignTop);
    properties_ = new QWidget;
    detail_ = new QVBoxLayout(properties_);
    scroll->setWidget(properties_);
    splitter->addWidget(left);
    splitter->addWidget(scroll);
    splitter->setSizes({300, 850});
    connect(list_, &QListWidget::currentRowChanged, this, [this] { select(); });
    refresh();
}

void SetupWorkspace::hideEvent(QHideEvent* event) {
    usageHidden_ = true;
    QWidget::hideEvent(event);
}

void SetupWorkspace::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    usageHidden_ = false;
}

void SetupWorkspace::refresh() {
    QString selected = renamedSelection_.isEmpty() && list_->currentItem()
        ? list_->currentItem()->data(Qt::UserRole).toString() : renamedSelection_;
    renamedSelection_.clear();
    int type = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole + 1).toInt() : 0;
    const int scrollPosition = list_->verticalScrollBar()->value();
    const QSignalBlocker blocker(list_);
    list_->clear();
    auto add = [&](QString name, QString text, int kind) {
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, name); item->setData(Qt::UserRole + 1, kind);
        auto* row = new QWidget(list_);
        auto* layout = new QHBoxLayout(row); layout->setContentsMargins(5, 1, 5, 1);
        auto* label = new QLabel(text, row);
        layout->addWidget(label, 1);
        if (!devices_ && kind == 0) {
            auto* favorite = new QToolButton(row);
            favorite->setText(name.toStdString() == config_.initial_mode ? "★" : "☆");
            favorite->setToolTip("Set as default mode");
            favorite->setAccessibleName("Set " + name + " as default mode");
            favorite->setAutoRaise(true);
            layout->addWidget(favorite);
            connect(favorite, &QToolButton::clicked, this, [this, name] { setStartupMode(name); });
        }
        auto* trash = new QToolButton(row);
        trash->setText("🗑"); trash->setToolTip("Remove " + name);
        trash->setAccessibleName("Remove " + name);
        trash->setAutoRaise(true);
        layout->addWidget(trash);
        connect(trash, &QToolButton::clicked, this, [this, name, kind] { remove(name, kind); });
        item->setSizeHint(row->sizeHint());
        list_->setItemWidget(item, row);
        if (name == selected && kind == type) list_->setCurrentItem(item);
    };
    auto heading = [&](const QString& text, const QString& action, int kind) {
        auto* item = new QListWidgetItem(list_);
        item->setFlags(Qt::NoItemFlags);
        auto* row = new QFrame(list_);
        row->setAutoFillBackground(true);
        auto palette = row->palette();
        palette.setColor(QPalette::Window, palette.color(QPalette::AlternateBase));
        row->setPalette(palette);
        auto* layout = new QHBoxLayout(row); layout->setContentsMargins(5, 3, 5, 3);
        auto* title = new QLabel(text, row);
        auto font = title->font(); font.setBold(true); title->setFont(font);
        layout->addWidget(title, 1);
        auto* plus = new QToolButton(row);
        plus->setText("+"); plus->setToolTip(action); plus->setAccessibleName(action);
        plus->setAutoRaise(true);
        layout->addWidget(plus);
        connect(plus, &QToolButton::clicked, this, [this, kind] { this->add(kind); });
        item->setSizeHint(row->sizeHint());
        list_->setItemWidget(item, row);
    };
    if (devices_) {
        for (auto kind : {DeviceKind::Evdev, DeviceKind::Uinput}) {
            heading(kind == DeviceKind::Evdev ? "CONTROLLERS" : "VIRTUAL JOYSTICKS",
                    kind == DeviceKind::Evdev ? "Add controller" : "Add virtual joystick", kind == DeviceKind::Uinput ? 1 : 0);
            for (const auto& [name, device] : config_.devices)
                if (device.kind == kind) add(qs(name), qs(name) + (device.virtual_name.empty() ? "" : " · " + qs(device.virtual_name)), kind == DeviceKind::Uinput ? 1 : 0);
        }
    } else {
        heading("MODES", "Add persistent mode", 0);
        for (const auto& mode : config_.modes) add(qs(mode), qs(mode) + (mode == config_.initial_mode ? " · Default mode" : " · Persistent mode"), 0);
        heading("MODIFIERS", "Add held modifier", 1);
        for (const auto& [name, input] : config_.modifiers)
            add(qs(name), qs(name) + " · While " + labeledInput(config_, input) + " is held", 1);
    }
    if (!list_->currentItem()) {
        for (int i = 0; i < list_->count(); ++i)
            if (list_->item(i)->flags() & Qt::ItemIsSelectable) { list_->setCurrentRow(i); break; }
    }
    list_->verticalScrollBar()->setValue(scrollPosition);
    select();
}

bool SetupWorkspace::commit(Config config, bool refreshAfter, QString renamedKey) {
    try {
        // Setup can be used to repair an already-invalid mapping. Validate fields
        // here; profile-wide issues remain navigable in the shared issue panel.
        Config fields = config; fields.bindings.clear();
        validate_edited_config(fields);
        if (error) error(QString{});
        if (committed) committed(std::move(config));
        if (!renamedKey.isEmpty()) renamedSelection_ = std::move(renamedKey);
        if (refreshAfter) QTimer::singleShot(0, this, [this] { refresh(); });
        return true;
    } catch (const ConfigError& failure) { if (error) error(qs(failure.what())); return false; }
}

void SetupWorkspace::select() {
    const auto* selectedItem = list_->currentItem();
    const QString nextKey = selectedItem ? selectedItem->data(Qt::UserRole).toString() : QString{};
    const int nextKind = selectedItem ? selectedItem->data(Qt::UserRole + 1).toInt() : 0;
    const int usageScroll = nextKey == usageKey_ && nextKind == usageKind_ ? usageScroll_ : 0;
    clearDetail();
    auto* current = list_->currentItem();
    if (!current || !current->data(Qt::UserRole).isValid()) return;
    const auto key = current->data(Qt::UserRole).toString().toStdString();
    const bool modifier = current->data(Qt::UserRole + 1).toInt() == 1;
    auto commitForm = [this](Config next, bool refreshAfter) { return commit(std::move(next), refreshAfter); };
    if (devices_) {
        if (!config_.devices.contains(key)) return;
        auto commitDevice = [this](Config next, bool refreshAfter, QString renamedKey) {
            return commit(std::move(next), refreshAfter, std::move(renamedKey));
        };
        QWidget* deviceForm = modifier
            ? static_cast<QWidget*>(new VirtualDeviceForm(config_, key, commitDevice,
                [this](QString message) { if (error) error(std::move(message)); }, properties_))
            : static_cast<QWidget*>(new PhysicalDeviceForm(config_, key, commitDevice, properties_));
        detail_->addWidget(deviceForm);
        deviceForm->show();
        return;
    }
    QWidget* fields = modifier
        ? static_cast<QWidget*>(new ModifierForm(config_, key, commitForm,
            [this](Control input) { if (showControl) showControl(input); }, properties_))
        : static_cast<QWidget*>(new ModeForm(config_, key, commitForm, properties_));
    detail_->addWidget(fields);
    fields->show();
    showUsage(key, modifier, usageScroll);
}

void SetupWorkspace::clearDetail() {
    while (auto* item = detail_->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->disconnect(this);
            // Forms use themselves as the receiver for edit callbacks. Disconnect
            // only those and the workspace callbacks while deleteLater is pending;
            // wildcard disconnect() also severs Qt's internal destroyed signals.
            for (auto* child : widget->findChildren<QObject*>()) {
                child->disconnect(this);
                child->disconnect(widget);
                child->setObjectName({});
            }
            widget->setObjectName({}); widget->deleteLater();
        }
        delete item;
    }
}

void SetupWorkspace::showUsage(const std::string& key, bool modifier, int scrollPosition) {
    auto* table = new QTableWidget(properties_);
    table->setObjectName("setupUsage");
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({"Controller", "Input", "In modes", "While held", "Output"});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    auto joined = [](const std::vector<std::string>& values) {
        QStringList names; for (const auto& value : values) names << qs(value); return names.join(", ");
    };
    for (std::size_t i = 0; i < config_.bindings.size(); ++i) {
        const auto& binding = config_.bindings[i];
        const auto& conditions = modifier ? binding.modifiers : binding.modes;
        if (std::find(conditions.begin(), conditions.end(), key) == conditions.end()) continue;
        const int row = table->rowCount();
        table->insertRow(row);
        auto* first = new QTableWidgetItem(qs(binding.input.device));
        first->setData(Qt::UserRole, static_cast<int>(i));
        table->setItem(row, 0, first);
        table->setItem(row, 1, new QTableWidgetItem(labeledInput(config_, binding.input)));
        table->setItem(row, 2, new QTableWidgetItem(joined(binding.modes)));
        table->setItem(row, 3, new QTableWidgetItem(joined(binding.modifiers)));
        table->setItem(row, 4, new QTableWidgetItem(mapping_ui::actionSummary(binding)));
    }
    table->resizeColumnsToContents();
    connect(table, &QTableWidget::cellClicked, this, [this, table](int row) {
        const auto index = table->item(row, 0)->data(Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(config_.bindings.size()) && showMapping)
            showMapping(index);
    });
    auto* usageLabel = new QLabel(QString("Used by %1 mappings").arg(table->rowCount()), properties_);
    detail_->addWidget(usageLabel);
    usageLabel->show();
    detail_->addWidget(table, 1);
    table->show();
    connect(table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, key, modifier](int value) {
        if (usageHidden_) return;
        usageKey_ = qs(key);
        usageKind_ = modifier ? 1 : 0;
        usageScroll_ = value;
    });
    QTimer::singleShot(0, table, [table, scrollPosition] { table->verticalScrollBar()->setValue(scrollPosition); });
}

void SetupWorkspace::requestAdd(bool virtualDevice) { add(virtualDevice ? 1 : 0); }

void SetupWorkspace::setStartupMode(const QString& key) {
    Config next = config_;
    next.initial_mode = key.toStdString();
    if (next.initial_mode == config_.initial_mode) return;
    commit(std::move(next), true);
}

void SetupWorkspace::add(int kind) {
    bool ok = false;
    std::map<int, AxisRange> copiedAxes;
    QString enteredName;
    if (devices_ && kind == 1) {
        QDialog dialog(this);
        dialog.setWindowTitle("Add virtual joystick");
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* name = new QLineEdit(&dialog);
        name->setObjectName("newVirtualName");
        form->addRow("Profile name", name);
        auto* source = new QComboBox(&dialog);
        source->setObjectName("virtualSourceDevice");
        source->addItem("None (use defaults)");
        const auto detected = profile_setup::discover_devices();
        for (int i = 0; i < static_cast<int>(detected.size()); ++i)
            if (detected[i].issue.empty())
                source->addItem(qs(detected[i].name) + " · " + qs(detected[i].path), i);
        form->addRow("Base on controller", source);
        layout->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        auto* accept = buttons->button(QDialogButtonBox::Ok);
        accept->setEnabled(false);
        connect(name, &QLineEdit::textChanged, &dialog, [name, accept] {
            accept->setEnabled(!name->text().trimmed().isEmpty());
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            ok = true;
            enteredName = name->text().trimmed();
            if (source->currentIndex() > 0) copiedAxes = detected.at(source->currentData().toInt()).axis_ranges;
        }
    } else {
        QInputDialog dialog(this);
        dialog.setWindowTitle("Add");
        dialog.setLabelText("Profile name");
        dialog.setOkButtonText("OK");
        auto* accept = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        accept->setEnabled(false);
        connect(&dialog, &QInputDialog::textValueChanged, &dialog, [accept](const QString& text) {
            accept->setEnabled(!text.trimmed().isEmpty());
        });
        ok = dialog.exec() == QDialog::Accepted;
        enteredName = dialog.textValue().trimmed();
    }
    auto name = enteredName.toStdString();
    if (!ok || name.empty()) return;
    Config next = config_;
    if (devices_) {
        if (next.devices.contains(name)) { if (error) error("That device name is already used."); return; }
        Device device{kind ? DeviceKind::Uinput : DeviceKind::Evdev, kind ? "" : "/dev/input/by-id/choose-controller", true, kind ? "joystick" : ""};
        if (kind) {
            device.axes = joystick_axes();
            for (const auto& [code, range] : copiedAxes) device.axes.insert_or_assign(code, range);
        }
        next.devices.emplace(name, device);
    } else if (!kind) {
        if (std::find(next.modes.begin(), next.modes.end(), name) != next.modes.end()) { if (error) error("That mode already exists."); return; }
        next.modes.push_back(name);
    } else {
        if (next.modifiers.contains(name)) { if (error) error("That modifier already exists."); return; }
        auto physical = std::find_if(next.devices.begin(), next.devices.end(), [](const auto& entry) { return entry.second.kind == DeviceKind::Evdev; });
        if (physical == next.devices.end()) { if (error) error("Add a controller in Devices first."); return; }
        next.modifiers.emplace(name, Control{physical->first, ControlKind::Button, -1});
    }
    if (!commit(std::move(next), false)) return;
    refresh();
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == qs(name) && (list_->item(i)->flags() & Qt::ItemIsSelectable)) {
            list_->setCurrentRow(i);
            break;
        }
}

void SetupWorkspace::remove(const QString& name, int kind) {
    if (QMessageBox::question(this, "Remove " + name, "Remove '" + name + "' from this profile?",
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto key = name.toStdString();
    Config next = config_;
    try {
        if (devices_) profile_setup::remove_device(next, key);
        else if (kind) profile_setup::remove_modifier(next, key);
        else profile_setup::remove_mode(next, key);
    } catch (const ConfigError& failure) { if (error) error(qs(failure.what())); return; }
    commit(std::move(next), true);
}
