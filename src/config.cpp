#include "joystick_penguin/config.hpp"

#include <linux/input-event-codes.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
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

int number(const YAML::Node& node, const std::string& where, int minimum, int maximum) {
    if (!node.IsScalar()) throw ConfigError(where + " must be an integer");
    int value;
    try {
        value = node.as<int>();
    } catch (const YAML::BadConversion&) {
        throw ConfigError(where + " must be an integer");
    }
    if (value < minimum || value > maximum)
        throw ConfigError(where + " is outside the supported Linux event-code range");
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
    keys(node, where, {"device", "button", "axis"});
    const auto button = node["button"];
    const auto axis = node["axis"];
    if (static_cast<bool>(button) == static_cast<bool>(axis))
        throw ConfigError(where + " requires exactly one of 'button' or 'axis'");
    if (button)
        return {field(node, "device", where), ControlKind::Button,
                number(button, where + ".button", BTN_MISC, KEY_MAX)};
    return {field(node, "device", where), ControlKind::AbsoluteAxis,
            number(axis, where + ".axis", 0, ABS_MAX)};
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
        keys(node, where, {"kind", "preset"});
        const auto preset = field(node, "preset", where);
        if (preset != "joystick") throw ConfigError(where + ".preset must be 'joystick'");
        return {DeviceKind::Uinput, "", true, preset};
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

ButtonAction parse_action(const YAML::Node& node, const std::string& where) {
    keys(node, where, {"type", "device", "button"});
    if (field(node, "type", where) != "button")
        throw ConfigError(where + ".type is unsupported");
    return {field(node, "device", where),
            number(required(node, "button", where), where + ".button", BTN_MISC, KEY_MAX)};
}

Binding parse_binding(const YAML::Node& node, std::size_t index) {
    const auto where = "bindings[" + std::to_string(index) + "]";
    keys(node, where, {"input", "modes", "modifiers", "action"});
    Binding binding;
    binding.input = input_control(required(node, "input", where), where + ".input");
    if (binding.input.kind != ControlKind::Button)
        throw ConfigError(where + " only button inputs are supported in this milestone");
    binding.modes = names(required(node, "modes", where), where + ".modes");
    if (binding.modes.empty()) throw ConfigError(where + ".modes cannot be empty");
    if (const auto modifiers = node["modifiers"])
        binding.modifiers = names(modifiers, where + ".modifiers");
    binding.action = parse_action(required(node, "action", where), where + ".action");
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
    check_device(config, binding.action.device, DeviceKind::Uinput, where + ".action");
    for (const auto& mode : binding.modes)
        if (!contains(config.modes, mode))
            throw ConfigError(where + " references unknown mode '" + mode + "'");
    for (const auto& modifier : binding.modifiers)
        if (!config.modifiers.count(modifier))
            throw ConfigError(where + " references unknown modifier '" + modifier + "'");
}

void validate_binding_precedence(const Config& config, std::size_t index) {
    const auto& binding = config.bindings[index];
    for (std::size_t previous = 0; previous < index; ++previous) {
        const auto& other = config.bindings[previous];
        if (other.input.device == binding.input.device &&
            other.input.kind == binding.input.kind &&
            other.input.code == binding.input.code &&
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

    for (std::size_t index = 0; index < config.bindings.size(); ++index) {
        validate_binding(config, index);
        validate_binding_precedence(config, index);
    }
}

Config parse(const YAML::Node& root) {
    keys(root, "profile", {"version", "devices", "modes", "modifiers", "bindings"});
    if (number(required(root, "version", "profile"), "version", 0,
               std::numeric_limits<int>::max()) != 1)
        throw ConfigError("unsupported profile version (expected 1)");

    Config config;
    config.devices = parse_devices(required(root, "devices", "profile"));
    parse_modes(required(root, "modes", "profile"), config);
    config.modifiers = parse_modifiers(root["modifiers"]);
    config.bindings = parse_bindings(required(root, "bindings", "profile"));
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
