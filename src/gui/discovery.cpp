#include "discovery.hpp"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace profile_setup {

std::vector<DiscoveredDevice> discover_devices(const std::string& input_root) {
    namespace fs = std::filesystem;
    std::map<std::string, DiscoveredDevice> devices;
    for (const auto* directory : {"by-id", "by-path"}) {
        std::error_code error;
        const auto root = fs::path(input_root) / directory;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            const auto& entry = *it;
            const auto filename = entry.path().filename().string();
            if (filename.find("event-joystick") == std::string::npos) continue;
            const bool link = entry.is_symlink(error);
            if (error) { error.clear(); continue; }
            if (!link) continue;
            const auto path = entry.path().string();
            const auto target = fs::canonical(entry.path(), error);
            // Keep missing links visible so the user can distinguish absent hardware.
            const auto identity = error ? path : target.string();
            error.clear();
            auto [found, inserted] = devices.try_emplace(identity);
            auto& device = found->second;
            if (!inserted) {
                device.aliases.push_back(path);
                continue;
            }
            device.path = path;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) { device.issue = std::strerror(errno); continue; }
            libevdev* evdev = nullptr;
            const int result = libevdev_new_from_fd(fd, &evdev);
            if (result < 0) {
                device.issue = std::strerror(-result);
                close(fd);
                continue;
            }
            device.name = libevdev_get_name(evdev) ? libevdev_get_name(evdev) : "Unknown controller";
            // Use exactly the same ordering as EvdevInput::resolve_keys().
            for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code)
                if (libevdev_has_event_code(evdev, EV_KEY, code)) device.buttons.push_back(code);
            for (int code = BTN_MISC; code < BTN_JOYSTICK; ++code)
                if (libevdev_has_event_code(evdev, EV_KEY, code)) device.buttons.push_back(code);
            for (int code = 0; code <= ABS_MAX; ++code) {
                if (!libevdev_has_event_code(evdev, EV_ABS, code)) continue;
                device.axes.push_back(code);
                if (code >= ABS_HAT0X && code <= ABS_HAT3Y) continue;
                if (const auto* info = libevdev_get_abs_info(evdev, code); info && info->minimum < info->maximum) {
                    const int neutral = info->minimum < 0 && info->maximum >= 0 ? 0 :
                        info->minimum + (static_cast<long long>(info->maximum) - info->minimum + 1) / 2;
                    device.axis_ranges.emplace(code, joystick_penguin::AxisRange{info->minimum, info->maximum, neutral});
                }
            }
            libevdev_free(evdev);
            close(fd);
        }
    }
    std::vector<DiscoveredDevice> result;
    result.reserve(devices.size());
    for (auto& [identity, device] : devices) {
        (void)identity;
        result.push_back(std::move(device));
    }
    return result;
}

} // namespace profile_setup
