#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace joystick_penguin {

enum class DeviceKind { Evdev, Uinput };
enum class VirtualBus { Virtual, Usb };

struct AxisRange {
    int minimum;
    int maximum;
    int neutral;
    bool operator==(const AxisRange&) const = default;
};

struct Device {
    DeviceKind kind;
    std::string path;    // Stable evdev path, e.g. /dev/input/by-id/...
    bool grab = true;    // Applies only to evdev devices.
    std::string preset;  // Applies only to uinput devices.
    std::map<int, AxisRange> axes = {}; // Virtual non-hat EV_ABS capabilities.
    int vendor_id = 1; // Virtual USB-style identity; applies only to uinput.
    int product_id = 1;
    std::string virtual_name = ""; // Defaults to "JP " + the configured device name.
    VirtualBus bus = VirtualBus::Virtual;
};

// Typed input buttons use negative keys for one-based indices; positive keys
// are explicit Linux EV_KEY codes. Axis and hat codes remain Linux EV_ABS.
enum class ControlKind { Button, HatDirection, AbsoluteAxis };

struct Control {
    std::string device;
    ControlKind kind;
    int code;
    int direction = 0; // -1 or +1 for one component of a physical hat.
};

struct ButtonAction {
    std::string device;
    int code;
};

struct HatAction {
    std::string device;
    int code;
    int direction;
};

struct AxisAction {
    std::string device;
    int code;
    bool invert = false;
};

struct ModeAction {
    std::string mode;
};

using Action = std::variant<ButtonAction, HatAction, AxisAction, ModeAction>;

struct TapHold {
    int threshold_ms;
    int tap_ms = 50; // How long tap outputs stay asserted before release.
    std::vector<Action> tap;
    std::vector<Action> hold;
};

struct Binding {
    Control input;
    std::vector<std::string> modes;
    std::vector<std::string> modifiers;
    std::vector<Action> actions;
    std::optional<TapHold> tap_hold;
};

struct Config {
    std::map<std::string, Device> devices;
    std::string initial_mode;
    std::vector<std::string> modes;
    std::map<std::string, Control> modifiers;
    std::vector<Binding> bindings;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Both entry points validate the entire document before returning a typed config.
Config load_config(const std::string& yaml_text);
Config load_config_file(const std::string& path);

} // namespace joystick_penguin
