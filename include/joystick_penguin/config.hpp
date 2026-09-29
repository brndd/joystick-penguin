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
    VirtualBus bus = VirtualBus::Usb;
    bool operator==(const Device&) const = default;
};

// Typed input buttons use negative keys for one-based indices; positive keys
// are explicit Linux EV_KEY codes. Axis and hat codes remain Linux EV_ABS.
enum class ControlKind { Button, HatDirection, AbsoluteAxis };

struct Control {
    std::string device;
    ControlKind kind;
    int code;
    int direction = 0; // -1 or +1 for one component of a physical hat.
    bool operator==(const Control&) const = default;
};

struct ButtonAction {
    std::string device;
    int code;
    bool operator==(const ButtonAction&) const = default;
};

struct HatAction {
    std::string device;
    int code;
    int direction;
    bool operator==(const HatAction&) const = default;
};

struct AxisAction {
    std::string device;
    int code;
    bool invert = false;
    bool operator==(const AxisAction&) const = default;
};

struct ModeAction {
    std::string mode;
    bool operator==(const ModeAction&) const = default;
};

using Action = std::variant<ButtonAction, HatAction, AxisAction, ModeAction>;

struct TapHold {
    int threshold_ms;
    int tap_ms = 50; // How long tap outputs stay asserted before release.
    std::vector<Action> tap;
    std::vector<Action> hold;
    bool operator==(const TapHold&) const = default;
};

struct Binding {
    Control input;
    std::vector<std::string> modes;
    std::vector<std::string> modifiers;
    std::vector<Action> actions;
    std::optional<TapHold> tap_hold;
    bool operator==(const Binding&) const = default;
};

struct InputLabel {
    Control input;
    std::string label;
    bool operator==(const InputLabel&) const = default;
};

struct OutputLabel {
    std::string device;
    int button; // One-based joystick preset button number.
    std::string label;
    bool operator==(const OutputLabel&) const = default;
};

struct Config {
    std::map<std::string, Device> devices;
    std::string initial_mode;
    std::vector<std::string> modes;
    std::map<std::string, std::vector<Control>> modifiers; // Any assigned button activates the named modifier.
    std::vector<Binding> bindings;
    std::vector<InputLabel> input_labels;
    std::vector<OutputLabel> output_labels;
    bool operator==(const Config&) const = default;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Both entry points validate the entire document before returning a typed config.
Config load_config(const std::string& yaml_text);
Config load_config_file(const std::string& path);

// Serialize the supported typed model as version 1 YAML. This normalizes layout,
// drops comments, and expands aliases. Validation uses the same loader as --check.
std::string serialize_config(const Config& config);
void validate_edited_config(const Config& config);
struct ConfigIssue {
    std::string message;
    std::vector<std::size_t> bindings; // Both locations for a precedence conflict.
};
std::vector<ConfigIssue> config_issues(const Config& config);
// Validate both the edited model and the serialized document before atomically
// replacing path. Throws ConfigError without replacing the original on failure.
void save_config_file(const Config& config, const std::string& path);

} // namespace joystick_penguin
