#include "physical_device_form.hpp"
#include "discovery.hpp"
#include "setup_model.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <algorithm>

using namespace joystick_penguin;
namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
}

PhysicalDeviceForm::PhysicalDeviceForm(const Config& config, std::string key, Commit commit, QWidget* parent)
    : QWidget(parent), config_(config), key_(std::move(key)), commit_(std::move(commit)) {
    const auto& device = config_.devices.at(key_);
    auto* form = new QFormLayout(this);
    // Stable profile identity and the physical device link may be edited offline.
    name_ = new QLineEdit(qs(key_), this);
    name_->setObjectName("setupName");
    form->addRow("Profile identifier", name_);
    path_ = new QLineEdit(qs(device.path), this);
    path_->setObjectName("controllerPath");
    form->addRow("Stable controller link", path_);
    // Detection is advisory; manually entered stable links remain usable.
    auto* discovered = new QComboBox(this);
    discovered->addItem("Choose detected controller…", QString{});
    form->addRow("Detected hardware", discovered);
    auto* availability = new QLabel(this);
    availability->setObjectName("controllerAvailability");
    availability->setWordWrap(true);
    availability->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    form->addRow("Availability", availability);
    auto updateAvailability = [this, discovered, availability] {
        const auto path = path_->text().trimmed().toStdString();
        QString access = "Offline / not detected";
        int matchedIndex = 0;
        const QSignalBlocker blocker(discovered);
        discovered->clear();
        discovered->addItem("Choose detected controller…", QString{});
        for (const auto& entry : profile_setup::discover_devices()) {
            discovered->addItem(qs(entry.name), qs(entry.path));
            discovered->setItemData(discovered->count() - 1, qs(entry.path), Qt::ToolTipRole);
            if (entry.path == path || std::find(entry.aliases.begin(), entry.aliases.end(), path) != entry.aliases.end()) {
                matchedIndex = discovered->count() - 1;
                access = qs(entry.name) + " · " + (entry.issue.empty() ? "Readable" : qs(entry.issue));
            }
        }
        discovered->setCurrentIndex(matchedIndex);
        availability->setText(access);
    };
    updateAvailability();
    connect(path_, &QLineEdit::textChanged, this, updateAvailability);
    auto* refresh = new QTimer(this);
    refresh->setInterval(1000);
    connect(refresh, &QTimer::timeout, this, [this, updateAvailability] {
        if (isVisible()) updateAvailability();
    });
    refresh->start();
    auto* grabRow = new QWidget(this);
    auto* grabLayout = new QHBoxLayout(grabRow);
    grabLayout->setContentsMargins(0, 0, 0, 0);
    grab_ = new QCheckBox("Exclusive mode", grabRow);
    grab_->setChecked(device.grab);
    auto* grabInfo = new QToolButton(grabRow);
    grabInfo->setText("ⓘ");
    grabInfo->setAutoRaise(true);
    grabLayout->addWidget(grab_);
    grabLayout->addWidget(grabInfo);
    grabLayout->addStretch();
    form->addRow(grabRow);
    connect(grabInfo, &QToolButton::clicked, this, [grabInfo] {
        QToolTip::showText(grabInfo->mapToGlobal(grabInfo->rect().bottomLeft()),
            "<table width='320'><tr><td>Exclusive mode prevents other applications from reading the physical controller directly.</td></tr></table>", grabInfo);
    });
    // Each proposal starts from the current document, including earlier commits.
    connect(path_, &QLineEdit::editingFinished, this, [this] {
        auto next = proposed();
        if (next.devices.at(key_) != config_.devices.at(key_)) commit_(std::move(next), false, {});
    });
    connect(grab_, &QCheckBox::toggled, this, [this] { commit_(proposed(), false, {}); });
    connect(discovered, &QComboBox::currentIndexChanged, this, [this, discovered] {
        if (discovered->currentData().toString().isEmpty()) return;
        path_->setText(discovered->currentData().toString());
        commit_(proposed(), false, {});
    });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const auto newName = name_->text().trimmed().toStdString();
        if (newName == key_) return;
        auto next = proposed();
        profile_setup::rename_device(next, key_, newName);
        if (commit_(std::move(next), true, qs(newName))) setEnabled(false);
    });
}

Config PhysicalDeviceForm::proposed() const {
    Config next = config_;
    auto& device = next.devices.at(key_);
    device.path = path_->text().trimmed().toStdString();
    device.grab = grab_->isChecked();
    return next;
}
