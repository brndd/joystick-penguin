#include "joystick_penguin/gesture_engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace joystick_penguin {

// Keep only the final value of a control within one input event. A direct hat
// reversal, for example, should not expose an intermediate neutral output.
void GestureEngine::OutputChanges::note(const OutputKey& key, int before, int after) {
    const auto [it, fresh] = values.try_emplace(key, before, after);
    if (fresh) order.push_back(key);
    else it->second.second = after;
}

std::vector<OutputEvent> GestureEngine::OutputChanges::finish() const {
    std::vector<OutputEvent> events;
    for (const auto& key : order) {
        const auto [before, after] = values.at(key);
        if (before != after) events.push_back({key.device, key.kind, key.code, after});
    }
    return events;
}

GestureEngine::GestureEngine(const Config& config)
    : mode_(config.initial_mode), bindings_(config.bindings) {
    if (config.modes.size() != 1 || config.modes.front() != mode_)
        throw ConfigError("gesture engine supports one persistent mode; mode transitions are not implemented yet");

    for (const auto& [name, device] : config.devices)
        if (device.kind == DeviceKind::Uinput)
            for (const auto& [code, range] : device.axes) {
                const OutputKey target{name, OutputEventKind::AbsoluteAxis, code};
                output_ranges_.emplace(target, range);
                neutrals_.emplace(target, range.neutral);
            }

    for (const auto& [name, input] : config.modifiers) {
        if (input.kind != ControlKind::Button)
            throw ConfigError("modifier '" + name + "' must use a button");
        modifiers_by_input_[{input.device, input.code}].push_back(name);
    }

    // Index bindings by physical control while retaining the selected binding's
    // index in each gesture for later cleanup and eventual mode transitions.
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto& binding = bindings_[index];
        if (binding.actions.empty()) throw ConfigError("bindings require at least one action");
        for (const auto& modifier : binding.modifiers) {
            const auto source = modifiers_by_input_.find({binding.input.device, binding.input.code});
            if (source != modifiers_by_input_.end() &&
                std::find(source->second.begin(), source->second.end(), modifier) != source->second.end())
                throw ConfigError("a button cannot require its own modifier '" + modifier + "'");
        }
        for (const auto& action : binding.actions) {
            if (const auto* axis = std::get_if<AxisAction>(&action)) {
                if (binding.input.kind != ControlKind::AbsoluteAxis ||
                    !output_ranges_.contains({axis->device, OutputEventKind::AbsoluteAxis, axis->code}))
                    throw ConfigError("axis actions require an axis input and a declared output range");
            } else if (binding.input.kind == ControlKind::AbsoluteAxis) {
                throw ConfigError("axis inputs require axis actions");
            }
            if (const auto* hat = std::get_if<HatAction>(&action))
                neutrals_.emplace(OutputKey{hat->device, OutputEventKind::AbsoluteAxis, hat->code}, 0);
        }
        if (std::find(binding.modes.begin(), binding.modes.end(), mode_) != binding.modes.end()) {
            candidates_[{binding.input.device, binding.input.kind,
                         binding.input.code, binding.input.direction}].push_back(index);
            const Key source{binding.input.device, binding.input.code};
            if (binding.input.kind == ControlKind::HatDirection) hat_inputs_.insert(source);
            if (binding.input.kind == ControlKind::AbsoluteAxis) axis_inputs_.insert(source);
        }
    }
    for (const auto& [source, indices] : candidates_)
        for (std::size_t i = 0; i < indices.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (bindings_[indices[i]].modifiers.size() == bindings_[indices[j]].modifiers.size())
                    throw ConfigError("equally specific bindings for input '" + source.device + ":" +
                                      std::to_string(source.code) + "'");
}

std::optional<std::size_t> GestureEngine::select(
    const InputKey& source, const std::set<std::string>& modifiers) const {
    // Only modifiers held at selection time matter; the most specific eligible
    // binding wins, with an unmodified binding acting as the fallback.
    const auto found = candidates_.find(source);
    if (found == candidates_.end()) return std::nullopt;
    std::optional<std::size_t> selected;
    for (const auto index : found->second) {
        const auto& required = bindings_[index].modifiers;
        if (!std::all_of(required.begin(), required.end(), [&modifiers](const std::string& name) {
                return modifiers.contains(name);
            })) continue;
        if (!selected || required.size() > bindings_[*selected].modifiers.size())
            selected = index;
    }
    return selected;
}

int GestureEngine::effective_abs(const OutputKey& key) const {
    // Unlike buttons, absolute controls have values: the latest updated owner
    // wins, and removing the last owner restores the configured neutral.
    const auto active = abs_owners_.find(key);
    if (active == abs_owners_.end() || active->second.empty()) return neutrals_.at(key);
    const auto winner = std::max_element(active->second.begin(), active->second.end(),
        [](const auto& a, const auto& b) { return a.second.sequence < b.second.sequence; });
    return winner->second.value;
}

void GestureEngine::assert_button(const OutputKey& key, OutputChanges& changes) {
    if (++button_owners_[key] == 1) changes.note(key, 0, 1);
}

void GestureEngine::update_abs(const OutputKey& key, std::uint64_t owner, int value, OutputChanges& changes) {
    const int before = effective_abs(key);
    abs_owners_[key][owner] = {value, next_update_++};
    const int after = effective_abs(key);
    if (before != after) changes.note(key, before, after);
}

void GestureEngine::release_claim(const OutputClaim& claim, OutputChanges& changes) {
    // A button releases only when its last owner leaves. An axis or hat instead
    // restores the next most recently updated owner (or neutral).
    if (claim.target.kind == OutputEventKind::Button) {
        const auto it = button_owners_.find(claim.target);
        if (--it->second == 0) {
            button_owners_.erase(it);
            changes.note(claim.target, 1, 0);
        }
    } else {
        const int before = effective_abs(claim.target);
        auto it = abs_owners_.find(claim.target);
        it->second.erase(claim.owner);
        if (it->second.empty()) abs_owners_.erase(it);
        const int after = effective_abs(claim.target);
        if (before != after) changes.note(claim.target, before, after);
    }
}

GestureEngine::Gesture GestureEngine::begin_gesture(const InputKey& source, OutputChanges& changes) {
    // Capture button/hat actions once. Releasing modifiers later never reselects
    // this gesture's binding or changes the outputs it owns.
    Gesture gesture;
    gesture.binding_index = select(source, held_modifiers_);
    if (!gesture.binding_index) return gesture;
    for (const auto& action : bindings_[*gesture.binding_index].actions) {
        if (const auto* button = std::get_if<ButtonAction>(&action)) {
            const OutputKey target{button->device, OutputEventKind::Button, button->code};
            gesture.outputs.push_back({target, next_owner_++});
            assert_button(target, changes);
        } else if (const auto* hat = std::get_if<HatAction>(&action)) {
            const OutputKey target{hat->device, OutputEventKind::AbsoluteAxis, hat->code};
            const auto owner = next_owner_++;
            gesture.outputs.push_back({target, owner});
            update_abs(target, owner, hat->direction, changes);
        }
    }
    return gesture;
}

void GestureEngine::end_gesture(const Gesture& gesture, OutputChanges& changes) {
    for (const auto& claim : gesture.outputs) release_claim(claim, changes);
}

void GestureEngine::release_button(const Key& source, OutputChanges& changes) {
    const auto gesture = down_.find(source);
    if (gesture == down_.end()) return;
    end_gesture(gesture->second, changes);
    down_.erase(gesture);
    if (const auto modifiers = modifiers_by_input_.find(source); modifiers != modifiers_by_input_.end()) {
        const auto previous = held_modifiers_;
        for (const auto& name : modifiers->second) held_modifiers_.erase(name);
        reroute_axes(previous, changes);
    }
}

void GestureEngine::process_hat(const InputEvent& event, OutputChanges& changes) {
    // Each hat component has its own directional gesture. A direct reversal
    // ends the old direction and starts the new one without waiting for zero.
    const Key source{event.device, event.code};
    auto& state = hats_[source];
    if (event.value == state.direction) return;
    if (state.gesture) {
        end_gesture(*state.gesture, changes);
        state.gesture.reset();
    }
    state.direction = event.value;
    if (state.direction == -1 || state.direction == 1)
        state.gesture = begin_gesture({event.device, ControlKind::HatDirection,
                                       event.code, state.direction}, changes);
}

void GestureEngine::route_axis(const Key& source, AxisState& state,
                               std::optional<std::size_t> selected, OutputChanges& changes) {
    // Axes are live-routed: changing modifiers removes the old destination and
    // asserts the new one with the last physical position, even without motion.
    if (state.binding != selected) {
        for (const auto& claim : state.outputs) release_claim(claim, changes);
        state.outputs.clear();
        state.binding = selected;
        if (selected)
            for (const auto& action : bindings_[*selected].actions) {
                const auto& axis = std::get<AxisAction>(action);
                state.outputs.push_back({{axis.device, OutputEventKind::AbsoluteAxis, axis.code},
                                         next_owner_++});
            }
    }
    if (!selected) return;
    const auto& actions = bindings_[*selected].actions;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& axis = std::get<AxisAction>(actions[i]);
        const auto& target = state.outputs[i].target;
        const auto& range = output_ranges_.at(target);
        if (state.minimum >= state.maximum)
            throw ConfigError("invalid physical axis range for '" + source.first + "'");
        const long double physical = std::clamp(static_cast<long double>(state.value),
                                                 static_cast<long double>(state.minimum),
                                                 static_cast<long double>(state.maximum));
        long double fraction = (physical - state.minimum) /
                               (static_cast<long double>(state.maximum) - state.minimum);
        if (axis.invert) fraction = 1 - fraction;
        const auto scaled = static_cast<int>(std::llround(
            static_cast<long double>(range.minimum) +
            fraction * (static_cast<long double>(range.maximum) - range.minimum)));
        update_abs(target, state.outputs[i].owner, scaled, changes);
    }
}

void GestureEngine::reroute_axes(const std::set<std::string>& previous, OutputChanges& changes) {
    // A cached baseline is inert on connection. A modifier change can activate
    // it, but an unrelated modifier must not create an ordinary axis output.
    for (auto& [source, state] : axes_) {
        const InputKey input{source.first, ControlKind::AbsoluteAxis, source.second};
        const auto selected = select(input, held_modifiers_);
        if (!state.active) {
            if (select(input, previous) == selected) continue;
            state.active = true; // A modifier event may use a cached physical position.
        }
        if (state.binding != selected) route_axis(source, state, selected, changes);
    }
}

void GestureEngine::process_axis(const InputEvent& event, OutputChanges& changes) {
    // Baselines seed the physical state after open/resync without asserting an
    // output. Actual EV_ABS events update either a hat gesture or a live axis.
    const Key source{event.device, event.code};
    if (hat_inputs_.contains(source)) {
        if (event.kind == InputEventKind::AxisBaseline) {
            auto& state = hats_[source];
            if (state.gesture) end_gesture(*state.gesture, changes);
            state = {event.value, std::nullopt};
        } else {
            process_hat(event, changes);
        }
    } else if (axis_inputs_.contains(source)) {
        auto& state = axes_[source];
        if (event.kind == InputEventKind::AxisBaseline) {
            for (const auto& claim : state.outputs) release_claim(claim, changes);
            state = AxisState{};
            state.value = event.value;
            state.minimum = event.minimum;
            state.maximum = event.maximum;
        } else {
            state.value = event.value;
            state.minimum = event.minimum;
            state.maximum = event.maximum;
            state.active = true;
            route_axis(source, state, select({event.device, ControlKind::AbsoluteAxis,
                                               event.code}, held_modifiers_), changes);
        }
    }
}

void GestureEngine::release_device(const std::string& device, OutputChanges& changes) {
    // Cancel only this device's gestures and owners. Losing one of its held
    // modifiers can still live-reroute axes on other connected devices.
    const auto previous = held_modifiers_;
    for (auto it = down_.begin(); it != down_.end();) {
        if (it->first.first != device) { ++it; continue; }
        end_gesture(it->second, changes);
        if (const auto modifiers = modifiers_by_input_.find(it->first);
            modifiers != modifiers_by_input_.end())
            for (const auto& name : modifiers->second) held_modifiers_.erase(name);
        it = down_.erase(it);
    }
    for (auto it = hats_.begin(); it != hats_.end();) {
        if (it->first.first != device) { ++it; continue; }
        if (it->second.gesture) end_gesture(*it->second.gesture, changes);
        it = hats_.erase(it);
    }
    for (auto it = axes_.begin(); it != axes_.end();) {
        if (it->first.first != device) { ++it; continue; }
        for (const auto& claim : it->second.outputs) release_claim(claim, changes);
        it = axes_.erase(it);
    }
    if (previous != held_modifiers_) reroute_axes(previous, changes);
}

std::vector<OutputEvent> GestureEngine::process(const InputEvent& event) {
    // Serialize every transition here: button presses capture before their own
    // modifier takes effect, while subsequent axis routing sees the new set.
    OutputChanges changes;
    if (event.kind == InputEventKind::SyncLost || event.kind == InputEventKind::Disconnected) {
        release_device(event.device, changes);
    } else if (event.kind == InputEventKind::Button) {
        const Key source{event.device, event.code};
        if (event.value == 0) {
            release_button(source, changes);
        } else if (event.value == 1 && !down_.contains(source)) {
            down_.emplace(source, begin_gesture({event.device, ControlKind::Button, event.code}, changes));
            if (const auto modifiers = modifiers_by_input_.find(source);
                modifiers != modifiers_by_input_.end()) {
                const auto previous = held_modifiers_;
                for (const auto& name : modifiers->second) held_modifiers_.insert(name);
                reroute_axes(previous, changes);
            }
        }
    } else if (event.kind == InputEventKind::AbsoluteAxis ||
               event.kind == InputEventKind::AxisBaseline) {
        process_axis(event, changes);
    }
    return changes.finish();
}

std::vector<OutputEvent> GestureEngine::release_all() {
    OutputChanges changes;
    for (const auto& [source, gesture] : down_) end_gesture(gesture, changes);
    down_.clear();
    held_modifiers_.clear();
    for (const auto& [source, state] : hats_)
        if (state.gesture) end_gesture(*state.gesture, changes);
    hats_.clear();
    for (const auto& [source, state] : axes_)
        for (const auto& claim : state.outputs) release_claim(claim, changes);
    axes_.clear();
    return changes.finish();
}

} // namespace joystick_penguin
