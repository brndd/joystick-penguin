#include "setup_model.hpp"

#include "joystick_penguin/joystick_preset.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace profile_setup {
namespace {

using namespace joystick_penguin;

void require_name(const std::string& name) {
    if (name.empty()) throw ConfigError("name must not be empty");
}

template<class Map> void rename_key(Map& map, const std::string& from, const std::string& to) {
    require_name(to);
    if (!map.contains(from)) throw ConfigError("unknown name '" + from + "'");
    if (from == to) return;
    if (map.contains(to)) throw ConfigError("name '" + to + "' is already in use");
    auto node = map.extract(from);
    node.key() = to;
    map.insert(std::move(node));
}

template<class F> void for_actions(Config& config, F visit) {
    for (auto& binding : config.bindings) {
        auto apply = [&](auto& actions) { for (auto& action : actions) visit(action); };
        apply(binding.actions);
        if (binding.tap_hold) {
            apply(binding.tap_hold->tap);
            apply(binding.tap_hold->hold);
        }
    }
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

void rename_device(Config& config, const std::string& from, const std::string& to) {
    rename_key(config.devices, from, to);
    if (from == to) return;
    for (auto& label : config.input_labels)
        if (label.input.device == from) label.input.device = to;
    for (auto& [name, input] : config.modifiers) {
        (void)name;
        if (input.device == from) input.device = to;
    }
    for (auto& binding : config.bindings)
        if (binding.input.device == from) binding.input.device = to;
    for_actions(config, [&](Action& action) {
        std::visit([&](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<T, ModeAction>)
                if (value.device == from) value.device = to;
        }, action);
    });
}

void remove_device(Config& config, const std::string& name) {
    if (!config.devices.contains(name)) throw ConfigError("unknown device '" + name + "'");
    for (const auto& [modifier, input] : config.modifiers)
        if (input.device == name)
            throw ConfigError("device '" + name + "' is used by modifier '" + modifier + "'");
    for (std::size_t i = 0; i < config.bindings.size(); ++i)
        if (config.bindings[i].input.device == name)
            throw ConfigError("device '" + name + "' is used by bindings[" + std::to_string(i) + "].input");
    for_actions(config, [&](const Action& action) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<T, ModeAction>)
                if (value.device == name)
                    throw ConfigError("device '" + name + "' is used by a binding action");
        }, action);
    });
    config.devices.erase(name);
    std::erase_if(config.input_labels, [&](const auto& label) { return label.input.device == name; });
}

void rename_mode(Config& config, const std::string& from, const std::string& to) {
    require_name(to);
    if (!contains(config.modes, from)) throw ConfigError("unknown mode '" + from + "'");
    if (from == to) return;
    if (contains(config.modes, to)) throw ConfigError("mode '" + to + "' already exists");
    for (auto& name : config.modes) if (name == from) name = to;
    if (config.initial_mode == from) config.initial_mode = to;
    for (auto& binding : config.bindings)
        for (auto& name : binding.modes) if (name == from) name = to;
    for_actions(config, [&](Action& action) {
        if (auto* mode = std::get_if<ModeAction>(&action); mode && mode->mode == from) mode->mode = to;
    });
}

void remove_mode(Config& config, const std::string& name) {
    if (!contains(config.modes, name)) throw ConfigError("unknown mode '" + name + "'");
    if (config.initial_mode == name) throw ConfigError("choose a different initial mode before removing '" + name + "'");
    for (std::size_t i = 0; i < config.bindings.size(); ++i)
        if (contains(config.bindings[i].modes, name))
            throw ConfigError("mode '" + name + "' is used by bindings[" + std::to_string(i) + "].modes");
    for_actions(config, [&](const Action& action) {
        if (const auto* mode = std::get_if<ModeAction>(&action); mode && mode->mode == name)
            throw ConfigError("mode '" + name + "' is used by a binding action");
    });
    std::erase(config.modes, name);
}

void rename_modifier(Config& config, const std::string& from, const std::string& to) {
    rename_key(config.modifiers, from, to);
    if (from == to) return;
    for (auto& binding : config.bindings)
        for (auto& name : binding.modifiers) if (name == from) name = to;
}

void remove_modifier(Config& config, const std::string& name) {
    if (!config.modifiers.contains(name)) throw ConfigError("unknown modifier '" + name + "'");
    for (std::size_t i = 0; i < config.bindings.size(); ++i)
        if (contains(config.bindings[i].modifiers, name))
            throw ConfigError("modifier '" + name + "' is used by bindings[" + std::to_string(i) + "].modifiers");
    config.modifiers.erase(name);
}

void remove_axis(Config& config, const std::string& device, int code) {
    auto found = config.devices.find(device);
    if (found == config.devices.end() || found->second.kind != DeviceKind::Uinput)
        throw ConfigError("unknown output device '" + device + "'");
    const auto defaults = joystick_axes();
    if (auto preset = defaults.find(code); preset != defaults.end()) {
        found->second.axes.insert_or_assign(code, preset->second);
        return;
    }
    for_actions(config, [&](const Action& action) {
        if (const auto* axis = std::get_if<AxisAction>(&action); axis && axis->device == device && axis->code == code)
            throw ConfigError("output axis " + device + ":" + std::to_string(code) + " is used by a binding");
    });
    found->second.axes.erase(code);
}

} // namespace profile_setup
