#include "joystick_penguin/button_router.hpp"

namespace joystick_penguin {

ButtonRouter::ButtonRouter(const Config& config) {
    if (config.modes.size() != 1 || !config.modifiers.empty())
        throw ConfigError("hardware path supports one mode and no modifiers; gesture engine is next");
    for (const auto& binding : config.bindings) {
        if (!binding.modifiers.empty())
            throw ConfigError("hardware path does not yet support modified bindings");
        bindings_.emplace(Key{binding.input.device, binding.input.code},
                          Key{binding.action.device, binding.action.code});
    }
}

void ButtonRouter::release(const Key& source, std::vector<OutputEvent>& output) {
    const auto press = pressed_.find(source);
    if (press == pressed_.end()) return;
    const Key target = press->second;
    pressed_.erase(press);
    const auto owner = owners_.find(target);
    if (--owner->second == 0) {
        owners_.erase(owner);
        output.push_back({target.first, OutputEventKind::Button, target.second, 0});
    }
}

void ButtonRouter::release_device(const std::string& device, std::vector<OutputEvent>& output) {
    for (auto it = pressed_.begin(); it != pressed_.end();) {
        if (it->first.first == device) {
            const Key source = it->first;
            ++it;
            release(source, output);
        } else {
            ++it;
        }
    }
}

std::vector<OutputEvent> ButtonRouter::process(const InputEvent& event) {
    std::vector<OutputEvent> output;
    if (event.kind == InputEventKind::SyncLost || event.kind == InputEventKind::Disconnected) {
        release_device(event.device, output);
    } else if (event.kind == InputEventKind::Button) {
        const Key source{event.device, event.code};
        if (event.value == 0) {
            release(source, output);
        } else if (event.value == 1 && !pressed_.count(source)) {
            if (const auto binding = bindings_.find(source); binding != bindings_.end()) {
                const auto& target = binding->second;
                pressed_.emplace(source, target);
                if (++owners_[target] == 1)
                    output.push_back({target.first, OutputEventKind::Button, target.second, 1});
            }
        }
    }
    return output;
}

std::vector<OutputEvent> ButtonRouter::release_all() {
    std::vector<OutputEvent> output;
    while (!pressed_.empty()) release(pressed_.begin()->first, output);
    return output;
}

} // namespace joystick_penguin
