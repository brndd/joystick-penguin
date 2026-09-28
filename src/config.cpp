#include "joystick_penguin/config.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace joystick_penguin {
namespace {

void expect_map(const YAML::Node& node, const std::string& where) {
    if (!node.IsMap()) throw ConfigError(where + " must be a mapping");
}

void expect_sequence(const YAML::Node& node, const std::string& where) {
    if (!node.IsSequence()) throw ConfigError(where + " must be a sequence");
}

void keys(const YAML::Node& node, const std::string& where,
          std::initializer_list<std::string_view> allowed) {
    expect_map(node, where);
    std::set<std::string> seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) throw ConfigError(where + " has a non-string key");
        const auto key = entry.first.as<std::string>();
        if (!seen.insert(key).second) throw ConfigError(where + " has duplicate key '" + key + "'");
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw ConfigError(where + " has unknown key '" + key + "'");
    }
}

YAML::Node required(const YAML::Node& node, const char* name, const std::string& where) {
    const auto child = node[name];
    if (!child || child.IsNull()) throw ConfigError(where + " is missing '" + name + "'");
    return child;
}

std::string text(const YAML::Node& node, const std::string& where) {
    if (!node.IsScalar()) throw ConfigError(where + " must be a nonempty string");
    const auto result = node.as<std::string>();
    if (result.empty()) throw ConfigError(where + " must be a nonempty string");
    return result;
}

std::string field(const YAML::Node& node, const char* name, const std::string& where) {
    return text(required(node, name, where), where + "." + name);
}

int integer(const YAML::Node& node, const std::string& where) {
    if (!node.IsScalar()) throw ConfigError(where + " must be an integer");
    int value;
    try {
        value = node.as<int>();
    } catch (const YAML::BadConversion&) {
        throw ConfigError(where + " must be an integer");
    }
    return value;
}

int number(const YAML::Node& node, const std::string& where, int minimum, int maximum) {
    const int value = integer(node, where);
    if (value < minimum || value > maximum)
        throw ConfigError(where + " is out of permitted range");
    return value;
}

bool is_hat_axis(int code) { return code >= ABS_HAT0X && code <= ABS_HAT3Y; }

int hat_axis(const YAML::Node& node, const std::string& where) {
    const int code = number(node, where, 0, ABS_MAX);
    if (!is_hat_axis(code)) throw ConfigError(where + " must be an ABS_HAT* axis code");
    return code;
}

int ordinary_axis(const YAML::Node& node, const std::string& where) {
    const int code = number(node, where, 0, ABS_MAX);
    if (is_hat_axis(code)) throw ConfigError(where + " is a hat axis; use 'hat' instead");
    return code;
}

int direction(const YAML::Node& node, const std::string& where) {
    const int value = integer(node, where);
    if (value != -1 && value != 1) throw ConfigError(where + " must be -1 or 1");
    return value;
}

std::vector<std::string> names(const YAML::Node& node, const std::string& where) {
    expect_sequence(node, where);
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& item : node) {
        const auto name = text(item, where + " entry");
        if (!seen.insert(name).second) throw ConfigError(where + " repeats '" + name + "'");
        result.push_back(name);
    }
    return result;
}

Control input_control(const YAML::Node& node, const std::string& where) {
    keys(node, where, {"device", "button", "button_code", "axis", "hat"});
    const auto button = node["button"];
    const auto button_code = node["button_code"];
    const auto axis = node["axis"];
    const auto hat = node["hat"];
    if (static_cast<int>(bool(button)) + int(bool(button_code)) + int(bool(axis)) + int(bool(hat)) != 1)
        throw ConfigError(where + " requires exactly one of 'button', 'button_code', 'axis', or 'hat'");
    if (button)
        return {field(node, "device", where), ControlKind::Button,
                button_index_key(number(button, where + ".button", 1, 255))};
    if (button_code)
        return {field(node, "device", where), ControlKind::Button,
                number(button_code, where + ".button_code", BTN_MISC, KEY_MAX)};
    if (axis)
        return {field(node, "device", where), ControlKind::AbsoluteAxis,
                ordinary_axis(axis, where + ".axis")};
    keys(hat, where + ".hat", {"axis", "direction"});
    return {field(node, "device", where), ControlKind::HatDirection,
            hat_axis(required(hat, "axis", where + ".hat"), where + ".hat.axis"),
            direction(required(hat, "direction", where + ".hat"), where + ".hat.direction")};
}

std::map<int, AxisRange> parse_axes(const YAML::Node& node, const std::string& where) {
    expect_map(node, where);
    std::map<int, AxisRange> axes;
    for (const auto& entry : node) {
        const int code = ordinary_axis(entry.first, where + " axis code");
        const auto path = where + "." + std::to_string(code);
        keys(entry.second, path, {"min", "max", "neutral"});
        const AxisRange range{integer(required(entry.second, "min", path), path + ".min"),
                              integer(required(entry.second, "max", path), path + ".max"),
                              integer(required(entry.second, "neutral", path), path + ".neutral")};
        if (range.minimum >= range.maximum || range.neutral < range.minimum ||
            range.neutral > range.maximum)
            throw ConfigError(path + " requires min < max and min <= neutral <= max");
        if (!axes.emplace(code, range).second)
            throw ConfigError(where + " repeats absolute axis " + std::to_string(code));
    }
    return axes;
}

bool contains(const std::vector<std::string>& entries, const std::string& name) {
    return std::find(entries.begin(), entries.end(), name) != entries.end();
}

Device parse_device(const YAML::Node& node, const std::string& where) {
    const auto kind = field(node, "kind", where);
    if (kind == "evdev") {
        keys(node, where, {"kind", "path", "grab"});
        Device device{DeviceKind::Evdev, field(node, "path", where), true, ""};
        if (const auto grab = node["grab"]) {
            if (!grab.IsScalar()) throw ConfigError(where + ".grab must be a boolean");
            try {
                device.grab = grab.as<bool>();
            } catch (const YAML::BadConversion&) {
                throw ConfigError(where + ".grab must be a boolean");
            }
        }
        return device;
    }
    if (kind == "uinput") {
        keys(node, where, {"kind", "preset", "axes", "name",
                           "bus", "vendor_id", "product_id"});
        const auto preset = field(node, "preset", where);
        if (preset != "joystick") throw ConfigError(where + ".preset must be 'joystick'");
        Device device{DeviceKind::Uinput, "", true, preset};
        device.axes = joystick_axes();
        if (const auto axes = node["axes"])
            for (const auto& [code, range] : parse_axes(axes, where + ".axes"))
                device.axes.insert_or_assign(code, range);
        if (const auto name = node["name"])
            device.virtual_name = text(name, where + ".name");
        if (const auto bus = node["bus"]) {
            const auto value = text(bus, where + ".bus");
            if (value == "usb") device.bus = VirtualBus::Usb;
            else if (value == "virtual") device.bus = VirtualBus::Virtual;
            else throw ConfigError(where + ".bus must be 'usb' or 'virtual'");
        }
        if (const auto vendor = node["vendor_id"])
            device.vendor_id = number(vendor, where + ".vendor_id", 0, 0xffff);
        if (const auto product = node["product_id"])
            device.product_id = number(product, where + ".product_id", 0, 0xffff);
        return device;
    }
    throw ConfigError(where + ".kind is unsupported: '" + kind + "'");
}

std::map<std::string, Device> parse_devices(const YAML::Node& node) {
    expect_map(node, "devices");
    std::map<std::string, Device> devices;
    for (const auto& entry : node) {
        const auto name = text(entry.first, "device name");
        // Parse only once the name has been checked; YAML permits duplicate keys.
        if (devices.count(name)) throw ConfigError("duplicate device '" + name + "'");
        devices.emplace(name, parse_device(entry.second, "devices." + name));
    }
    return devices;
}

void parse_modes(const YAML::Node& node, Config& config) {
    keys(node, "modes", {"initial", "names"});
    config.initial_mode = field(node, "initial", "modes");
    config.modes = names(required(node, "names", "modes"), "modes.names");
}

std::map<std::string, Control> parse_modifiers(const YAML::Node& node) {
    std::map<std::string, Control> modifiers;
    if (!node) return modifiers;
    expect_map(node, "modifiers");
    for (const auto& entry : node) {
        const auto name = text(entry.first, "modifier name");
        if (modifiers.count(name)) throw ConfigError("duplicate modifier '" + name + "'");
        const auto where = "modifiers." + name;
        keys(entry.second, where, {"input"});
        auto input = input_control(required(entry.second, "input", where), where + ".input");
        if (input.kind != ControlKind::Button) throw ConfigError(where + " must use a button");
        modifiers.emplace(name, std::move(input));
    }
    return modifiers;
}

Action parse_action(const YAML::Node& node, const std::string& where) {
    const auto type = field(node, "type", where);
    if (type == "button") {
        keys(node, where, {"type", "device", "button", "button_code"});
        if (static_cast<bool>(node["button"]) == static_cast<bool>(node["button_code"]))
            throw ConfigError(where + " requires exactly one of 'button' or 'button_code'");
        if (const auto index = node["button"])
            return ButtonAction{field(node, "device", where),
                                joystick_button_code(number(index, where + ".button", 1,
                                                            joystick_button_count))};
        return ButtonAction{field(node, "device", where),
                            number(node["button_code"], where + ".button_code", BTN_MISC, KEY_MAX)};
    }
    if (type == "hat") {
        keys(node, where, {"type", "device", "axis", "direction"});
        return HatAction{field(node, "device", where),
                         hat_axis(required(node, "axis", where), where + ".axis"),
                         direction(required(node, "direction", where), where + ".direction")};
    }
    if (type == "axis") {
        keys(node, where, {"type", "device", "axis", "invert"});
        bool invert = false;
        if (const auto value = node["invert"]) {
            try {
                invert = value.as<bool>();
            } catch (const YAML::BadConversion&) {
                throw ConfigError(where + ".invert must be a boolean");
            }
        }
        return AxisAction{field(node, "device", where),
                          ordinary_axis(required(node, "axis", where), where + ".axis"), invert};
    }
    if (type == "mode") {
        keys(node, where, {"type", "mode"});
        return ModeAction{field(node, "mode", where)};
    }
    throw ConfigError(where + ".type is unsupported");
}

std::vector<Action> parse_actions(const YAML::Node& node, const std::string& where) {
    const auto action = node["action"];
    const auto actions = node["actions"];
    if (static_cast<bool>(action) == static_cast<bool>(actions))
        throw ConfigError(where + " requires exactly one of 'action' or 'actions'");
    std::vector<Action> result;
    if (action) {
        result.push_back(parse_action(action, where + ".action"));
    } else {
        expect_sequence(actions, where + ".actions");
        if (actions.size() == 0) throw ConfigError(where + ".actions cannot be empty");
        for (std::size_t i = 0; i < actions.size(); ++i)
            result.push_back(parse_action(actions[i], where + ".actions[" +
                                          std::to_string(i) + "]"));
    }
    return result;
}

Binding parse_binding(const YAML::Node& node, std::size_t index) {
    const auto where = "bindings[" + std::to_string(index) + "]";
    keys(node, where, {"input", "modes", "modifiers", "action", "actions",
                       "threshold_ms", "tap_ms", "tap", "hold"});
    Binding binding;
    binding.input = input_control(required(node, "input", where), where + ".input");
    binding.modes = names(required(node, "modes", where), where + ".modes");
    if (binding.modes.empty()) throw ConfigError(where + ".modes cannot be empty");
    if (const auto modifiers = node["modifiers"])
        binding.modifiers = names(modifiers, where + ".modifiers");
    if (node["threshold_ms"] || node["tap_ms"] || node["tap"] || node["hold"]) {
        if (binding.input.kind != ControlKind::Button)
            throw ConfigError(where + " tap/hold requires a button input");
        if (node["action"] || node["actions"])
            throw ConfigError(where + " cannot mix tap/hold with action or actions");
        if (!node["tap"] && !node["hold"])
            throw ConfigError(where + " requires tap or hold");
        TapHold timing{number(required(node, "threshold_ms", where), where + ".threshold_ms",
                              1, std::numeric_limits<int>::max()), 50, {}, {}};
        if (const auto tap_ms = node["tap_ms"])
            timing.tap_ms = number(tap_ms, where + ".tap_ms", 1,
                                   std::numeric_limits<int>::max());
        if (node["tap"]) {
            keys(node["tap"], where + ".tap", {"action", "actions"});
            timing.tap = parse_actions(node["tap"], where + ".tap");
        }
        if (node["hold"]) {
            keys(node["hold"], where + ".hold", {"action", "actions"});
            timing.hold = parse_actions(node["hold"], where + ".hold");
        }
        binding.tap_hold = std::move(timing);
    } else {
        binding.actions = parse_actions(node, where);
    }
    return binding;
}

std::vector<Binding> parse_bindings(const YAML::Node& node) {
    expect_sequence(node, "bindings");
    std::vector<Binding> bindings;
    bindings.reserve(node.size());
    for (std::size_t index = 0; index < node.size(); ++index)
        bindings.push_back(parse_binding(node[index], index));
    return bindings;
}

void check_device(const Config& config, const std::string& name, DeviceKind kind,
                  const std::string& where) {
    const auto found = config.devices.find(name);
    if (found == config.devices.end() || found->second.kind != kind)
        throw ConfigError(where + " references unknown or wrong-kind device '" + name + "'");
}

bool overlaps(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const auto& name : a)
        if (contains(b, name)) return true;
    return false;
}

void validate_binding(const Config& config, std::size_t index) {
    const auto& binding = config.bindings[index];
    const auto where = "bindings[" + std::to_string(index) + "]";
    check_device(config, binding.input.device, DeviceKind::Evdev, where + ".input");
    auto validate_actions = [&](const std::vector<Action>& actions) {
        std::set<std::tuple<std::string, int, int>> targets;
        for (const auto& action : actions) {
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ModeAction>) {
                    if (binding.input.kind != ControlKind::Button)
                        throw ConfigError(where + " mode actions require a button input");
                    if (!contains(config.modes, value.mode))
                        throw ConfigError(where + " references unknown mode '" + value.mode + "'");
                } else {
                    check_device(config, value.device, DeviceKind::Uinput, where + ".actions");
                    constexpr bool button = std::is_same_v<T, ButtonAction>;
                    constexpr bool axis = std::is_same_v<T, AxisAction>;
                    const int event_type = button ? EV_KEY : EV_ABS;
                    if (!targets.emplace(value.device, event_type, value.code).second)
                        throw ConfigError(where + " repeats output control " + value.device + ":" +
                                          std::to_string(value.code));
                    if constexpr (axis) {
                        if (binding.input.kind != ControlKind::AbsoluteAxis)
                            throw ConfigError(where + " axis actions require an absolute-axis input");
                        if (!config.devices.at(value.device).axes.contains(value.code))
                            throw ConfigError(where + " requires declared output axis " + value.device + ":" +
                                              std::to_string(value.code));
                    } else if (binding.input.kind == ControlKind::AbsoluteAxis) {
                        throw ConfigError(where + " absolute-axis inputs require axis actions");
                    }
                }
            }, action);
        }
    };
    validate_actions(binding.actions);
    if (binding.tap_hold) {
        validate_actions(binding.tap_hold->tap);
        validate_actions(binding.tap_hold->hold);
    }
    for (const auto& mode : binding.modes)
        if (!contains(config.modes, mode))
            throw ConfigError(where + " references unknown mode '" + mode + "'");
    for (const auto& modifier : binding.modifiers) {
        const auto found = config.modifiers.find(modifier);
        if (found == config.modifiers.end())
            throw ConfigError(where + " references unknown modifier '" + modifier + "'");
        if (found->second.device == binding.input.device &&
            found->second.code == binding.input.code)
            throw ConfigError(where + " cannot require its own modifier '" + modifier + "'");
    }
}

void validate_binding_precedence(const Config& config, std::size_t index) {
    const auto& binding = config.bindings[index];
    for (std::size_t previous = 0; previous < index; ++previous) {
        const auto& other = config.bindings[previous];
        if (other.input.device == binding.input.device &&
            other.input.kind == binding.input.kind &&
            other.input.code == binding.input.code &&
            other.input.direction == binding.input.direction &&
            other.modifiers.size() == binding.modifiers.size() &&
            overlaps(other.modes, binding.modes)) {
            throw ConfigError("bindings[" + std::to_string(index) + "] conflicts with bindings[" +
                              std::to_string(previous) + "] at equal modifier specificity");
        }
    }
}

void validate_config(const Config& config) {
    if (!contains(config.modes, config.initial_mode))
        throw ConfigError("modes.initial is not listed in modes.names");

    for (const auto& [name, input] : config.modifiers)
        check_device(config, input.device, DeviceKind::Evdev, "modifiers." + name);

    for (std::size_t i = 0; i < config.input_labels.size(); ++i) {
        const auto& label = config.input_labels[i];
        check_device(config, label.input.device, DeviceKind::Evdev, "input_labels[" + std::to_string(i) + "]");
        for (std::size_t j = 0; j < i; ++j)
            if (config.input_labels[j].input == label.input)
                throw ConfigError("duplicate input identity in input_labels[" + std::to_string(i) + "]");
    }

    for (std::size_t index = 0; index < config.bindings.size(); ++index) {
        validate_binding(config, index);
        validate_binding_precedence(config, index);
    }
}

Config parse(const YAML::Node& root) {
    keys(root, "profile", {"version", "devices", "modes", "modifiers", "bindings", "input_labels"});
    if (number(required(root, "version", "profile"), "version", 0,
               std::numeric_limits<int>::max()) != 1)
        throw ConfigError("unsupported profile version (expected 1)");

    Config config;
    config.devices = parse_devices(required(root, "devices", "profile"));
    parse_modes(required(root, "modes", "profile"), config);
    config.modifiers = parse_modifiers(root["modifiers"]);
    config.bindings = parse_bindings(required(root, "bindings", "profile"));
    if (const auto labels = root["input_labels"]) {
        expect_sequence(labels, "input_labels");
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto where = "input_labels[" + std::to_string(i) + "]";
            keys(labels[i], where, {"input", "label"});
            config.input_labels.push_back({input_control(required(labels[i], "input", where), where + ".input"),
                                           field(labels[i], "label", where)});
        }
    }
    validate_config(config);
    return config;
}

} // namespace

Config load_config(const std::string& yaml_text) {
    try {
        return parse(YAML::Load(yaml_text));
    } catch (const YAML::Exception& error) {
        throw ConfigError(std::string("invalid YAML: ") + error.what());
    }
}

Config load_config_file(const std::string& path) {
    try {
        return parse(YAML::LoadFile(path));
    } catch (const YAML::Exception& error) {
        throw ConfigError(path + ": " + error.what());
    }
}

} // namespace joystick_penguin
