#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace joystick_penguin {

enum class DeviceKind { Evdev, Uinput };

struct AxisRange {
    int minimum;
    int maximum;
    int neutral;
};

struct Device {
    DeviceKind kind;
    std::string path;    // Stable evdev path, e.g. /dev/input/by-id/...
    bool grab = true;    // Applies only to evdev devices.
    std::string preset;  // Applies only to uinput devices.
    std::map<int, AxisRange> axes = {}; // Virtual non-hat EV_ABS capabilities.
};

// Codes are Linux evdev event codes, not ordinal button numbers.
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

using Action = std::variant<ButtonAction, HatAction, AxisAction>;

struct Binding {
    Control input;
    std::vector<std::string> modes;
    std::vector<std::string> modifiers;
    std::vector<Action> actions;
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
