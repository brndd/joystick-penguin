#include "action_editor.hpp"
#include "button_selector.hpp"
#include "control_browser.hpp"

#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QSpinBox>

#include <type_traits>
#include <variant>

using namespace joystick_penguin;

namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
std::string str(const QString& value) { return value.toStdString(); }

int buttonNumber(int code) {
    for (int i = 1; i <= joystick_button_count; ++i)
        if (joystick_button_code(i) == code) return i;
    return 0;
}

void populate(QComboBox* combo, const Config& config) {
    combo->clear();
    for (const auto& [name, device] : config.devices)
        if (device.kind == DeviceKind::Uinput) combo->addItem(qs(name));
}

void selectName(QComboBox* combo, const std::string& value) {
    auto name = qs(value);
    if (combo->findText(name) < 0) combo->addItem(name);
    combo->setCurrentText(name);
}
} // namespace

ActionEditor::ActionEditor(const Config& config, ControlKind input, Action original, QWidget* parent)
    : QGroupBox("Action", parent), config_(config) {
    auto* form = new QFormLayout(this);
    type_ = new QComboBox(this);
    type_->setObjectName("actionType");
    if (input != ControlKind::AbsoluteAxis) type_->addItem("Virtual button", 0);
    if (input != ControlKind::AbsoluteAxis) type_->addItem("Virtual hat", 1);
    if (input == ControlKind::AbsoluteAxis) type_->addItem("Virtual axis", 2);
    if (input == ControlKind::Button) type_->addItem("Select mode", 3);
    form->addRow("Action", type_);
    device_ = new QComboBox(this);
    device_->setObjectName("actionDevice");
    populate(device_, config);
    form->addRow("Output device", device_);
    buttonFormat_ = new QComboBox(this);
    buttonFormat_->setObjectName("actionButtonFormat");
    buttonFormat_->addItems({"Button", "EV_KEY literal"});
    form->addRow("Button format", buttonFormat_);
    button_ = new ButtonSelector(config, ButtonSelector::Target::Virtual, this);
    button_->setObjectName("actionButton");
    form->addRow("Button", button_);
    buttonCode_ = new QSpinBox(this);
    buttonCode_->setObjectName("actionButtonCode");
    buttonCode_->setRange(BTN_MISC, KEY_MAX);
    form->addRow("EV_KEY code", buttonCode_);
    axis_ = new QSpinBox(this);
    axis_->setObjectName("actionAxis");
    axis_->setRange(0, ABS_MAX);
    form->addRow("Axis EV_ABS code", axis_);
    hatAxis_ = new QComboBox(this);
    for (int code = ABS_HAT0X; code <= ABS_HAT3Y; ++code)
        hatAxis_->addItem(inputName({"", ControlKind::AbsoluteAxis, code}), code);
    form->addRow("Hat component", hatAxis_);
    literalHat_ = new QCheckBox("EV_ABS literal", this);
    form->addRow(literalHat_);
    connect(hatAxis_, &QComboBox::currentIndexChanged, this, [this] { axis_->setValue(hatAxis_->currentData().toInt()); });
    connect(axis_, &QSpinBox::valueChanged, this, [this](int code) { hatAxis_->setCurrentIndex(hatAxis_->findData(code)); });
    connect(literalHat_, &QCheckBox::toggled, this, [this] { updateFields(); });
    outputAxis_ = new QComboBox(this);
    outputAxis_->setObjectName("actionOutputAxis");
    form->addRow("Declared output axis", outputAxis_);
    direction_ = new QComboBox(this);
    direction_->addItem("Negative (−1)", -1);
    direction_->addItem("Positive (+1)", 1);
    form->addRow("Hat direction", direction_);
    invert_ = new QCheckBox("Invert output axis", this);
    invert_->setObjectName("actionInvert");
    form->addRow(invert_);
    mode_ = new QComboBox(this);
    mode_->setObjectName("actionMode");
    for (const auto& name : config.modes) mode_->addItem(qs(name));
    form->addRow("Mode", mode_);
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { updateFields(); notifyChanged(); });
    connect(buttonFormat_, &QComboBox::currentIndexChanged, this, [this] { updateFields(); notifyChanged(); });
    connect(device_, &QComboBox::currentIndexChanged, this, [this] { updateAxes(); updateButtons(); notifyChanged(); });
    connect(button_, &QComboBox::currentIndexChanged, this, [this] { notifyChanged(); });
    connect(buttonCode_, &QSpinBox::valueChanged, this, [this] { notifyChanged(); });
    connect(axis_, &QSpinBox::valueChanged, this, [this] { notifyChanged(); });
    connect(hatAxis_, &QComboBox::currentIndexChanged, this, [this] { notifyChanged(); });
    connect(literalHat_, &QCheckBox::toggled, this, [this] { notifyChanged(); });
    connect(outputAxis_, &QComboBox::currentIndexChanged, this, [this] { notifyChanged(); });
    connect(direction_, &QComboBox::currentIndexChanged, this, [this] { notifyChanged(); });
    connect(invert_, &QCheckBox::toggled, this, [this] { notifyChanged(); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { notifyChanged(); });
    const int originalType = std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ButtonAction>) return 0;
        if constexpr (std::is_same_v<T, HatAction>) return 1;
        if constexpr (std::is_same_v<T, AxisAction>) return 2;
        return 3;
    }, original);
    type_->setCurrentIndex(type_->findData(originalType));
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ModeAction>) {
            selectName(mode_, value.mode);
        } else {
            selectName(device_, value.device);
            if constexpr (std::is_same_v<T, ButtonAction>) {
                int number = buttonNumber(value.code);
                buttonFormat_->setCurrentIndex(number ? 0 : 1);
                updateButtons();
                if (number) button_->setNumber(number);
                else buttonCode_->setValue(value.code);
            } else if constexpr (std::is_same_v<T, HatAction>) {
                axis_->setValue(value.code);
                direction_->setCurrentIndex(value.direction < 0 ? 0 : 1);
            } else {
                updateAxes();
                outputAxis_->setCurrentIndex(outputAxis_->findData(value.code));
                invert_->setChecked(value.invert);
            }
        }
    }, original);
    updateFields();
    updateButtons(true);
}

Action ActionEditor::result() const {
    const int type = type_->currentData().toInt();
    if (type == 3) return ModeAction{str(mode_->currentText())};
    const auto device = str(device_->currentText());
    if (type == 0)
        return ButtonAction{device, buttonFormat_->currentIndex() == 0
                                        ? joystick_button_code(button_->number()) : buttonCode_->value()};
    if (type == 1) return HatAction{device, axis_->value(), direction_->currentData().toInt()};
    return AxisAction{device, outputAxis_->currentData().toInt(), invert_->isChecked()};
}

void ActionEditor::notifyChanged() { if (changed) changed(); }

void ActionEditor::visible(QWidget* widget, bool show) {
    widget->setVisible(show);
    if (auto* label = qobject_cast<QFormLayout*>(layout())->labelForField(widget)) label->setVisible(show);
}

void ActionEditor::updateFields() {
    const int type = type_->currentData().toInt();
    visible(device_, type != 3);
    visible(buttonFormat_, type == 0);
    visible(button_, type == 0 && buttonFormat_->currentIndex() == 0);
    visible(buttonCode_, type == 0 && buttonFormat_->currentIndex() != 0);
    visible(axis_, type == 1 && literalHat_->isChecked());
    visible(hatAxis_, type == 1);
    visible(literalHat_, type == 1);
    visible(outputAxis_, type == 2);
    visible(direction_, type == 1);
    visible(invert_, type == 2);
    visible(mode_, type == 3);
    if (type == 1) axis_->setRange(ABS_HAT0X, ABS_HAT3Y);
    else axis_->setRange(0, ABS_MAX);
    updateAxes();
}

void ActionEditor::updateButtons(bool preserveMissing) {
    button_->setDevice(str(device_->currentText()), button_->number(), preserveMissing);
}

void ActionEditor::updateAxes() {
    const int previous = outputAxis_->currentData().toInt();
    outputAxis_->clear();
    auto found = config_.devices.find(str(device_->currentText()));
    if (found != config_.devices.end() && found->second.kind == DeviceKind::Uinput)
        for (const auto& [code, range] : found->second.axes) {
            (void)range;
            outputAxis_->addItem(inputName({"", ControlKind::AbsoluteAxis, code}) + QString(" · EV_ABS %1").arg(code), code);
        }
    const int index = outputAxis_->findData(previous);
    if (index >= 0) outputAxis_->setCurrentIndex(index);
}
