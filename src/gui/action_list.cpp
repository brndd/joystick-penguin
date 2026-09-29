#include "action_list.hpp"
#include "action_editor.hpp"
#include "mapping_model.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>
#include <QComboBox>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

using namespace joystick_penguin;
using mapping_ui::actionName;

namespace {
void setLabel(QLabel* label, const QString& text) {
    label->setProperty("fullText", text);
    label->setToolTip(text);
    label->setText(label->fontMetrics().elidedText(text, Qt::ElideRight, label->width()));
}
}

ActionList::ActionList(ProfileDocument& document, QWidget* parent) : QWidget(parent), document_(document) {
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("actionSplit");
    outer->addWidget(split);

    // Combined Immediate or Tap/Hold rows, with controls scoped to each row.
    auto* listPane = new QWidget(split);
    listPane->setMinimumWidth(180);
    auto* listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableWidget(listPane);
    table_->setObjectName("actionList");
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels({"Name", "Type"});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setMinimumHeight(120);
    table_->setMinimumWidth(0);
    table_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    listLayout->addWidget(table_, 1);
    auto* addButton = new QPushButton("Add action", listPane);
    listLayout->addWidget(addButton);
    // The inline editor is replaced when the selected action location changes.
    auto* editorPane = new QWidget(split);
    editorLayout_ = new QVBoxLayout(editorPane);
    editorLayout_->setContentsMargins(0, 0, 0, 0);
    hint_ = new QLabel("Select an action to edit", editorPane);
    hint_->setAlignment(Qt::AlignCenter);
    editorLayout_->addWidget(hint_);
    editorLayout_->addStretch();
    split->addWidget(listPane);
    split->addWidget(editorPane);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({340, 390});
    connect(addButton, &QPushButton::clicked, this, [this] { add(); });
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) { openEditor(row); });
}

void ActionList::closeEditor() {
    if (!editor_) return;
    delete editor_;
    editor_ = nullptr;
    editorLocation_.reset();
    hint_->show();
}

void ActionList::clearSelection() {
    closeEditor();
    const QSignalBlocker blocker(table_);
    table_->setCurrentCell(-1, -1);
}

void ActionList::selectBinding(int row) {
    const bool changed = binding_ != row;
    if (changed) clearSelection();
    binding_ = row;
    refresh();
    if (changed && table_->rowCount() > 0) {
        table_->setCurrentCell(0, 0);
        openEditor(0);
    }
}

std::optional<ActionList::Location> ActionList::location(int row) const {
    const auto* item = row >= 0 ? table_->item(row, 0) : nullptr;
    if (!item || binding_ < 0) return std::nullopt;
    return Location{binding_, static_cast<Branch>(item->data(Qt::UserRole).toInt()), item->data(Qt::UserRole + 1).toInt()};
}

int ActionList::tableRow(Location target) const {
    for (int row = 0; row < table_->rowCount(); ++row)
        if (location(row) == target) return row;
    return -1;
}

const std::vector<Action>* ActionList::actions(Location where) const {
    const auto& config = document_.config();
    if (where.binding < 0 || where.binding >= static_cast<int>(config.bindings.size())) return nullptr;
    const auto& binding = config.bindings[where.binding];
    if (!binding.tap_hold) return where.branch == Branch::Immediate ? &binding.actions : nullptr;
    if (where.branch == Branch::Immediate) return nullptr;
    return where.branch == Branch::Tap ? &binding.tap_hold->tap : &binding.tap_hold->hold;
}

std::vector<Action>& ActionList::actions(Binding& binding, Branch branch) {
    if (!binding.tap_hold) return binding.actions;
    return branch == Branch::Tap ? binding.tap_hold->tap : binding.tap_hold->hold;
}

void ActionList::notifyEdit(bool rebuild) {
    if (edited) edited();
    if (rebuild) refresh();
}

void ActionList::add() {
    const auto& config = document_.config();
    if (binding_ < 0 || binding_ >= static_cast<int>(config.bindings.size())) return;
    const auto device = std::find_if(config.devices.begin(), config.devices.end(),
                                     [](const auto& entry) { return entry.second.kind == DeviceKind::Uinput; });
    if (device == config.devices.end()) {
        QMessageBox::warning(this, "No virtual joystick", "Set up a virtual joystick first.");
        return;
    }
    Branch branch = Branch::Immediate;
    if (config.bindings[binding_].tap_hold) {
        auto selected = location(table_->currentRow());
        branch = selected && selected->branch != Branch::Immediate ? selected->branch : Branch::Hold;
    }
    Location where{binding_, branch, 0};
    where.index = static_cast<int>(actions(where)->size());
    const auto deviceName = device->first;
    const auto kind = config.bindings[binding_].input.kind;
    const int axis = device->second.axes.empty() ? ABS_X : device->second.axes.begin()->first;
    document_.editMapping(binding_, [&](Binding& binding) {
        auto& list = actions(binding, branch);
        if (kind == ControlKind::AbsoluteAxis) list.push_back(AxisAction{deviceName, axis, false});
        else list.push_back(ButtonAction{deviceName, joystick_button_code(1)});
    });
    notifyEdit(true);
    table_->setCurrentCell(tableRow(where), 0);
}

void ActionList::openEditor(int row) {
    if (filling_) return;
    auto where = location(row);
    const auto* list = where ? actions(*where) : nullptr;
    if (!list || where->index < 0 || where->index >= static_cast<int>(list->size())) {
        closeEditor();
        return;
    }
    if (editor_ && editorLocation_ == where) return;
    closeEditor();
    auto* form = new ActionEditor(document_.config(), document_.config().bindings[where->binding].input.kind,
                                  (*list)[where->index], this);
    form->setObjectName("inlineActionEditor");
    editor_ = form;
    editorLocation_ = where;
    form->changed = [this, where = *where] {
        const auto* list = actions(where);
        if (!list || where.index >= static_cast<int>(list->size()) || !editor_ || editorLocation_ != where) return;
        document_.editMapping(where.binding, [&](Binding& binding) {
            actions(binding, where.branch)[where.index] = editor_->result();
        });
        list = actions(where);
        if (auto* item = table_->item(tableRow(where), 0)) {
            const QString text = actionName(document_.config(), (*list)[where.index]);
            item->setData(Qt::AccessibleTextRole, text);
            item->setToolTip(text);
            if (auto* widget = table_->cellWidget(item->row(), 0))
                if (auto* label = widget->findChild<QLabel*>("actionRowLabel")) setLabel(label, text);
        }
        notifyEdit(false);
    };
    editorLayout_->insertWidget(0, form);
    hint_->hide();
    form->show();
}

void ActionList::remove(int row) {
    auto where = location(row);
    const auto* list = where ? actions(*where) : nullptr;
    if (!list || where->index < 0 || where->index >= static_cast<int>(list->size())) return;
    closeEditor();
    document_.editMapping(where->binding, [&](Binding& binding) {
        auto& items = actions(binding, where->branch);
        items.erase(items.begin() + where->index);
    });
    notifyEdit(true);
    table_->setCurrentCell(std::min(row, table_->rowCount() - 1), 0);
    openEditor(table_->currentRow());
}

void ActionList::move(int row, int offset) {
    auto where = location(row);
    const auto* list = where ? actions(*where) : nullptr;
    if (!list || where->index < 0 || where->index + offset < 0 || where->index + offset >= static_cast<int>(list->size())) return;
    closeEditor();
    document_.editMapping(where->binding, [&](Binding& binding) {
        auto& items = actions(binding, where->branch);
        std::swap(items[where->index], items[where->index + offset]);
    });
    notifyEdit(true);
    where->index += offset;
    table_->setCurrentCell(tableRow(*where), 0);
    openEditor(table_->currentRow());
}

void ActionList::changeBranch(int row, Branch branch) {
    auto where = location(row);
    if (!where || where->branch == branch || branch == Branch::Immediate) return;
    const auto* from = actions(*where);
    auto destination = *where;
    destination.branch = branch;
    const auto* to = actions(destination);
    if (!from || !to || where->index < 0 || where->index >= static_cast<int>(from->size())) return;
    closeEditor();
    destination.index = static_cast<int>(to->size());
    document_.editMapping(where->binding, [&](Binding& binding) {
        auto& source = actions(binding, where->branch);
        auto& target = actions(binding, branch);
        target.push_back(std::move(source[where->index]));
        source.erase(source.begin() + where->index);
    });
    notifyEdit(true);
    table_->setCurrentCell(tableRow(destination), 0);
    openEditor(table_->currentRow());
}

void ActionList::refresh() {
    const auto& config = document_.config();
    if (binding_ < 0 || binding_ >= static_cast<int>(config.bindings.size())) {
        closeEditor();
        const QSignalBlocker blocker(table_);
        table_->setRowCount(0);
        return;
    }
    const auto& binding = config.bindings[binding_];
    auto previous = location(table_->currentRow());
    const QSignalBlocker blocker(table_);
    filling_ = true;
    table_->setRowCount(0);
    auto append = [this, &config, timed = binding.tap_hold.has_value()](const std::vector<Action>& list, Branch branch) {
        for (int index = 0; index < static_cast<int>(list.size()); ++index) {
            const int rowNumber = table_->rowCount();
            table_->insertRow(rowNumber);
            const QString text = actionName(config, list[index]);
            auto* item = new QTableWidgetItem;
            item->setData(Qt::AccessibleTextRole, text);
            item->setData(Qt::UserRole, static_cast<int>(branch));
            item->setData(Qt::UserRole + 1, index);
            item->setToolTip(text);
            table_->setItem(rowNumber, 0, item);
            auto* row = new QWidget(table_);
            row->setProperty("actionIndex", rowNumber);
            row->installEventFilter(this);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(4, 1, 4, 1);
            auto* label = new QLabel(row);
            label->setObjectName("actionRowLabel");
            label->setMinimumWidth(0);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            setLabel(label, text);
            label->installEventFilter(this);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            layout->addWidget(label, 1);
            for (int offset : {-1, 1}) {
                auto* control = new QToolButton(row);
                const QString description = QString("Move action %1 %2").arg(rowNumber + 1).arg(offset < 0 ? "up" : "down");
                control->setText(offset < 0 ? "↑" : "↓");
                control->setToolTip(description); control->setAccessibleName(description);
                control->setAutoRaise(true);
                control->setEnabled(index + offset >= 0 && index + offset < static_cast<int>(list.size()));
                layout->addWidget(control);
                connect(control, &QToolButton::clicked, this, [this, rowNumber, offset] { move(rowNumber, offset); });
            }
            auto* trash = new QToolButton(row);
            const QString removeName = QString("Remove action %1").arg(rowNumber + 1);
            trash->setText("🗑"); trash->setToolTip(removeName); trash->setAccessibleName(removeName);
            trash->setAutoRaise(true);
            layout->addWidget(trash);
            connect(trash, &QToolButton::clicked, this, [this, rowNumber] { remove(rowNumber); });
            table_->setRowHeight(rowNumber, row->sizeHint().height());
            table_->setCellWidget(rowNumber, 0, row);
            auto* type = new QTableWidgetItem;
            type->setData(Qt::AccessibleTextRole, branch == Branch::Tap ? "Tap" : branch == Branch::Hold ? "Hold" : "Immediate");
            if (!timed) type->setText("Immediate");
            table_->setItem(rowNumber, 1, type);
            if (timed) {
                auto* choice = new QComboBox(table_);
                choice->addItem("Tap", static_cast<int>(Branch::Tap));
                choice->addItem("Hold", static_cast<int>(Branch::Hold));
                choice->setCurrentIndex(branch == Branch::Tap ? 0 : 1);
                choice->setAccessibleName(QString("Action %1 type").arg(rowNumber + 1));
                table_->setCellWidget(rowNumber, 1, choice);
                connect(choice, &QComboBox::currentIndexChanged, this, [this, rowNumber, choice = QPointer<QComboBox>(choice)](int) {
                    if (!choice) return;
                    const auto target = static_cast<Branch>(choice->currentData().toInt());
                    const auto original = location(rowNumber);
                    QTimer::singleShot(0, this, [this, choice, rowNumber, target, original] {
                        if (choice && original == location(rowNumber)) changeBranch(rowNumber, target);
                    });
                });
            }
        }
    };
    if (binding.tap_hold) {
        append(binding.tap_hold->tap, Branch::Tap);
        append(binding.tap_hold->hold, Branch::Hold);
    } else append(binding.actions, Branch::Immediate);
    if (previous) {
        previous->branch = binding.tap_hold ? (previous->branch == Branch::Tap ? Branch::Tap : Branch::Hold) : Branch::Immediate;
        const int row = tableRow(*previous);
        if (row >= 0) table_->setCurrentCell(row, 0);
    }
    filling_ = false;
}

bool ActionList::eventFilter(QObject* watched, QEvent* event) {
    if (watched->objectName() == "actionRowLabel" && event->type() == QEvent::Resize) {
        auto* label = static_cast<QLabel*>(watched);
        const QString text = label->property("fullText").toString();
        const QString elided = label->fontMetrics().elidedText(text, Qt::ElideRight, label->width());
        if (label->text() != elided) label->setText(elided);
    }
    if (watched->property("actionIndex").isValid() && event->type() == QEvent::MouseButtonPress)
        table_->setCurrentCell(watched->property("actionIndex").toInt(), 0);
    return QWidget::eventFilter(watched, event);
}
