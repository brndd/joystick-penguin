#include "condition_forms.hpp"
#include "control_browser.hpp"
#include "setup_model.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <linux/input-event-codes.h>

using namespace joystick_penguin;
namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
}

ModeForm::ModeForm(const Config& config, std::string key, SetupCommit commit, QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    auto* name = new QLineEdit(qs(key), this);
    name->setObjectName("setupName");
    form->addRow("Mode name", name);
    connect(name, &QLineEdit::editingFinished, this, [name, config = &config, key = std::move(key), commit = std::move(commit), this] {
        const auto newName = name->text().trimmed().toStdString();
        if (newName == key) return;
        Config next = *config;
        profile_setup::rename_mode(next, key, newName);
        if (commit(std::move(next), true)) setEnabled(false);
    });
}

ModifierForm::ModifierForm(const Config& config, std::string key, SetupCommit commit,
                           std::function<void(Control)> showControl, QWidget* parent)
    : QWidget(parent), config_(config), key_(std::move(key)), commit_(std::move(commit)) {
    const auto input = config_.modifiers.at(key_);
    auto* form = new QFormLayout(this);
    // A modifier is a named physical button; format selects index or EV_KEY code.
    name_ = new QLineEdit(qs(key_), this);
    name_->setObjectName("setupName");
    form->addRow("Modifier name", name_);
    controller_ = new QComboBox(this);
    for (const auto& [id, device] : config_.devices)
        if (device.kind == DeviceKind::Evdev) controller_->addItem(qs(id));
    controller_->setCurrentText(qs(input.device));
    form->addRow("Controller", controller_);
    format_ = new QComboBox(this);
    format_->addItems({"Button number", "EV_KEY literal"});
    format_->setCurrentIndex(input.code < 0 ? 0 : 1);
    form->addRow("Input identity", format_);
    code_ = new QSpinBox(this);
    code_->setRange(input.code < 0 ? 1 : BTN_MISC, input.code < 0 ? 255 : KEY_MAX);
    code_->setValue(input.code < 0 ? -input.code : input.code);
    code_->setObjectName("modifierButton");
    form->addRow("Button", code_);
    connect(format_, &QComboBox::currentIndexChanged, code_, [this] {
        code_->setRange(format_->currentIndex() ? BTN_MISC : 1, format_->currentIndex() ? KEY_MAX : 255);
    });
    form->addRow("Label", new QLabel(labeledInput(config_, input), this));
    auto* usage = new QPushButton("↩ Show physical control and mappings", this);
    auto* usageRow = new QHBoxLayout;
    usageRow->addWidget(usage); usageRow->addStretch();
    form->addRow(usageRow);
    connect(usage, &QPushButton::clicked, this, [this, showControl = std::move(showControl)] {
        if (showControl) showControl(config_.modifiers.at(key_));
    });
    connect(controller_, &QComboBox::currentIndexChanged, this, [this] { commit_(proposed(), false); });
    connect(format_, &QComboBox::currentIndexChanged, this, [this] { commit_(proposed(), false); });
    connect(code_, &QSpinBox::valueChanged, this, [this] { commit_(proposed(), false); });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const auto newName = name_->text().trimmed().toStdString();
        if (newName == key_) return;
        auto next = proposed();
        profile_setup::rename_modifier(next, key_, newName);
        if (commit_(std::move(next), true)) setEnabled(false);
    });
}

Config ModifierForm::proposed() const {
    Config next = config_;
    next.modifiers.at(key_) = {controller_->currentText().toStdString(), ControlKind::Button,
                               format_->currentIndex() ? code_->value() : -code_->value()};
    return next;
}
