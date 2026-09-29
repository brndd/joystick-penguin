#include "control_browser.hpp"
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>
#include <QSocketNotifier>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cstring>

using namespace joystick_penguin;

QString inputName(const Control& input) {
    if (input.kind == ControlKind::Button)
        return input.code < 0 ? QString("Button %1").arg(-input.code) : QString("EV_KEY %1").arg(input.code);
    const char* name = libevdev_event_code_get_name(EV_ABS, input.code);
    QString axis = name ? QString::fromLatin1(name).mid(4) : QString("EV_ABS %1").arg(input.code);
    if (input.kind == ControlKind::HatDirection)
        return axis + (input.direction < 0 ? " · Negative (−)" : " · Positive (+)");
    return axis;
}

QString labeledInput(const Config& config, const Control& input) {
    for (const auto& label : config.input_labels)
        if (label.input == input) return QString::fromStdString(label.label) + " (" + inputName(input) + ")";
    return inputName(input);
}

struct ControlBrowser::State {
    const Config& config;
    QComboBox* controller;
    QLineEdit* search;
    QTreeWidget* tree;
    QLabel* monitor;
    QToolButton* reconnect;
    std::vector<Control> controls;
    std::vector<int> buttons;
    libevdev* device = nullptr;
    int fd = -1;
    QSocketNotifier* notifier = nullptr;
    QString path;
    QString controllerKey;
    QElapsedTimer clock;
    std::map<int, qint64> motion;
    std::map<int, int> motionValue;
    int readFlags = LIBEVDEV_READ_FLAG_NORMAL;
    bool rebuilding = false;
    bool controllers = false;
    explicit State(const Config& c) : config(c) { clock.start(); }
    void close() {
        delete notifier; notifier = nullptr;
        if (device) libevdev_free(device);
        device = nullptr;
        if (fd >= 0) ::close(fd);
        fd = -1;
        buttons.clear(); motion.clear(); motionValue.clear(); readFlags = LIBEVDEV_READ_FLAG_NORMAL;
    }
    ~State() { close(); }
};

ControlBrowser::ControlBrowser(const Config& config, QWidget* parent) : QWidget(parent), s_(std::make_unique<State>(config)) {
    auto& s = *s_;
    setMinimumWidth(230);
    auto* layout = new QVBoxLayout(this);
    auto* controllerLabel = new QLabel("&Controller", this);
    layout->addWidget(controllerLabel);
    s.controller = new QComboBox(this);
    s.controller->setObjectName("controllerBrowser");
    s.controller->setAccessibleName("Controller");
    controllerLabel->setBuddy(s.controller);
    layout->addWidget(s.controller);
    s.search = new QLineEdit(this);
    s.search->setPlaceholderText("Find control…");
    s.search->setAccessibleName("Find control");
    layout->addWidget(s.search);
    s.tree = new QTreeWidget(this);
    s.tree->setObjectName("controlTree");
    s.tree->setHeaderLabels({"Control", "Maps", "Activity"});
    s.tree->setIndentation(12);
    s.tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    s.tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    s.tree->header()->setStretchLastSection(false);
    s.tree->setColumnWidth(0, 165);
    s.tree->setColumnWidth(1, 65);
    s.tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    s.tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    s.tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(s.tree, 1);
    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    s.monitor = new QLabel(this);
    s.monitor->setObjectName("monitorStatus");
    s.monitor->setWordWrap(false);
    statusRow->addWidget(s.monitor, 1);
    s.reconnect = new QToolButton(this);
    s.reconnect->setObjectName("reconnectMonitor");
    s.reconnect->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    s.reconnect->setToolTip("Reconnect monitor");
    s.reconnect->setAccessibleName("Reconnect monitor");
    s.reconnect->setAutoRaise(true);
    statusRow->addWidget(s.reconnect);
    layout->addLayout(statusRow);
    connect(s.reconnect, &QToolButton::clicked, this, [this] { connectDevice(); rebuild(); });
    connect(s.controller, &QComboBox::currentTextChanged, this, [this] {
        connectDevice(); rebuild();
        const auto selected = selectedControls();
        if (selected.empty() && !s_->controls.empty() && s_->controllers)
            select(s_->controls.front());
        emitSelection();
    });
    connect(s.search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(s.tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!s_->rebuilding) emitSelection();
    });
    connect(s.tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        auto& s = *s_;
        if (s.rebuilding || column != 0 || !item->data(0, Qt::UserRole).isValid()) return;
        const Control input = s.controls[item->data(0, Qt::UserRole).toInt()];
        QString text = item->text(0).trimmed();
        if (text == inputName(input)) text.clear();
        if (labelChanged) labelChanged(input, text);
    });
    connect(s.tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0 || !item || !item->data(0, Qt::UserRole).isValid()) return;
        s_->tree->editItem(item, 0);
    });
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { displayActivity(); });
    timer->start();
    refresh();
}

ControlBrowser::~ControlBrowser() {
    s_->tree->disconnect(this);
    s_->controller->disconnect(this);
    for (auto* timer : findChildren<QTimer*>()) timer->stop();
}

void ControlBrowser::refresh() {
    auto& s = *s_;
    auto previous = s.controller->currentText();
    s.controller->blockSignals(true);
    s.controller->clear();
    for (const auto& [name, device] : s.config.devices)
        if (device.kind == DeviceKind::Evdev) s.controller->addItem(QString::fromStdString(name));
    if (s.controller->findText(previous) >= 0) s.controller->setCurrentText(previous);
    s.controller->blockSignals(false);
    s.controllers = s.controller->count() > 0;
    s.tree->setEnabled(s.controllers);
    auto found = s.config.devices.find(s.controller->currentText().toStdString());
    QString path = found == s.config.devices.end() ? QString{} : QString::fromStdString(found->second.path);
    if (s.path != path || s.controllerKey != s.controller->currentText()) connectDevice();
    rebuild();
    if (!s.controllers) {
        s.monitor->setText("No device");
        s.monitor->setToolTip("Add a physical controller in the Devices tab to observe controls.");
        s.monitor->setStyleSheet("color: #b00020;");
        return;
    }
    const auto selected = selectedControls();
    if (selected.empty() && !s.controls.empty()) {
        select(s.controls.front());
        emitSelection();
    } else if (selected.empty()) {
        emitSelection();
    }
}

void ControlBrowser::connectDevice() {
    auto& s = *s_;
    s.close(); s.controls.clear();
    s.controllerKey = s.controller->currentText();
    auto found = s.config.devices.find(s.controller->currentText().toStdString());
    s.path = found == s.config.devices.end() ? QString{} : QString::fromStdString(found->second.path);
    s.fd = ::open(s.path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int error = s.fd < 0 ? -errno : libevdev_new_from_fd(s.fd, &s.device);
    if (error < 0) {
        s.monitor->setText("Offline");
        s.monitor->setToolTip("Offline / unavailable: " + QString::fromLocal8Bit(std::strerror(-error)) + ". Manual editing is available.");
        s.monitor->setStyleSheet("color: #b00020;");
        s.close(); return;
    }
    for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code)
        if (libevdev_has_event_code(s.device, EV_KEY, code)) s.buttons.push_back(code);
    for (int code = BTN_MISC; code < BTN_JOYSTICK; ++code)
        if (libevdev_has_event_code(s.device, EV_KEY, code)) s.buttons.push_back(code);
    const auto name = s.controller->currentText().toStdString();
    for (int i = 0; i < static_cast<int>(s.buttons.size()); ++i) s.controls.push_back({name, ControlKind::Button, -i - 1});
    for (int code = 0; code <= ABS_MAX; ++code) {
        if (!libevdev_has_event_code(s.device, EV_ABS, code)) continue;
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
            s.controls.push_back({name, ControlKind::HatDirection, code, -1});
            s.controls.push_back({name, ControlKind::HatDirection, code, 1});
        } else s.controls.push_back({name, ControlKind::AbsoluteAxis, code});
    }
    s.monitor->setText("Monitoring");
    s.monitor->setToolTip(QString::fromUtf8(libevdev_get_name(s.device)) + " · Read-only monitoring. An exclusive grab by another process can suppress events; silence alone does not prove a grab.");
    s.monitor->setStyleSheet("color: #2e7d32;");
    s.notifier = new QSocketNotifier(s.fd, QSocketNotifier::Read, this);
    connect(s.notifier, &QSocketNotifier::activated, this, [this] {
        auto& s = *s_;
        input_event event{};
        for (int i = 0; i < 512; ++i) {
            int result = libevdev_next_event(s.device, s.readFlags, &event);
            if (result == -EAGAIN) {
                if (s.readFlags == LIBEVDEV_READ_FLAG_SYNC) { s.readFlags = LIBEVDEV_READ_FLAG_NORMAL; continue; }
                break;
            }
            if (result < 0) {
                s.monitor->setText("Disconnected");
                s.monitor->setToolTip("Controller disconnected / unreadable. Reconnect monitor to retry; offline editing is available.");
                s.monitor->setStyleSheet("color: #b00020;");
                s.notifier->setEnabled(false);
                return;
            }
            s.readFlags = result == LIBEVDEV_READ_STATUS_SYNC ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
            if (event.type == EV_ABS) {
                const auto* info = libevdev_get_abs_info(s.device, event.code);
                const qint64 span = info ? qint64(info->maximum) - info->minimum : 0;
                const qint64 deadband = std::max<qint64>(1, span / 200);
                if (!s.motionValue.contains(event.code) || std::abs(qint64(event.value) - s.motionValue[event.code]) >= deadband) {
                    s.motion[event.code] = s.clock.elapsed(); s.motionValue[event.code] = event.value;
                }
            }
        }
    });
}

std::optional<Control> ControlBrowser::current() const {
    auto* item = s_->tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid()) return std::nullopt;
    int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(s_->controls.size())) return std::nullopt;
    return s_->controls[index];
}

std::vector<Control> ControlBrowser::selectedControls() const {
    std::vector<Control> result;
    for (auto* item : s_->tree->selectedItems()) {
        if (!item->data(0, Qt::UserRole).isValid()) continue;
        int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(s_->controls.size())) continue;
        const auto& input = s_->controls[index];
        if (std::find(result.begin(), result.end(), input) == result.end()) result.push_back(input);
    }
    return result;
}

void ControlBrowser::emitSelection() {
    if (activated) activated(selectedControls());
}

void ControlBrowser::select(const Control& input) {
    auto& s = *s_;
    if (s.controller->currentText() != QString::fromStdString(input.device)) {
        s.controller->blockSignals(true);
        s.controller->setCurrentText(QString::fromStdString(input.device));
        s.controller->blockSignals(false);
        connectDevice(); rebuild();
    }
    for (int g = 0; g < s.tree->topLevelItemCount(); ++g) {
        auto* group = s.tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            auto* item = group->child(i);
            if (s.controls[item->data(0, Qt::UserRole).toInt()] == input) {
                s.rebuilding = true;
                s.tree->clearSelection();
                s.tree->setCurrentItem(item);
                item->setSelected(true);
                s.rebuilding = false;
                return;
            }
        }
    }
}

void ControlBrowser::rebuild() {
    auto& s = *s_;
    auto previous = current();
    const auto selected = selectedControls();
    s.rebuilding = true;
    auto include = [&](const Control& input) {
        if (input.device == s.controller->currentText().toStdString() &&
            std::find(s.controls.begin(), s.controls.end(), input) == s.controls.end()) s.controls.push_back(input);
    };
    for (const auto& binding : s.config.bindings) include(binding.input);
    for (const auto& [name, inputs] : s.config.modifiers) {
        (void)name;
        for (const auto& input : inputs) include(input);
    }
    for (const auto& label : s.config.input_labels) include(label.input);
    s.tree->clear();
    QTreeWidgetItem* groups[3];
    for (int g = 0; g < 3; ++g) groups[g] = new QTreeWidgetItem(s.tree, {QStringList{"Buttons", "Hats", "Axes"}[g]});
    for (int i = 0; i < static_cast<int>(s.controls.size()); ++i) {
        const auto& input = s.controls[i];
        if (input.device != s.controller->currentText().toStdString()) continue;
        QString label;
        for (const auto& entry : s.config.input_labels)
            if (entry.input == input) label = QString::fromStdString(entry.label);
        const auto identity = labeledInput(s.config, input);
        if (!identity.contains(s.search->text(), Qt::CaseInsensitive)) continue;
        int count = std::count_if(s.config.bindings.begin(), s.config.bindings.end(), [&](const auto& b) { return b.input == input; });
        QString usage = count ? QString::number(count) : "Unmapped";
        for (const auto& [modifier, controls] : s.config.modifiers) {
            (void)modifier;
            if (std::find(controls.begin(), controls.end(), input) != controls.end())
                usage = count ? usage + " · M" : "Modifier";
        }
        auto* item = new QTreeWidgetItem(groups[static_cast<int>(input.kind)], {label.isEmpty() ? inputName(input) : label, usage, "○"});
        item->setData(0, Qt::UserRole, i);
        item->setToolTip(0, identity + QString(" · code %1").arg(input.code) + " — click twice to edit the label");
        item->setToolTip(2, "Offline");
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        if (std::find(selected.begin(), selected.end(), input) != selected.end()) item->setSelected(true);
        if (previous && *previous == input) s.tree->setCurrentItem(item);
    }
    s.tree->expandAll();
    s.rebuilding = false;
    displayActivity();
}

void ControlBrowser::displayActivity() {
    auto& s = *s_;
    for (int g = 0; g < s.tree->topLevelItemCount(); ++g) {
        auto* group = s.tree->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            auto* item = group->child(i);
            const auto& input = s.controls[item->data(0, Qt::UserRole).toInt()];
            int code = input.code;
            if (code < 0) code = -code <= static_cast<int>(s.buttons.size()) ? s.buttons[-code - 1] : -1;
            if (!s.device || code < 0 || (s.notifier && !s.notifier->isEnabled())) { item->setText(2, "○"); item->setToolTip(2, "Offline"); continue; }
            int type = input.kind == ControlKind::Button ? EV_KEY : EV_ABS;
            if (!libevdev_has_event_code(s.device, type, code)) { item->setText(2, "○ Unavailable"); continue; }
            int value = libevdev_get_event_value(s.device, type, code);
            bool active = input.kind == ControlKind::Button ? value != 0 : input.kind == ControlKind::HatDirection ? value * input.direction > 0 :
                          s.motion.contains(code) && s.clock.elapsed() - s.motion[code] < 180;
            item->setText(2, (active ? "● " : "○ ") + (type == EV_ABS ? QString::number(value) : active ? "Held" : "Released"));
        }
    }
}
