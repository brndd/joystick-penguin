#include "condition_forms.hpp"
#include "button_selector.hpp"
#include "control_browser.hpp"
#include "setup_model.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
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
    auto* form = new QFormLayout(this);
    // Each assigned button activates the same named modifier.
    name_ = new QLineEdit(qs(key_), this);
    name_->setObjectName("setupName");
    form->addRow("Modifier name", name_);
    auto* buttons = new QWidget(this);
    auto* list = new QVBoxLayout(buttons);
    list->setContentsMargins(0, 0, 0, 0);
    const auto& inputs = config_.modifiers.at(key_);
    if (inputs.empty()) list->addWidget(new QLabel("Unassigned — add a button to activate this modifier", buttons));
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs[i];
        auto* row = new QWidget(buttons);
        auto* fields = new QHBoxLayout(row);
        fields->setContentsMargins(0, 0, 0, 0);
        auto* controller = new QComboBox(row);
        for (const auto& [id, device] : config_.devices)
            if (device.kind == DeviceKind::Evdev) controller->addItem(qs(id));
        if (controller->findText(qs(input.device)) < 0) controller->addItem(qs(input.device));
        controller->setCurrentText(qs(input.device));
        auto* format = new QComboBox(row);
        format->setObjectName("modifierButtonFormat");
        format->addItems({"Button", "EV_KEY literal"});
        format->setCurrentIndex(input.code < 0 ? 0 : 1);
        auto* button = new ButtonSelector(config_, ButtonSelector::Target::Physical, row);
        button->setObjectName("modifierButton");
        button->setDevice(input.device, input.code < 0 ? -input.code : 1, input.code < 0);
        auto* code = new QSpinBox(row);
        code->setRange(BTN_MISC, KEY_MAX);
        if (input.code > 0) code->setValue(input.code);
        code->setObjectName("modifierButtonCode");
        button->setVisible(input.code < 0);
        code->setVisible(input.code > 0);
        auto* usage = new QPushButton("↩ Show control", row);
        auto* remove = new QPushButton("Remove button", row);
        fields->addWidget(controller);
        fields->addWidget(format);
        fields->addWidget(button);
        fields->addWidget(code);
        fields->addWidget(usage);
        fields->addWidget(remove);
        list->addWidget(row);
        rows_.push_back({controller, format, button, code});
        connect(format, &QComboBox::currentIndexChanged, this, [this, format, button, code] {
            button->setVisible(format->currentIndex() == 0);
            code->setVisible(format->currentIndex() != 0);
            commit_(proposed(), false);
        });
        connect(controller, &QComboBox::currentIndexChanged, this, [this, controller, button] {
            button->setDevice(controller->currentText().toStdString(), button->number());
            if (button->number() > 0) commit_(proposed(), false);
        });
        connect(button, &QComboBox::currentIndexChanged, this, [this] { commit_(proposed(), false); });
        connect(code, &QSpinBox::valueChanged, this, [this] { commit_(proposed(), false); });
        connect(usage, &QPushButton::clicked, this, [this, i, showControl] {
            if (showControl) showControl(proposed().modifiers.at(key_).at(i));
        });
        connect(remove, &QPushButton::clicked, this, [this, i] {
            auto next = proposed();
            next.modifiers.at(key_).erase(next.modifiers.at(key_).begin() + i);
            if (commit_(std::move(next), true)) setEnabled(false);
        });
    }
    form->addRow("Assigned buttons", buttons);
    auto* add = new QPushButton("Add button", this);
    add->setEnabled(std::any_of(config_.devices.begin(), config_.devices.end(), [](const auto& entry) {
        return entry.second.kind == DeviceKind::Evdev;
    }));
    form->addRow(add);
    connect(add, &QPushButton::clicked, this, [this] {
        auto next = proposed();
        const auto device = std::find_if(next.devices.begin(), next.devices.end(), [](const auto& entry) {
            return entry.second.kind == DeviceKind::Evdev;
        })->first;
        auto& inputs = next.modifiers.at(key_);
        int code = -1;
        while (std::any_of(inputs.begin(), inputs.end(), [&](const Control& input) {
            return input.device == device && input.code == code;
        })) --code;
        if (code < -255) return;
        inputs.push_back({device, ControlKind::Button, code});
        if (commit_(std::move(next), true)) setEnabled(false);
    });
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
    auto& inputs = next.modifiers.at(key_);
    inputs.clear();
    for (const auto& row : rows_)
        inputs.push_back({row.controller->currentText().toStdString(), ControlKind::Button,
                          row.format->currentIndex() ? row.code->value() : -row.button->number()});
    return next;
}
