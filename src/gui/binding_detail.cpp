#include "binding_detail.hpp"
#include "action_list.hpp"
#include "button_selector.hpp"
#include "control_browser.hpp"
#include "mapping_model.hpp"

#include <linux/input-event-codes.h>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSpinBox>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <variant>

using namespace joystick_penguin;
using namespace mapping_ui;

namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
std::string str(const QString& value) { return value.toStdString(); }

void showInfo(QWidget* anchor, const QString& text) {
    QToolTip::showText(anchor->mapToGlobal(anchor->rect().bottomLeft()),
                       "<table width='320'><tr><td>" + text.toHtmlEscaped() + "</td></tr></table>", anchor);
}

std::vector<std::string> checkedNames(QListWidget* list) {
    std::vector<std::string> values;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) values.push_back(str(list->item(i)->text()));
    return values;
}

void populateChecks(QListWidget* list, const std::vector<std::string>& available,
                    const std::vector<std::string>& selected) {
    list->clear();
    for (const auto& name : available) {
        auto* item = new QListWidgetItem(qs(name), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(std::find(selected.begin(), selected.end(), name) != selected.end()
                                ? Qt::Checked : Qt::Unchecked);
    }
    for (const auto& name : selected)
        if (std::find(available.begin(), available.end(), name) == available.end()) {
            auto* item = new QListWidgetItem(qs(name), list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
    if (auto* search = list->parentWidget()->findChild<QLineEdit*>())
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(search->text(), Qt::CaseInsensitive));
}
}

BindingDetail::BindingDetail(ProfileDocument& document, QWidget* parent) : QWidget(parent), document_(document) {
    auto* right = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::PlainText);
    right->addWidget(summary_);

    buildInputFields(right);
    buildConditions(right);
    buildTiming(right);
    actionList_ = new ActionList(document_, this);
    actionList_->edited = [this] { notifyEdited(); };
    right->addWidget(actionList_);
    issue_ = new QLabel(this);
    issue_->setObjectName("detailError");
    issue_->setWordWrap(true);
    right->addWidget(issue_);
    connectEdits();
}

void BindingDetail::buildInputFields(QVBoxLayout* right) {
    auto* inputDetails = new QToolButton(this);
    inputDetails->setText("Change input / advanced identifiers");
    inputDetails->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    inputDetails->setArrowType(Qt::RightArrow);
    inputDetails->setCheckable(true); inputDetails->setChecked(false);
    auto* inputFields = new QWidget(this);
    inputForm_ = new QFormLayout;
    inputDevice_ = new QComboBox(this);
    inputDevice_->setObjectName("inputDevice");
    inputForm_->addRow("Physical device", inputDevice_);
    inputKind_ = new QComboBox(this);
    inputKind_->setObjectName("inputKind");
    inputKind_->addItems({"Button", "Absolute axis", "Hat direction"});
    inputForm_->addRow("Input type", inputKind_);
    buttonFormat_ = new QComboBox(this);
    buttonFormat_->setObjectName("inputButtonFormat");
    buttonFormat_->addItems({"Button", "EV_KEY literal"});
    inputForm_->addRow("Button format", buttonFormat_);
    inputButton_ = new ButtonSelector(document_.config(), ButtonSelector::Target::Physical, this);
    inputButton_->setObjectName("inputButton");
    inputForm_->addRow("Button", inputButton_);
    inputButtonCode_ = new QSpinBox(this);
    inputButtonCode_->setObjectName("inputButtonCode");
    inputButtonCode_->setRange(BTN_MISC, KEY_MAX);
    inputForm_->addRow("EV_KEY code", inputButtonCode_);
    inputAxis_ = new QComboBox(this);
    inputAxis_->setObjectName("inputAxis");
    inputForm_->addRow("Axis EV_ABS code", inputAxis_);
    hatDirection_ = new QComboBox(this);
    hatDirection_->addItem("Negative (−1)", -1);
    hatDirection_->addItem("Positive (+1)", 1);
    inputForm_->addRow("Hat direction", hatDirection_);
    inputFields->setLayout(inputForm_);
    inputFields->hide();
    connect(inputDetails, &QToolButton::toggled, inputFields, &QWidget::setVisible);
    connect(inputDetails, &QToolButton::toggled, this, [inputDetails](bool checked) {
        inputDetails->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
    right->addWidget(inputDetails);
    right->addWidget(inputFields);
}

void BindingDetail::buildConditions(QVBoxLayout* right) {
    auto* selectors = new QHBoxLayout;
    auto* modesBox = new QWidget(this);
    auto* modesLayout = new QVBoxLayout(modesBox);
    modesLayout->setContentsMargins(0, 0, 0, 0);
    modesLayout->addWidget(new QLabel("In modes", modesBox));
    modes_ = new QListWidget(modesBox);
    modes_->setObjectName("bindingModes");
    modes_->setFixedHeight(5 * (modes_->fontMetrics().height() + 7) + 2 * modes_->frameWidth());
    modesLayout->addWidget(modes_);
    auto searchable = [this](QListWidget* list, QVBoxLayout* layout, const QString& label) {
        auto* row = new QWidget(list->parentWidget());
        auto* fields = new QHBoxLayout(row); fields->setContentsMargins(0, 0, 0, 0);
        auto* search = new QLineEdit(row);
        search->setPlaceholderText("Find " + label + "…");
        search->setAccessibleName("Find " + label);
        fields->addWidget(search);
        layout->insertWidget(1, row);
        connect(search, &QLineEdit::textChanged, list, [list](const QString& text) {
            for (int i = 0; i < list->count(); ++i)
                list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
        });
    };
    searchable(modes_, modesLayout, "modes");
    auto* modifiersBox = new QWidget(this);
    auto* modifiersLayout = new QVBoxLayout(modifiersBox);
    modifiersLayout->setContentsMargins(0, 0, 0, 0);
    modifiersLayout->addWidget(new QLabel("While held", modifiersBox));
    modifiers_ = new QListWidget(modifiersBox);
    modifiers_->setObjectName("bindingModifiers");
    modifiers_->setFixedHeight(5 * (modifiers_->fontMetrics().height() + 7) + 2 * modifiers_->frameWidth());
    modifiersLayout->addWidget(modifiers_);
    searchable(modifiers_, modifiersLayout, "modifiers");
    selectors->addWidget(modesBox);
    selectors->addWidget(modifiersBox);
    right->addLayout(selectors);
}

void BindingDetail::buildTiming(QVBoxLayout* right) {
    timed_ = new QCheckBox("Tap / hold (button inputs only)", this);
    timed_->setObjectName("tapHold");
    right->addWidget(timed_);
    timingRow_ = new QWidget(this);
    auto* timing = new QHBoxLayout(timingRow_);
    timing->setContentsMargins(0, 0, 0, 0);
    threshold_ = new QSpinBox(this);
    threshold_->setObjectName("holdThreshold");
    threshold_->setRange(1, std::numeric_limits<int>::max());
    threshold_->setSuffix(" ms threshold");
    threshold_->setFixedWidth(threshold_->fontMetrics().horizontalAdvance("999999 ms threshold") + 35);
    tapMs_ = new QSpinBox(this);
    tapMs_->setObjectName("tapDuration");
    tapMs_->setRange(1, std::numeric_limits<int>::max());
    tapMs_->setSuffix(" ms tap pulse");
    tapMs_->setFixedWidth(tapMs_->fontMetrics().horizontalAdvance("999999 ms tap pulse") + 35);
    timing->addWidget(threshold_);
    auto* timingInfo = new QToolButton(this);
    timingInfo->setText("ⓘ"); timingInfo->setAutoRaise(true); timingInfo->setAccessibleName("About tap and hold timing");
    timing->addWidget(timingInfo);
    connect(timingInfo, &QToolButton::clicked, this, [timingInfo] {
        showInfo(timingInfo, "Release before the threshold to pulse Tap outputs. Hold past the threshold to activate Hold outputs until release. Either branch can be empty.");
    });
    timing->addWidget(tapMs_);
    auto* tapInfo = new QToolButton(this);
    tapInfo->setText("ⓘ"); tapInfo->setAutoRaise(true); tapInfo->setAccessibleName("About tap pulse duration");
    timing->addWidget(tapInfo);
    connect(tapInfo, &QToolButton::clicked, this, [tapInfo] {
        showInfo(tapInfo, "How long Tap outputs remain active after release before being reset.");
    });
    timing->addStretch();
    right->addWidget(timingRow_);
}

void BindingDetail::connectEdits() {
    // Form population is guarded by filling_ so these signals only record edits.
    connect(inputDevice_, &QComboBox::currentIndexChanged, this, [this] {
        if (filling_) return;
        inputButton_->setDevice(str(inputDevice_->currentText()), inputButton_->number());
        inputChanged();
    });
    connect(inputKind_, &QComboBox::currentIndexChanged, this, [this] { inputKindChanged(); });
    connect(buttonFormat_, &QComboBox::currentIndexChanged, this, [this] {
        if (filling_) return;
        showInputField(inputButton_, buttonFormat_->currentIndex() == 0);
        showInputField(inputButtonCode_, buttonFormat_->currentIndex() != 0);
        inputChanged();
    });
    connect(inputButton_, &QComboBox::currentIndexChanged, this, [this] { inputChanged(); });
    connect(inputButtonCode_, &QSpinBox::valueChanged, this, [this] { inputChanged(); });
    connect(inputAxis_, &QComboBox::currentIndexChanged, this, [this] { inputChanged(); });
    connect(hatDirection_, &QComboBox::currentIndexChanged, this, [this] { inputChanged(); });
    connect(modes_, &QListWidget::itemChanged, this, [this] {
        if (filling_ || selected_ < 0) return;
        if (document_.editMapping(selected_, [this](Binding& binding) { binding.modes = checkedNames(modes_); })) notifyEdited();
    });
    connect(modifiers_, &QListWidget::itemChanged, this, [this] {
        if (filling_ || selected_ < 0) return;
        if (document_.editMapping(selected_, [this](Binding& binding) { binding.modifiers = checkedNames(modifiers_); })) notifyEdited();
    });
    connect(timed_, &QCheckBox::toggled, this, [this](bool checked) {
        if (filling_ || selected_ < 0) return;
        const auto& binding = document_.config().bindings.at(selected_);
        if (checked) {
            document_.editMapping(selected_, [](Binding& edited) {
                edited.tap_hold = TapHold{200, 50, {}, edited.actions};
                edited.actions.clear();
            });
        } else {
            if (!binding.tap_hold->tap.empty() && !binding.tap_hold->hold.empty()) {
                filling_ = true;
                timed_->setChecked(true);
                filling_ = false;
                QMessageBox::information(this, "Keep tap and hold actions",
                    "Both branches have actions. Remove one branch before changing to immediate actions.");
                return;
            }
            document_.editMapping(selected_, [](Binding& edited) {
                edited.actions = edited.tap_hold->hold.empty() ? edited.tap_hold->tap : edited.tap_hold->hold;
                edited.tap_hold.reset();
            });
        }
        actionList_->closeEditor();
        refresh();
        notifyEdited();
    });
    auto timingChanged = [this] {
        if (filling_ || selected_ < 0 || !document_.config().bindings.at(selected_).tap_hold) return;
        if (document_.editMapping(selected_, [this](Binding& binding) {
            binding.tap_hold->threshold_ms = threshold_->value();
            binding.tap_hold->tap_ms = tapMs_->value();
        })) notifyEdited();
    };
    connect(threshold_, &QSpinBox::valueChanged, this, timingChanged);
    connect(tapMs_, &QSpinBox::valueChanged, this, timingChanged);
}

void BindingDetail::notifyEdited() {
    updateSummary();
    if (edited) edited();
}

void BindingDetail::selectBinding(int row) {
    if (selected_ != row) actionList_->clearSelection();
    selected_ = row;
    actionList_->selectBinding(row);
    setVisible(row >= 0);
    refresh();
}

void BindingDetail::updateSummary() {
    const auto& config = document_.config();
    if (selected_ < 0 || selected_ >= static_cast<int>(config.bindings.size())) return;
    const auto& binding = config.bindings[selected_];
    summary_->setText(labeledInput(config, binding.input) + " · " + qs(binding.input.device) + "\n" + actionSummary(binding));
}

void BindingDetail::setIssue(const QString& message) { issue_->setText(message); }
void BindingDetail::closeActionEditor() { actionList_->closeEditor(); }

void BindingDetail::showInputField(QWidget* field, bool show) {
    field->setVisible(show);
    if (auto* label = inputForm_->labelForField(field)) label->setVisible(show);
}

void BindingDetail::updateAxisCodes() {
    inputAxis_->clear();
    if (inputKind_->currentIndex() == 2) {
        for (int code = ABS_HAT0X; code <= ABS_HAT3Y; ++code)
            inputAxis_->addItem(inputName({"", ControlKind::AbsoluteAxis, code}) + QString(" · EV_ABS %1").arg(code), code);
    } else {
        for (int code = 0; code <= ABS_MAX; ++code)
            if (code < ABS_HAT0X || code > ABS_HAT3Y)
                inputAxis_->addItem(inputName({"", ControlKind::AbsoluteAxis, code}) + QString(" · EV_ABS %1").arg(code), code);
    }
}

void BindingDetail::inputChanged() {
    if (filling_ || selected_ < 0) return;
    Control input = document_.config().bindings.at(selected_).input;
    input.device = str(inputDevice_->currentText());
    switch (inputKind_->currentIndex()) {
    case 0:
        if (buttonFormat_->currentIndex() == 0 && inputButton_->number() <= 0) return;
        input.kind = ControlKind::Button;
        input.code = buttonFormat_->currentIndex() ? inputButtonCode_->value() : -inputButton_->number();
        input.direction = 0;
        break;
    case 1:
        input.kind = ControlKind::AbsoluteAxis;
        input.code = inputAxis_->currentData().toInt();
        input.direction = 0;
        break;
    default:
        input.kind = ControlKind::HatDirection;
        input.code = inputAxis_->currentData().toInt();
        input.direction = hatDirection_->currentData().toInt();
    }
    if (inputChanging) inputChanging(input);
    if (document_.editMapping(selected_, [&](Binding& binding) { binding.input = input; })) notifyEdited();
}

void BindingDetail::inputKindChanged() {
    if (filling_) return;
    if (selected_ >= 0) {
        const auto& binding = document_.config().bindings.at(selected_);
        const auto kind = inputKind_->currentIndex() == 0 ? ControlKind::Button :
                          inputKind_->currentIndex() == 1 ? ControlKind::AbsoluteAxis : ControlKind::HatDirection;
        auto incompatible = [kind](const Action& action) {
            if (kind == ControlKind::AbsoluteAxis) return !std::holds_alternative<AxisAction>(action);
            return std::holds_alternative<AxisAction>(action) || (kind == ControlKind::HatDirection && std::holds_alternative<ModeAction>(action));
        };
        const bool removeTiming = binding.tap_hold && kind != ControlKind::Button;
        if (removeTiming || std::any_of(binding.actions.begin(), binding.actions.end(), incompatible)) {
            if (QMessageBox::question(this, "Change input behavior", "This input cannot use some existing actions. Remove incompatible actions and, if present, tap/hold timing and its branches?", QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                refresh(); return;
            }
            actionList_->closeEditor();
            document_.editMapping(selected_, [&](Binding& edited) {
                std::erase_if(edited.actions, incompatible);
                if (removeTiming) edited.tap_hold.reset();
            });
        }
    }
    actionList_->clearSelection();
    filling_ = true;
    updateAxisCodes();
    filling_ = false;
    showInputField(inputAxis_, inputKind_->currentIndex() != 0);
    showInputField(hatDirection_, inputKind_->currentIndex() == 2);
    showInputField(buttonFormat_, inputKind_->currentIndex() == 0);
    showInputField(inputButton_, inputKind_->currentIndex() == 0 && buttonFormat_->currentIndex() == 0);
    showInputField(inputButtonCode_, inputKind_->currentIndex() == 0 && buttonFormat_->currentIndex() != 0);
    inputChanged();
    refresh();
}

void BindingDetail::refresh() {
    const auto& config = document_.config();
    if (selected_ < 0 || selected_ >= static_cast<int>(config.bindings.size())) return;
    filling_ = true;
    const auto& binding = config.bindings.at(selected_);
    updateSummary();
    inputDevice_->clear();
    for (const auto& [name, device] : config.devices)
        if (device.kind == DeviceKind::Evdev) inputDevice_->addItem(qs(name));
    const auto name = qs(binding.input.device);
    if (inputDevice_->findText(name) < 0) inputDevice_->addItem(name);
    inputDevice_->setCurrentText(name);
    const int kind = binding.input.kind == ControlKind::Button ? 0 :
                     binding.input.kind == ControlKind::AbsoluteAxis ? 1 : 2;
    inputKind_->setCurrentIndex(kind);
    const bool indexed = binding.input.code < 0 && kind == 0;
    buttonFormat_->setCurrentIndex(indexed ? 0 : 1);
    inputButton_->setDevice(binding.input.device, indexed ? -binding.input.code : 1, indexed);
    if (kind == 0 && !indexed) inputButtonCode_->setValue(binding.input.code);
    updateAxisCodes();
    if (kind != 0) inputAxis_->setCurrentIndex(inputAxis_->findData(binding.input.code));
    hatDirection_->setCurrentIndex(binding.input.direction < 0 ? 0 : 1);
    showInputField(buttonFormat_, kind == 0);
    showInputField(inputButton_, kind == 0 && indexed);
    showInputField(inputButtonCode_, kind == 0 && !indexed);
    showInputField(inputAxis_, kind != 0);
    showInputField(hatDirection_, kind == 2);
    populateChecks(modes_, config.modes, binding.modes);
    std::vector<std::string> modifiers;
    for (const auto& [name, control] : config.modifiers) {
        (void)control;
        modifiers.push_back(name);
    }
    populateChecks(modifiers_, modifiers, binding.modifiers);
    timed_->setEnabled(kind == 0);
    timed_->setChecked(binding.tap_hold.has_value());
    threshold_->setEnabled(binding.tap_hold.has_value());
    timingRow_->setVisible(binding.tap_hold.has_value());
    tapMs_->setEnabled(binding.tap_hold.has_value());
    threshold_->setValue(binding.tap_hold ? binding.tap_hold->threshold_ms : 200);
    tapMs_->setValue(binding.tap_hold ? binding.tap_hold->tap_ms : 50);
    actionList_->refresh();
    filling_ = false;
}
