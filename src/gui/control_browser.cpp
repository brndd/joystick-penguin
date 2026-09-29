#include "control_browser.hpp"
#include "joystick_penguin/joystick_preset.hpp"
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSocketNotifier>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <type_traits>

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

QString labeledOutputButton(const Config& config, const std::string& device, int number) {
    const QString identity = QString("Button %1").arg(number);
    for (const auto& label : config.output_labels)
        if (label.device == device && label.button == number)
            return QString::fromStdString(label.label) + " (" + identity + ")";
    return identity;
}

namespace {
namespace fs = std::filesystem;

class ControlTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> selectedAll;
    void selectAll() override {
        const QSignalBlocker blocker(selectionModel());
        QTreeWidget::selectAll();
        for (auto* item : findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) item->setSelected(true);
        if (selectedAll) selectedAll();
    }
};

bool outputMatches(const Action& action, const Control& control) {
    return std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ModeAction>) return false;
        else if constexpr (std::is_same_v<T, ButtonAction>)
            return value.device == control.device && control.kind == ControlKind::Button && value.code == control.code;
        else if constexpr (std::is_same_v<T, AxisAction>)
            return value.device == control.device && control.kind == ControlKind::AbsoluteAxis && value.code == control.code;
        else return value.device == control.device && control.kind == ControlKind::HatDirection &&
                    value.code == control.code && value.direction == control.direction;
    }, action);
}

bool targets(const Binding& binding, const Control& control) {
    auto contains = [&](const std::vector<Action>& actions) {
        return std::any_of(actions.begin(), actions.end(), [&](const Action& action) { return outputMatches(action, control); });
    };
    return contains(binding.actions) || (binding.tap_hold &&
        (contains(binding.tap_hold->tap) || contains(binding.tap_hold->hold)));
}

// The profile does not store a path for uinput outputs. Match only a unique
// event node with the identity used by UinputOutput; never guess between twins.
QString virtualPath(const Config& config, const std::string& key, const Device& device) {
    const QString label = QString::fromStdString(device.virtual_name.empty() ? "JP " + key : device.virtual_name);
    for (const auto& [otherKey, other] : config.devices) {
        if (otherKey == key || other.kind != DeviceKind::Uinput) continue;
        const QString otherLabel = QString::fromStdString(other.virtual_name.empty() ? "JP " + otherKey : other.virtual_name);
        if (label == otherLabel && device.bus == other.bus && device.vendor_id == other.vendor_id &&
            device.product_id == other.product_id) return {};
    }
    QString match;
    std::error_code error;
    for (fs::directory_iterator it("/dev/input", fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        const auto path = it->path();
        const auto filename = path.filename().string();
        if (!filename.starts_with("event")) continue;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        libevdev* evdev = nullptr;
        if (libevdev_new_from_fd(fd, &evdev) == 0) {
            const char* name = libevdev_get_name(evdev);
            if (name && QString::fromUtf8(name) == label &&
                libevdev_get_id_bustype(evdev) == (device.bus == VirtualBus::Usb ? BUS_USB : BUS_VIRTUAL) &&
                libevdev_get_id_vendor(evdev) == device.vendor_id &&
                libevdev_get_id_product(evdev) == device.product_id) {
                if (!match.isEmpty()) { libevdev_free(evdev); ::close(fd); return {}; }
                match = QString::fromStdString(path.string());
            }
            libevdev_free(evdev);
        }
        ::close(fd);
    }
    return match;
}
}

struct ControlBrowser::State {
    struct Monitor {
        libevdev* device = nullptr;
        int fd = -1;
        QSocketNotifier* notifier = nullptr;
        QString path;
        QString status = "Offline";
        QString issue = "Offline / unavailable. Manual editing is available.";
        std::vector<int> buttons;
        std::map<int, qint64> motion;
        std::map<int, int> motionValue;
        int readFlags = LIBEVDEV_READ_FLAG_NORMAL;
        void close() {
            delete notifier; notifier = nullptr;
            if (device) libevdev_free(device);
            device = nullptr;
            if (fd >= 0) ::close(fd);
            fd = -1;
            buttons.clear(); motion.clear(); motionValue.clear(); readFlags = LIBEVDEV_READ_FLAG_NORMAL;
        }
        ~Monitor() { close(); }
    };
    const Config& config;
    QComboBox* controller;
    QLineEdit* search;
    QTreeWidget* tree;
    QLabel* monitor;
    QToolButton* reconnect;
    std::vector<Control> controls;
    std::vector<Control> selected;
    std::map<std::string, std::unique_ptr<Monitor>> monitors;
    std::map<QString, bool> expanded;
    QString lastSearch;
    QElapsedTimer clock;
    bool rebuilding = false;
    bool initialized = false;
    explicit State(const Config& c) : config(c) { clock.start(); }
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
    auto* tree = new ControlTree(this);
    s.tree = tree;
    tree->selectedAll = [this] {
        if (s_->rebuilding) return;
        s_->selected = selectedControls();
        emitSelection();
    };
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
    connect(s.reconnect, &QToolButton::clicked, this, [this] { connectDevices(true); rebuild(); });
    connect(s.controller, &QComboBox::currentIndexChanged, this, [this] {
        rebuild();
        selectVisible();
        emitSelection();
    });
    connect(s.search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(s.tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (s_->rebuilding) return;
        // A header represents all its descendants. Keep leaf selections as the
        // source of truth so collapse and header multi-selection work alike.
        s_->rebuilding = true;
        for (auto* item : s_->tree->selectedItems()) {
            if (item->data(0, Qt::UserRole).isValid()) continue;
            auto selectChildren = [&](auto&& self, QTreeWidgetItem* parent) -> void {
                for (int i = 0; i < parent->childCount(); ++i) {
                    auto* child = parent->child(i);
                    child->setSelected(true);
                    self(self, child);
                }
            };
            selectChildren(selectChildren, item);
        }
        s_->rebuilding = false;
        s_->selected = selectedControls();
        emitSelection();
    });
    connect(s.tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        auto& s = *s_;
        if (s.rebuilding || column != 0 || !item->data(0, Qt::UserRole).isValid()) return;
        const Control input = s.controls[item->data(0, Qt::UserRole).toInt()];
        const bool physical = s.config.devices.at(input.device).kind == DeviceKind::Evdev;
        if (!physical && input.kind != ControlKind::Button) return;
        QString text = item->text(0).trimmed();
        QString identity = inputName(input);
        if (!physical) for (int n = 1; n <= joystick_button_count; ++n)
            if (joystick_button_code(n) == input.code) identity = QString("Button %1").arg(n);
        if (text == identity) text.clear();
        if (labelChanged) labelChanged(input, text);
    });
    connect(s.tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0 || !item || !item->data(0, Qt::UserRole).isValid()) return;
        s_->tree->editItem(item, 0);
    });
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!isVisible()) return;
        for (const auto& [name, monitor] : s_->monitors) {
            (void)name;
            if (monitor->status == "Monitoring") { displayActivity(); break; }
        }
    });
    timer->start();
    refresh();
}

ControlBrowser::~ControlBrowser() = default;

void ControlBrowser::refresh() {
    auto& s = *s_;
    const auto previous = s.controller->currentData();
    {
        const QSignalBlocker blocker(s.controller);
        s.controller->clear();
        s.controller->addItem("All", -1);
        s.controller->addItem("Physical only", -2);
        s.controller->addItem("Virtual only", -3);
        for (const auto& [name, device] : s.config.devices) {
            (void)device;
            s.controller->addItem(QString::fromStdString(name), QString::fromStdString(name));
        }
        if (previous.isValid() && s.controller->findData(previous) >= 0)
            s.controller->setCurrentIndex(s.controller->findData(previous));
    }
    s.tree->setEnabled(!s.config.devices.empty());
    connectDevices(false);
    rebuild();
    if (!s.initialized && !s.controls.empty()) {
        s.initialized = true;
        selectVisible();
        emitSelection();
    } else if (s.selected.empty()) emitSelection();
}

void ControlBrowser::resetBrowsing() {
    auto& s = *s_;
    s.rebuilding = true;
    s.tree->clear();
    s.controls.clear();
    s.selected.clear();
    s.expanded.clear();
    s.lastSearch.clear();
    s.initialized = false;
    s.rebuilding = false;
    {
        const QSignalBlocker searchBlocker(s.search);
        s.search->clear();
        const QSignalBlocker blocker(s.controller);
        s.controller->setCurrentIndex(0);
    }
    refresh();
}

void ControlBrowser::connectDevices(bool force) {
    auto& s = *s_;
    for (auto it = s.monitors.begin(); it != s.monitors.end();) {
        if (!s.config.devices.contains(it->first)) it = s.monitors.erase(it);
        else ++it;
    }
    for (const auto& [name, device] : s.config.devices) {
        auto& monitor = s.monitors[name];
        if (!monitor) monitor = std::make_unique<State::Monitor>();
        const QString path = device.kind == DeviceKind::Evdev ? QString::fromStdString(device.path) : virtualPath(s.config, name, device);
        if (!force && monitor->path == path && (monitor->device || path.isEmpty())) continue;
        monitor->close();
        monitor->path = path;
        monitor->status = "Offline";
        monitor->issue = device.kind == DeviceKind::Uinput && path.isEmpty() ?
            "Virtual device not running, unreadable, or identity ambiguous. Manual browsing is available." :
            "Offline / unavailable. Manual editing is available.";
        if (path.isEmpty()) continue;
        monitor->fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        const int error = monitor->fd < 0 ? -errno : libevdev_new_from_fd(monitor->fd, &monitor->device);
        if (error < 0) {
            monitor->issue = "Offline / unavailable: " + QString::fromLocal8Bit(std::strerror(-error)) + ". Manual editing is available.";
            monitor->close();
            continue;
        }
        for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code)
            if (libevdev_has_event_code(monitor->device, EV_KEY, code)) monitor->buttons.push_back(code);
        for (int code = BTN_MISC; code < BTN_JOYSTICK; ++code)
            if (libevdev_has_event_code(monitor->device, EV_KEY, code)) monitor->buttons.push_back(code);
        monitor->status = "Monitoring";
        monitor->issue = QString::fromUtf8(libevdev_get_name(monitor->device)) +
            " · Live activity may be hidden while another app is using this controller.";
        monitor->notifier = new QSocketNotifier(monitor->fd, QSocketNotifier::Read, this);
        auto* observed = monitor.get();
        connect(monitor->notifier, &QSocketNotifier::activated, this, [this, observed] {
            input_event event{};
            for (int i = 0; i < 512; ++i) {
                const int result = libevdev_next_event(observed->device, observed->readFlags, &event);
                if (result == -EAGAIN) {
                    if (observed->readFlags == LIBEVDEV_READ_FLAG_SYNC) { observed->readFlags = LIBEVDEV_READ_FLAG_NORMAL; continue; }
                    break;
                }
                if (result < 0) {
                    observed->status = "Disconnected";
                    observed->issue = "Controller disconnected / unreadable. Reconnect monitor to retry; offline editing is available.";
                    observed->notifier->setEnabled(false);
                    displayActivity();
                    return;
                }
                observed->readFlags = result == LIBEVDEV_READ_STATUS_SYNC ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
                if (event.type == EV_ABS) {
                    const auto* info = libevdev_get_abs_info(observed->device, event.code);
                    const qint64 span = info ? qint64(info->maximum) - info->minimum : 0;
                    const qint64 deadband = std::max<qint64>(1, span / 200);
                    if (!observed->motionValue.contains(event.code) ||
                        std::abs(qint64(event.value) - observed->motionValue[event.code]) >= deadband) {
                        observed->motion[event.code] = s_->clock.elapsed();
                        observed->motionValue[event.code] = event.value;
                    }
                }
            }
        });
    }
}

std::optional<Control> ControlBrowser::current() const {
    auto* item = s_->tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid()) return std::nullopt;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(s_->controls.size())) return std::nullopt;
    return s_->controls[index];
}

std::vector<Control> ControlBrowser::selectedControls() const {
    std::vector<Control> result;
    for (auto* item : s_->tree->selectedItems()) {
        if (!item->data(0, Qt::UserRole).isValid()) continue;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(s_->controls.size())) continue;
        const auto& input = s_->controls[index];
        if (std::find(result.begin(), result.end(), input) == result.end()) result.push_back(input);
    }
    return result;
}

void ControlBrowser::emitSelection() {
    if (activated) activated(s_->selected);
}

void ControlBrowser::selectVisible() {
    auto& s = *s_;
    s.rebuilding = true;
    s.tree->selectAll();
    for (auto* item : s.tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) {
        if (!item->data(0, Qt::UserRole).isValid()) continue;
        if (s.config.devices.at(s.controls[item->data(0, Qt::UserRole).toInt()].device).kind != DeviceKind::Evdev) continue;
        s.tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        break;
    }
    s.rebuilding = false;
    s.selected = selectedControls();
}

void ControlBrowser::select(const Control& input) {
    auto& s = *s_;
    const auto found = s.config.devices.find(input.device);
    if (found == s.config.devices.end()) return;
    const int filter = s.controller->currentIndex();
    const bool visible = filter == 0 ||
        (filter == 1 && found->second.kind == DeviceKind::Evdev) ||
        (filter == 2 && found->second.kind == DeviceKind::Uinput) ||
        (filter > 2 && s.controller->currentData().toString() == QString::fromStdString(input.device));
    if (!visible) {
        const QSignalBlocker blocker(s.controller);
        s.controller->setCurrentIndex(s.controller->findData(QString::fromStdString(input.device)));
        rebuild();
    }
    auto findControl = [&]() -> QTreeWidgetItem* {
        for (auto* item : s.tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) {
            if (!item->data(0, Qt::UserRole).isValid()) continue;
            if (s.controls[item->data(0, Qt::UserRole).toInt()] == input) return item;
        }
        return nullptr;
    };
    if (!findControl() && !s.search->text().isEmpty()) s.search->clear();
    if (auto* item = findControl()) {
        s.rebuilding = true;
        s.tree->clearSelection();
        s.tree->setCurrentItem(item);
        item->setSelected(true);
        s.rebuilding = false;
        s.selected = selectedControls();
        emitSelection();
    }
}

void ControlBrowser::rebuild() {
    auto& s = *s_;
    const auto selected = s.selected;
    const bool allSelected = s.search->text().isEmpty() && s.lastSearch.isEmpty() &&
                             !selected.empty() && selected.size() == s.controls.size();
    const auto previous = current();
    for (int i = 0; i < s.tree->topLevelItemCount(); ++i) {
        auto* device = s.tree->topLevelItem(i);
        s.expanded[device->text(0)] = device->isExpanded();
        for (int j = 0; j < device->childCount(); ++j)
            s.expanded[device->text(0) + "/" + device->child(j)->text(0)] = device->child(j)->isExpanded();
    }
    s.rebuilding = true;
    s.tree->clear();
    s.controls.clear();
    for (const auto& [name, device] : s.config.devices) {
        const int filter = s.controller->currentIndex();
        if ((filter == 1 && device.kind != DeviceKind::Evdev) ||
            (filter == 2 && device.kind != DeviceKind::Uinput) ||
            (filter > 2 && s.controller->currentData().toString() != QString::fromStdString(name))) continue;
        const bool physical = device.kind == DeviceKind::Evdev;
        std::vector<Control> controls;
        auto include = [&](Control input) {
            if (std::find(controls.begin(), controls.end(), input) == controls.end()) controls.push_back(std::move(input));
        };
        const auto& monitor = *s.monitors.at(name);
        if (physical) {
            for (int i = 0; i < static_cast<int>(monitor.buttons.size()); ++i)
                include({name, ControlKind::Button, -i - 1});
            if (monitor.device) for (int code = 0; code <= ABS_MAX; ++code) {
                if (!libevdev_has_event_code(monitor.device, EV_ABS, code)) continue;
                if (joystick_hats().contains(code)) {
                    include({name, ControlKind::HatDirection, code, -1});
                    include({name, ControlKind::HatDirection, code, 1});
                } else include({name, ControlKind::AbsoluteAxis, code});
            }
            for (const auto& binding : s.config.bindings) if (binding.input.device == name) include(binding.input);
            for (const auto& [modifier, inputs] : s.config.modifiers) {
                (void)modifier;
                for (const auto& input : inputs) if (input.device == name) include(input);
            }
            for (const auto& label : s.config.input_labels) if (label.input.device == name) include(label.input);
        } else {
            for (int i = 1; i <= joystick_button_count; ++i)
                include({name, ControlKind::Button, joystick_button_code(i)});
            for (const int code : joystick_hats()) {
                include({name, ControlKind::HatDirection, code, -1});
                include({name, ControlKind::HatDirection, code, 1});
            }
            auto axes = joystick_axes();
            axes.insert(device.axes.begin(), device.axes.end());
            for (const auto& [code, range] : axes) { (void)range; include({name, ControlKind::AbsoluteAxis, code}); }
            auto actions = [&](const std::vector<Action>& list) {
                for (const auto& action : list) std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, ButtonAction>) {
                        if (value.device == name) include({name, ControlKind::Button, value.code});
                    } else if constexpr (std::is_same_v<T, HatAction>) {
                        if (value.device == name) include({name, ControlKind::HatDirection, value.code, value.direction});
                    } else if constexpr (std::is_same_v<T, AxisAction>) {
                        if (value.device == name) include({name, ControlKind::AbsoluteAxis, value.code});
                    }
                }, action);
            };
            for (const auto& binding : s.config.bindings) {
                actions(binding.actions);
                if (binding.tap_hold) { actions(binding.tap_hold->tap); actions(binding.tap_hold->hold); }
            }
        }
        auto* root = new QTreeWidgetItem(s.tree, {QString::fromStdString(name) + (physical ? " · Physical" : " · Virtual")});
        QTreeWidgetItem* groups[3];
        for (int g = 0; g < 3; ++g)
            groups[g] = new QTreeWidgetItem(root, {QStringList{"Buttons", "Hats", "Axes"}[g]});
        for (const auto& control : controls) {
            QString identity = inputName(control);
            QString title = identity;
            if (physical) for (const auto& label : s.config.input_labels)
                if (label.input == control) title = QString::fromStdString(label.label);
            if (!physical && control.kind == ControlKind::Button) {
                for (int n = 1; n <= joystick_button_count; ++n)
                    if (joystick_button_code(n) == control.code) {
                        identity = QString("Button %1").arg(n);
                        title = identity;
                        for (const auto& label : s.config.output_labels)
                            if (label.device == name && label.button == n) title = QString::fromStdString(label.label);
                    }
            }
            if (!(title + " " + identity).contains(s.search->text(), Qt::CaseInsensitive)) continue;
            int count = std::count_if(s.config.bindings.begin(), s.config.bindings.end(), [&](const Binding& binding) {
                return physical ? binding.input == control : targets(binding, control);
            });
            QString usage = count ? QString::number(count) : "Unmapped";
            if (physical) for (const auto& [modifier, inputs] : s.config.modifiers) {
                (void)modifier;
                if (std::find(inputs.begin(), inputs.end(), control) != inputs.end()) {
                    usage = count ? usage + " · M" : "Modifier";
                    break;
                }
            }
            const int index = static_cast<int>(s.controls.size());
            s.controls.push_back(control);
            auto* item = new QTreeWidgetItem(groups[static_cast<int>(control.kind)], {title, usage, "○"});
            item->setData(0, Qt::UserRole, index);
            item->setToolTip(0, identity + QString(" · code %1").arg(control.code) +
                                    ((physical || (control.kind == ControlKind::Button && identity.startsWith("Button ")))
                                        ? " — click twice to edit the label" : ""));
            item->setToolTip(2, "Offline");
            if (physical || (!physical && control.kind == ControlKind::Button && identity.startsWith("Button ")))
                item->setFlags(item->flags() | Qt::ItemIsEditable);
            if (allSelected || std::find(selected.begin(), selected.end(), control) != selected.end()) item->setSelected(true);
            if (previous && *previous == control) s.tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        }
        root->setExpanded(!s.expanded.contains(root->text(0)) || s.expanded.at(root->text(0)));
        for (auto* group : groups) {
            const auto key = root->text(0) + "/" + group->text(0);
            group->setExpanded(!s.expanded.contains(key) || s.expanded.at(key));
        }
    }
    if (allSelected && (!previous || !s.config.devices.contains(previous->device) ||
                        s.config.devices.at(previous->device).kind != DeviceKind::Evdev)) {
        for (auto* item : s.tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) {
            if (!item->data(0, Qt::UserRole).isValid()) continue;
            if (s.config.devices.at(s.controls[item->data(0, Qt::UserRole).toInt()].device).kind != DeviceKind::Evdev) continue;
            s.tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            break;
        }
    }
    s.rebuilding = false;
    s.selected = selected;
    if (allSelected) s.selected = selectedControls();
    std::erase_if(s.selected, [&](const Control& control) { return !s.config.devices.contains(control.device); });
    s.lastSearch = s.search->text();
    displayActivity();
    if (s.selected != selected) emitSelection();
}

void ControlBrowser::displayActivity() {
    auto& s = *s_;
    QStringList issues;
    int online = 0;
    bool disconnected = false;
    for (const auto& [name, monitor] : s.monitors) {
        if (monitor->status == "Monitoring") ++online;
        if (monitor->status == "Disconnected") disconnected = true;
        issues << QString("<div style='color: %1'>%2: %3</div>")
            .arg(monitor->status == "Monitoring" ? "#2e7d32" : "#ef6c00",
                 QString::fromStdString(name).toHtmlEscaped(), monitor->issue.toHtmlEscaped());
    }
    s.monitor->setText(s.monitors.empty() ? "No device" : s.monitors.size() == 1 ?
        s.monitors.begin()->second->status :
        QString("%1 %2/%3").arg(online ? "Monitoring" : disconnected ? "Disconnected" : "Offline")
            .arg(online).arg(s.monitors.size()));
    s.monitor->setToolTip("<html><body>" + issues.join(QString{}) + "</body></html>");
    s.monitor->setStyleSheet(online == static_cast<int>(s.monitors.size()) && online ? "color: #2e7d32;" : "color: #ef6c00;");
    for (auto* item : s.tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) {
        if (!item->data(0, Qt::UserRole).isValid()) continue;
        const auto& input = s.controls[item->data(0, Qt::UserRole).toInt()];
        const auto& monitor = *s.monitors.at(input.device);
        int code = input.code;
        if (code < 0) code = -code <= static_cast<int>(monitor.buttons.size()) ? monitor.buttons[-code - 1] : -1;
        if (!monitor.device || code < 0 || !monitor.notifier || !monitor.notifier->isEnabled()) {
            item->setText(2, "○"); item->setToolTip(2, "Offline"); continue;
        }
        const int type = input.kind == ControlKind::Button ? EV_KEY : EV_ABS;
        if (!libevdev_has_event_code(monitor.device, type, code)) { item->setText(2, "○ Unavailable"); continue; }
        const int value = libevdev_get_event_value(monitor.device, type, code);
        const bool active = input.kind == ControlKind::Button ? value != 0 :
            input.kind == ControlKind::HatDirection ? value * input.direction > 0 :
            monitor.motion.contains(code) && s.clock.elapsed() - monitor.motion.at(code) < 180;
        item->setText(2, (active ? "● " : "○ ") + (type == EV_ABS ? QString::number(value) : active ? "Held" : "Released"));
        item->setToolTip(2, monitor.issue);
    }
}
