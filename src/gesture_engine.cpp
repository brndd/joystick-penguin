#include "joystick_penguin/gesture_engine.hpp"

#include <algorithm>

namespace joystick_penguin {

GestureEngine::GestureEngine(const Config& config)
    : mode_(config.initial_mode), bindings_(config.bindings) {
    if (config.modes.size() != 1 || config.modes.front() != mode_)
        throw ConfigError("gesture engine supports one persistent mode; mode transitions are not implemented yet");

    for (const auto& [name, input] : config.modifiers) {
        if (input.kind != ControlKind::Button)
            throw ConfigError("modifier '" + name + "' must use a button");
        modifiers_by_input_[{input.device, input.code}].push_back(name);
    }

    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto& binding = bindings_[index];
        if (binding.input.kind != ControlKind::Button || binding.actions.empty())
            throw ConfigError("gesture engine requires button inputs and at least one action");
        for (const auto& modifier : binding.modifiers) {
            const auto source = modifiers_by_input_.find({binding.input.device, binding.input.code});
            if (source != modifiers_by_input_.end() &&
                std::find(source->second.begin(), source->second.end(), modifier) != source->second.end())
                throw ConfigError("a button cannot require its own modifier '" + modifier + "'");
        }
        if (std::find(binding.modes.begin(), binding.modes.end(), mode_) != binding.modes.end())
            candidates_[{binding.input.device, binding.input.code}].push_back(index);
    }

    for (const auto& [source, indices] : candidates_)
        for (std::size_t i = 0; i < indices.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (bindings_[indices[i]].modifiers.size() == bindings_[indices[j]].modifiers.size())
                    throw ConfigError("equally specific bindings for input '" + source.first + ":" +
                                      std::to_string(source.second) + "'");
}

std::optional<std::size_t> GestureEngine::select(const Key& source) const {
    const auto found = candidates_.find(source);
    if (found == candidates_.end()) return std::nullopt;
    std::optional<std::size_t> selected;
    for (const auto index : found->second) {
        const auto& required = bindings_[index].modifiers;
        if (!std::all_of(required.begin(), required.end(), [this](const std::string& name) {
                return held_modifiers_.contains(name);
            })) continue;
        if (!selected || required.size() > bindings_[*selected].modifiers.size())
            selected = index;
    }
    return selected;
}

void GestureEngine::release(const Key& source, std::vector<OutputEvent>& events) {
    const auto gesture = down_.find(source);
    if (gesture == down_.end()) return;
    if (const auto modifiers = modifiers_by_input_.find(source); modifiers != modifiers_by_input_.end())
        for (const auto& name : modifiers->second) held_modifiers_.erase(name);
    for (const auto& target : gesture->second.outputs) {
        const auto owner = owners_.find(target);
        if (--owner->second == 0) {
            owners_.erase(owner);
            events.push_back({target.first, OutputEventKind::Button, target.second, 0});
        }
    }
    down_.erase(gesture);
}

void GestureEngine::release_device(const std::string& device, std::vector<OutputEvent>& events) {
    for (auto it = down_.begin(); it != down_.end();) {
        if (it->first.first == device) {
            const Key source = it->first;
            ++it;
            release(source, events);
        } else {
            ++it;
        }
    }
}

std::vector<OutputEvent> GestureEngine::process(const InputEvent& event) {
    std::vector<OutputEvent> output;
    if (event.kind == InputEventKind::SyncLost || event.kind == InputEventKind::Disconnected) {
        release_device(event.device, output);
    } else if (event.kind == InputEventKind::Button) {
        const Key source{event.device, event.code};
        if (event.value == 0) {
            release(source, output);
        } else if (event.value == 1) {
            const auto [press, fresh] = down_.try_emplace(source);
            if (!fresh) return output;
            // Select against the pre-press modifier snapshot. If this source is
            // itself a modifier, it affects only gestures pressed after this one.
            press->second.binding = select(source);
            if (press->second.binding) {
                std::set<Key> asserted;
                for (const auto& action : bindings_[*press->second.binding].actions) {
                    const Key target{action.device, action.code};
                    if (!asserted.insert(target).second) continue;
                    press->second.outputs.push_back(target);
                    if (++owners_[target] == 1)
                        output.push_back({target.first, OutputEventKind::Button, target.second, 1});
                }
            }
            if (const auto modifiers = modifiers_by_input_.find(source);
                modifiers != modifiers_by_input_.end())
                for (const auto& name : modifiers->second) held_modifiers_.insert(name);
        }
    }
    return output;
}

std::vector<OutputEvent> GestureEngine::release_all() {
    std::vector<OutputEvent> output;
    while (!down_.empty()) release(down_.begin()->first, output);
    return output;
}

} // namespace joystick_penguin
