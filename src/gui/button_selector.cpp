#include "button_selector.hpp"
#include "control_browser.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <algorithm>
#include <optional>
#include <vector>

using namespace joystick_penguin;

namespace {
// This is the evdev implementation of physical indexed-button enumeration.
// Keep its ordering in step with EvdevInput::resolve_keys(). Other controller
// backends can supply their own indexed choices without changing the forms.
std::optional<std::vector<int>> evdevButtons(const Device& device) {
    const int fd = ::open(device.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    libevdev* evdev = nullptr;
    if (libevdev_new_from_fd(fd, &evdev) < 0) { ::close(fd); return std::nullopt; }
    std::vector<int> codes;
    for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code)
        if (libevdev_has_event_code(evdev, EV_KEY, code)) codes.push_back(code);
    for (int code = BTN_MISC; code < BTN_JOYSTICK; ++code)
        if (libevdev_has_event_code(evdev, EV_KEY, code)) codes.push_back(code);
    libevdev_free(evdev);
    ::close(fd);
    return codes;
}
}

ButtonSelector::ButtonSelector(const Config& config, Target target, QWidget* parent)
    : QComboBox(parent), config_(config), target_(target) {
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(17);
}

void ButtonSelector::setDevice(const std::string& name, int number, bool preserveMissing) {
    const QSignalBlocker blocker(this);
    clear();
    if (target_ == Target::Virtual) {
        for (int i = 1; i <= joystick_button_count; ++i)
            addItem(labeledOutputButton(config_, name, i), i);
    } else {
        auto found = config_.devices.find(name);
        std::optional<std::vector<int>> codes;
        if (found != config_.devices.end() && found->second.kind == DeviceKind::Evdev)
            codes = evdevButtons(found->second);
        const bool offline = !codes;
        const int count = offline ? 255 : std::min(static_cast<int>(codes->size()), 255);
        for (int i = 1; i <= count; ++i) {
            const QString title = labeledInput(config_, {name, ControlKind::Button, -i});
            addItem(title, i);
            if (offline) setItemData(i - 1, "Availability unverified (controller offline or unreadable)", Qt::ToolTipRole);
        }
        setToolTip(offline ? "Controller offline or unreadable: button availability is unverified." : QString{});
    }
    // A readable device with no buttons offers no replacement for an existing
    // indexed choice; retain it visibly instead of committing button zero.
    setNumber(number, preserveMissing || count() == 0);
}

void ButtonSelector::setNumber(int number, bool preserveMissing) {
    const int index = findData(number);
    if (index >= 0) { setCurrentIndex(index); return; }
    if (preserveMissing && number > 0) {
        addItem(QString("Button %1 (unavailable)").arg(number), number);
        setItemData(count() - 1, "Existing button is not available on this controller", Qt::ToolTipRole);
        setCurrentIndex(count() - 1);
        static_cast<QStandardItemModel*>(model())->item(count() - 1)->setEnabled(false);
    } else setCurrentIndex(count() ? 0 : -1);
}
