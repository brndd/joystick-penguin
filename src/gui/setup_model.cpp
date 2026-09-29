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
    for (auto& label : config.output_labels)
        if (label.device == from) label.device = to;
    for (auto& [name, inputs] : config.modifiers) {
        (void)name;
        for (auto& input : inputs)
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
    const auto kind = config.devices.at(name).kind;
    if (kind == DeviceKind::Evdev) {
        std::erase_if(config.bindings, [&](const Binding& binding) { return binding.input.device == name; });
        for (auto& [modifier, inputs] : config.modifiers) {
            (void)modifier;
            std::erase_if(inputs, [&](const Control& input) { return input.device == name; });
        }
    } else {
        auto remove_actions = [&](std::vector<Action>& actions) {
            std::erase_if(actions, [&](const Action& action) {
                return std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, ModeAction>) return false;
                    else return value.device == name;
                }, action);
            });
        };
        for (auto& binding : config.bindings) {
            remove_actions(binding.actions);
            if (binding.tap_hold) {
                remove_actions(binding.tap_hold->tap);
                remove_actions(binding.tap_hold->hold);
            }
        }
        std::erase_if(config.bindings, [](const Binding& binding) {
            return binding.actions.empty() && (!binding.tap_hold ||
                (binding.tap_hold->tap.empty() && binding.tap_hold->hold.empty()));
        });
    }
    config.devices.erase(name);
    std::erase_if(config.input_labels, [&](const auto& label) { return label.input.device == name; });
    std::erase_if(config.output_labels, [&](const auto& label) { return label.device == name; });
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

void mirror_button_labels(Config& config, const std::string& source, const std::string& target) {
    if (!config.devices.contains(source) || config.devices.at(source).kind != DeviceKind::Evdev)
        throw ConfigError("unknown input controller '" + source + "'");
    if (!config.devices.contains(target) || config.devices.at(target).kind != DeviceKind::Uinput)
        throw ConfigError("unknown output device '" + target + "'");
    std::erase_if(config.output_labels, [&](const OutputLabel& label) {
        return label.device == target && label.button >= 1 && label.button <= joystick_button_count;
    });
    for (const auto& label : config.input_labels)
        if (label.input.device == source && label.input.kind == ControlKind::Button &&
            label.input.code < 0 && label.input.code >= -joystick_button_count)
            config.output_labels.push_back({target, -label.input.code, label.label});
}

void refresh_virtual_device(Config& config, const std::string& source, const std::string& target,
                            const std::map<int, AxisRange>& axes) {
    mirror_button_labels(config, source, target);
    auto& output = config.devices.at(target);
    for (const auto& [code, range] : axes) output.axes.insert_or_assign(code, range);
}

} // namespace profile_setup
