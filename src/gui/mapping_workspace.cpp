#include "mapping_workspace.hpp"
#include "binding_detail.hpp"
#include "control_browser.hpp"
#include "mapping_model.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>

using namespace joystick_penguin;
using namespace mapping_ui;

namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
std::string str(const QString& value) { return value.toStdString(); }
QComboBox* filterCombo(QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItem("All");
    return combo;
}
}

MappingWorkspace::MappingWorkspace(ProfileDocument& document, QWidget* parent)
    : QWidget(parent), document_(document), config_(document.config()) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* outer = new QSplitter(this);
    layout->addWidget(outer);
    controls_ = new ControlBrowser(config_, outer);
    rightStack_ = new QStackedWidget(outer);
    outer->addWidget(controls_); outer->addWidget(rightStack_);
    outer->setSizes({300, 850});

    // Browsing: search, filters, and the source-model-backed mapping table.
    normalPage_ = new QSplitter(Qt::Vertical, rightStack_);
    auto* splitter = static_cast<QSplitter*>(normalPage_);
    rightStack_->addWidget(normalPage_);
    auto* browser = new QWidget(splitter);
    auto* left = new QVBoxLayout(browser);
    search_ = new QLineEdit(browser);
    search_->setPlaceholderText("Search bindings and actions…");
    search_->setObjectName("bindingSearch");
    left->addWidget(search_);
    auto* filters = new QHBoxLayout;
    deviceFilter_ = filterCombo(browser);
    kindFilter_ = filterCombo(browser);
    kindFilter_->addItems({"Button", "EV_KEY", "Axis", "Hat"});
    modeFilter_ = filterCombo(browser);
    modifierFilter_ = filterCombo(browser);
    int filterIndex = 0;
    for (auto* combo : {deviceFilter_, kindFilter_, modeFilter_, modifierFilter_}) {
        auto* column = new QVBoxLayout;
        auto* label = new QLabel(QStringList{"Input controller", "Input type", "View mode", "While held"}[filterIndex++], browser);
        label->setBuddy(combo); column->addWidget(label); column->addWidget(combo); filters->addLayout(column);
    }
    left->addLayout(filters);
    model_ = new BindingModel(config_, this);
    proxy_ = new BindingFilter(this);
    proxy_->config = &config_;
    proxy_->setSourceModel(model_);
    table_ = new QTableView(browser);
    table_->setObjectName("bindingTable");
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(0, 85);
    table_->setColumnWidth(1, 135);
    table_->setColumnWidth(2, 120);
    table_->setColumnWidth(3, 90);
    table_->setWordWrap(true);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    left->addWidget(table_);
    table_->setMinimumHeight(180);
    auto* rowButtons = new QWidget(browser);
    auto* rowLayout = new QHBoxLayout(rowButtons);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto* add = new QPushButton("Add", rowButtons);
    auto* duplicate = new QPushButton("Duplicate", rowButtons);
    auto* remove = new QPushButton("Delete", rowButtons);
    rowLayout->addWidget(add); rowLayout->addWidget(duplicate); rowLayout->addWidget(remove);
    left->addWidget(rowButtons);
    for (const auto& entry : {std::pair{duplicate, QKeySequence("Ctrl+D")}, std::pair{remove, QKeySequence(Qt::Key_Delete)}}) {
        auto* action = new QAction(entry.first->text(), table_);
        action->setShortcut(entry.second); action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        table_->addAction(action);
        connect(action, &QAction::triggered, entry.first, &QPushButton::click);
    }
    controls_->activated = [this](const std::vector<Control>& inputs) {
        proxy_->inputFilter = true; proxy_->inputs = inputs; proxy_->refresh();
        if (proxy_->rowCount()) table_->setCurrentIndex(proxy_->index(0, 0));
        else selectBinding(-1);
    };
    controls_->labelChanged = [this](Control input, QString label) {
        const auto selectedIndex = table_->currentIndex();
        const int selectedRow = selectedIndex.isValid() ? proxy_->mapToSource(selectedIndex).row() : -1;
        if (!document_.editLabels([&](Config& config) {
            if (config.devices.at(input.device).kind == DeviceKind::Evdev) {
                std::erase_if(config.input_labels, [&](const auto& entry) { return entry.input == input; });
                if (!label.isEmpty()) config.input_labels.push_back({input, str(label)});
            } else {
                for (int n = 1; n <= joystick_button_count; ++n) if (joystick_button_code(n) == input.code) {
                    std::erase_if(config.output_labels, [&](const auto& entry) { return entry.device == input.device && entry.button == n; });
                    if (!label.isEmpty()) config.output_labels.push_back({input.device, n, str(label)});
                    break;
                }
            }
        })) return;
        detail_->refresh();
        refreshBinding();
        if (selectedRow >= 0) {
            const auto visible = proxy_->mapFromSource(model_->index(selectedRow, 0));
            if (visible.isValid()) table_->setCurrentIndex(visible);
        }
    };
    // The detail owns its input, condition, timing, and action editors.
    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    detail_ = new BindingDetail(document_);
    scroll->setWidget(detail_);
    detail_->edited = [this] { refreshBinding(); };
    detail_->inputChanging = [this](Control input) {
        auto& inputs = proxy_->inputs;
        if (!inputs.empty() && std::find(inputs.begin(), inputs.end(), input) == inputs.end()) {
            inputs.push_back(input); proxy_->refresh();
        }
    };
    splitter->addWidget(browser); splitter->addWidget(scroll);
    splitter->setSizes({300, 490});

    // Empty profiles offer a direct path to device setup.
    setupPrompt_ = new QWidget(rightStack_);
    auto* promptLayout = new QVBoxLayout(setupPrompt_);
    promptLayout->addStretch();
    auto* promptTitle = new QLabel("Set up a physical and virtual controller", setupPrompt_);
    promptTitle->setObjectName("setupPromptTitle");
    promptTitle->setAlignment(Qt::AlignCenter); promptTitle->setWordWrap(true);
    auto titleFont = promptTitle->font(); titleFont.setPointSize(titleFont.pointSize() + 5); titleFont.setBold(true);
    promptTitle->setFont(titleFont);
    promptLayout->addWidget(promptTitle);
    auto* promptHint = new QLabel("Mappings need a physical controller to read from and a virtual joystick to write to.", setupPrompt_);
    promptHint->setAlignment(Qt::AlignCenter); promptHint->setWordWrap(true);
    promptLayout->addWidget(promptHint);
    auto* promptButtons = new QHBoxLayout;
    promptButtons->addStretch();
    auto* createPhysical = new QPushButton("Create a physical device", setupPrompt_);
    createPhysical->setObjectName("createPhysical");
    auto* createVirtual = new QPushButton("Create a virtual device", setupPrompt_);
    createVirtual->setObjectName("createVirtual");
    promptButtons->addWidget(createPhysical); promptButtons->addWidget(createVirtual);
    promptButtons->addStretch(); promptLayout->addLayout(promptButtons); promptLayout->addStretch();
    connect(createPhysical, &QPushButton::clicked, this, [this] { if (requestDeviceSetup) requestDeviceSetup(false); });
    connect(createVirtual, &QPushButton::clicked, this, [this] { if (requestDeviceSetup) requestDeviceSetup(true); });
    rightStack_->addWidget(setupPrompt_);

    // Selection and edit intents go through the document, not the model's Config.
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current) {
                if (current.isValid()) selectBinding(proxy_->mapToSource(current).row());
            });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& value) {
        proxy_->search = value; proxy_->refresh();
    });
    auto updateFilter = [this] {
        proxy_->device = deviceFilter_->currentIndex() ? deviceFilter_->currentText() : QString{};
        proxy_->kind = kindFilter_->currentIndex() ? kindFilter_->currentText() : QString{};
        proxy_->mode = modeFilter_->currentIndex() ? modeFilter_->currentText() : QString{};
        proxy_->modifier = modifierFilter_->currentIndex() ? modifierFilter_->currentText() : QString{};
        proxy_->refresh();
    };
    for (auto* combo : {deviceFilter_, kindFilter_, modeFilter_, modifierFilter_})
        connect(combo, &QComboBox::currentIndexChanged, this, updateFilter);
    connect(add, &QPushButton::clicked, this, [this] {
        auto selectedControl = controls_->current();
        const auto selectedControls = controls_->selectedControls();
        if (!selectedControl || !config_.devices.contains(selectedControl->device) ||
            config_.devices.at(selectedControl->device).kind != DeviceKind::Evdev ||
            std::find(selectedControls.begin(), selectedControls.end(), *selectedControl) == selectedControls.end()) {
            QMessageBox::warning(this, "No control selected", "Select a physical control in the list on the left first.");
            return;
        }
        auto virtualDevice = std::find_if(config_.devices.begin(), config_.devices.end(),
                                          [](const auto& entry) { return entry.second.kind == DeviceKind::Uinput; });
        if (virtualDevice == config_.devices.end() || config_.modes.empty()) {
            QMessageBox::warning(this, "No devices or modes", "Set up a virtual joystick and a mode first.");
            return;
        }
        Binding binding{*selectedControl, {config_.modes.front()}, {},
                        {ButtonAction{virtualDevice->first, joystick_button_code(1)}}, std::nullopt};
        if (selectedControl->kind == ControlKind::AbsoluteAxis && !virtualDevice->second.axes.empty())
            binding.actions = {AxisAction{virtualDevice->first, virtualDevice->second.axes.begin()->first, false}};
        if (modeFilter_->currentIndex() > 0) binding.modes = {str(modeFilter_->currentText())};
        document_.insert(std::move(binding));
        refreshBinding();
        const auto index = proxy_->mapFromSource(model_->index(model_->rowCount() - 1, 0));
        if (index.isValid()) table_->setCurrentIndex(index);
        selectBinding(model_->rowCount() - 1);
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        if (selected_ < 0) return;
        document_.insert(config_.bindings.at(selected_));
        refreshBinding();
        const auto index = proxy_->mapFromSource(model_->index(model_->rowCount() - 1, 0));
        if (index.isValid()) table_->setCurrentIndex(index);
        selectBinding(model_->rowCount() - 1);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (selected_ < 0) return;
        const int row = selected_;
        detail_->closeActionEditor();
        document_.erase(row);
        selectBinding(-1);
        refreshBinding();
        if (model_->rowCount()) {
            const auto target = proxy_->mapFromSource(model_->index(std::min(row, model_->rowCount() - 1), 0));
            if (target.isValid()) table_->setCurrentIndex(target);
        }
    });
    selectBinding(-1); refreshFilters(); updateRightView();
}

MappingWorkspace::~MappingWorkspace() {
    // Models and views must go before the document they reference.
    table_->setModel(nullptr);
}

void MappingWorkspace::modelChange(ProfileDocument::Change change, int row, bool before) {
    if (before) model_->beginChange(static_cast<int>(change), row);
    else model_->endChange(static_cast<int>(change), row);
}

void MappingWorkspace::refreshFilters() {
    auto refill = [](QComboBox* combo, const auto& names) {
        const auto previous = combo->currentText();
        const QSignalBlocker blocker(combo);
        combo->clear(); combo->addItem("All");
        for (const auto& name : names) combo->addItem(qs(name));
        if (combo->findText(previous) >= 0) combo->setCurrentText(previous);
    };
    std::vector<std::string> physical;
    for (const auto& [name, device] : config_.devices)
        if (device.kind == DeviceKind::Evdev) physical.push_back(name);
    refill(deviceFilter_, physical);
    refill(modeFilter_, config_.modes);
    std::vector<std::string> modifiers;
    for (const auto& [name, control] : config_.modifiers) {
        (void)control; modifiers.push_back(name);
    }
    refill(modifierFilter_, modifiers);
    proxy_->device = deviceFilter_->currentIndex() > 0 ? deviceFilter_->currentText() : QString{};
    proxy_->mode = modeFilter_->currentIndex() > 0 ? modeFilter_->currentText() : QString{};
    proxy_->modifier = modifierFilter_->currentIndex() > 0 ? modifierFilter_->currentText() : QString{};
    proxy_->refresh(); controls_->refresh(); updateRightView();
}

void MappingWorkspace::selectBinding(int row) {
    if (selected_ != row) document_.breakMappingSession();
    selected_ = row;
    detail_->selectBinding(row);
    notifyChanged();
}

void MappingWorkspace::refreshBinding() {
    const QSignalBlocker blocker(table_->selectionModel());
    controls_->refresh();
    if (selected_ >= 0) model_->changed(selected_);
    proxy_->refresh();
    if (selected_ >= 0) {
        const auto visible = proxy_->mapFromSource(model_->index(selected_, 0));
        if (visible.isValid()) table_->setCurrentIndex(visible);
    }
    detail_->updateSummary();
    notifyChanged();
}

void MappingWorkspace::showControl(const Control& input) {
    controls_->select(input);
}

void MappingWorkspace::showMapping(int row) {
    if (row < 0 || row >= model_->rowCount()) return;
    const auto input = config_.bindings[row].input;
    showControl(input);
    auto visibleRow = [&] { return proxy_->mapFromSource(model_->index(row, 0)); };
    if (!visibleRow().isValid()) {
        for (auto* filter : {deviceFilter_, kindFilter_, modeFilter_, modifierFilter_})
            if (!visibleRow().isValid()) filter->setCurrentIndex(0);
        if (!visibleRow().isValid()) search_->clear();
    }
    if (auto index = visibleRow(); index.isValid()) {
        table_->setCurrentIndex(index); table_->scrollTo(index); selectBinding(row);
    }
}

void MappingWorkspace::resetBrowsing(bool resetKind) {
    detail_->closeActionEditor();
    proxy_->inputFilter = true; proxy_->inputs.clear();
    search_->clear();
    if (resetKind) kindFilter_->setCurrentIndex(0);
    selectBinding(-1); controls_->resetBrowsing(); refreshFilters();
    if (resetKind && model_->rowCount()) table_->setCurrentIndex(proxy_->index(0, 0));
}

void MappingWorkspace::refreshControls() { controls_->refresh(); }
void MappingWorkspace::focusSearch() { search_->setFocus(); search_->selectAll(); }
void MappingWorkspace::setIssue(const QString& message) { detail_->setIssue(message); }
void MappingWorkspace::setTableError(int row, const QString& message) { model_->setError(row, message); }
void MappingWorkspace::closeActionEditor() { detail_->closeActionEditor(); }
int MappingWorkspace::bindingCount() const { return model_->rowCount(); }
void MappingWorkspace::notifyChanged() { if (changed) changed(); }

void MappingWorkspace::updateRightView() {
    bool physical = false, virtualDevice = false;
    for (const auto& [name, device] : config_.devices) {
        (void)name;
        if (device.kind == DeviceKind::Evdev) physical = true;
        if (device.kind == DeviceKind::Uinput) virtualDevice = true;
    }
    rightStack_->setCurrentWidget(physical && virtualDevice ? normalPage_ : setupPrompt_);
}
