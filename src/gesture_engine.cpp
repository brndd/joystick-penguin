#include "joystick_penguin/gesture_engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace joystick_penguin {
namespace {

const Clock& steady_clock() {
    static const SteadyClock clock;
    return clock;
}

} // namespace

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

GestureEngine::GestureEngine(const Config& config) : GestureEngine(config, steady_clock()) {}

GestureEngine::GestureEngine(const Config& config, const Clock& clock)
    : mode_(config.initial_mode), clock_(clock), bindings_(config.bindings) {
    for (const auto& [name, device] : config.devices)
        if (device.kind == DeviceKind::Uinput)
            for (const auto& [code, range] : device.axes) {
                const OutputKey target{name, OutputEventKind::AbsoluteAxis, code};
                output_ranges_.emplace(target, range);
                neutrals_.emplace(target, range.neutral);
            }

    for (const auto& [name, inputs] : config.modifiers)
        for (const auto& input : inputs) {
            if (input.kind != ControlKind::Button)
                throw ConfigError("modifier '" + name + "' must use a button");
            modifiers_by_input_[{input.device, input.code}].push_back(name);
        }

    // Index bindings by physical control while retaining the selected binding's
    // index in each gesture for later cleanup and eventual mode transitions.
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto& binding = bindings_[index];
        if (binding.actions.empty() && !binding.tap_hold)
            throw ConfigError("bindings require at least one action");
        for (const auto& modifier : binding.modifiers) {
            const auto source = modifiers_by_input_.find({binding.input.device, binding.input.code});
            if (source != modifiers_by_input_.end() &&
                std::find(source->second.begin(), source->second.end(), modifier) != source->second.end())
                throw ConfigError("a button cannot require its own modifier '" + modifier + "'");
        }
        auto index_actions = [&](const std::vector<Action>& actions) {
            for (const auto& action : actions) {
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
        };
        index_actions(binding.actions);
        if (binding.tap_hold) {
            index_actions(binding.tap_hold->tap);
            index_actions(binding.tap_hold->hold);
        }
        candidates_[{binding.input.device, binding.input.kind,
                     binding.input.code, binding.input.direction}].push_back(index);
        const Key source{binding.input.device, binding.input.code};
        if (binding.input.kind == ControlKind::HatDirection) hat_inputs_.insert(source);
        if (binding.input.kind == ControlKind::AbsoluteAxis) axis_inputs_.insert(source);
    }
    for (const auto& [source, indices] : candidates_)
        for (std::size_t i = 0; i < indices.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (bindings_[indices[i]].modifiers.size() == bindings_[indices[j]].modifiers.size() &&
                    std::any_of(bindings_[indices[i]].modes.begin(), bindings_[indices[i]].modes.end(),
                        [&](const auto& mode) {
                            const auto& other = bindings_[indices[j]].modes;
                            return std::find(other.begin(), other.end(), mode) != other.end();
                        }))
                    throw ConfigError("equally specific bindings for input '" + source.device + ":" +
                                      std::to_string(source.code) + "'");
}

std::optional<std::size_t> GestureEngine::select(
    const InputKey& source, const std::set<std::string>& modifiers,
    const std::string& mode) const {
    // Only modifiers held at selection time matter; the most specific eligible
    // binding wins, with an unmodified binding acting as the fallback.
    const auto found = candidates_.find(source);
    if (found == candidates_.end()) return std::nullopt;
    std::optional<std::size_t> selected;
    for (const auto index : found->second) {
        const auto& modes = bindings_[index].modes;
        if (std::find(modes.begin(), modes.end(), mode) == modes.end()) continue;
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
    // Capture button/hat bindings once. Mode and modifier changes never reselect
    // this gesture, including while its tap/hold timer is pending.
    Gesture gesture;
    gesture.binding_index = select(source, held_modifiers_, mode_);
    if (!gesture.binding_index) return gesture;
    const auto& binding = bindings_[*gesture.binding_index];
    if (binding.tap_hold) {
        gesture.deadline = clock_.now() + std::chrono::milliseconds(binding.tap_hold->threshold_ms);
        return gesture;
    }
    activate(binding.actions, gesture.outputs, changes);
    return gesture;
}

void GestureEngine::activate(const std::vector<Action>& actions,
                             std::vector<OutputClaim>& outputs, OutputChanges& changes) {
    for (const auto& action : actions) {
        if (const auto* button = std::get_if<ButtonAction>(&action)) {
            const OutputKey target{button->device, OutputEventKind::Button, button->code};
            outputs.push_back({target, next_owner_++});
            assert_button(target, changes);
        } else if (const auto* hat = std::get_if<HatAction>(&action)) {
            const OutputKey target{hat->device, OutputEventKind::AbsoluteAxis, hat->code};
            const auto owner = next_owner_++;
            outputs.push_back({target, owner});
            update_abs(target, owner, hat->direction, changes);
        } else if (const auto* mode = std::get_if<ModeAction>(&action)) {
            change_mode(mode->mode, changes);
        }
    }
}

void GestureEngine::change_mode(const std::string& mode, OutputChanges& changes) {
    if (mode == mode_) return;
    const auto previous = mode_;
    mode_ = mode;
    reroute_axes(held_modifiers_, previous, changes);
}

void GestureEngine::end_gesture(const Gesture& gesture, OutputChanges& changes) {
    for (const auto& claim : gesture.outputs) release_claim(claim, changes);
}

void GestureEngine::release_button(const Key& source, OutputChanges& changes) {
    const auto gesture = down_.find(source);
    if (gesture == down_.end()) return;
    if (gesture->second.deadline) {
        const auto& timing = *bindings_[*gesture->second.binding_index].tap_hold;
        if (!gesture->second.hold_activated && clock_.now() >= *gesture->second.deadline) {
            gesture->second.hold_activated = true;
            activate(timing.hold, gesture->second.outputs, changes);
        }
        if (!gesture->second.hold_activated) {
            activate(timing.tap, tap_outputs_, changes);
            // Tap outputs must stay asserted long enough for consumers to see
            // them, so they release on a timer rather than in the next frame.
            if (!tap_outputs_.empty())
                tap_deadline_ = clock_.now() + std::chrono::milliseconds(timing.tap_ms);
        }
    }
    end_gesture(gesture->second, changes);
    down_.erase(gesture);
    if (const auto modifiers = modifiers_by_input_.find(source); modifiers != modifiers_by_input_.end()) {
        const auto previous = held_modifiers_;
        for (const auto& name : modifiers->second)
            if (--held_modifier_counts_.at(name) == 0) {
                held_modifier_counts_.erase(name);
                held_modifiers_.erase(name);
            }
        reroute_axes(previous, mode_, changes);
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
        const auto scale = [](long double value, long double from_min, long double from_max,
                              long double to_min, long double to_max) {
            return to_min + (value - from_min) / (from_max - from_min) * (to_max - to_min);
        };
        // Inclusive integer ranges have an upper midpoint (2048 for 0..4095).
        // A single endpoint-to-endpoint slope misses that center after inversion
        // or when the source and target have different resolutions.
        const long double center = static_cast<long long>(state.minimum) +
            (static_cast<long long>(state.maximum) - state.minimum + 1) / 2;
        long double mapped;
        if (range.neutral > range.minimum && range.neutral < range.maximum &&
            center < state.maximum) {
            mapped = physical <= center
                ? scale(physical, state.minimum, center,
                        axis.invert ? range.maximum : range.minimum, range.neutral)
                : scale(physical, center, state.maximum,
                        range.neutral, axis.invert ? range.minimum : range.maximum);
        } else {
            // Endpoint-neutral axes (e.g. throttles) have no center to preserve.
            mapped = scale(physical, state.minimum, state.maximum,
                           axis.invert ? range.maximum : range.minimum,
                           axis.invert ? range.minimum : range.maximum);
        }
        const auto scaled = static_cast<int>(std::llround(mapped));
        update_abs(target, state.outputs[i].owner, scaled, changes);
    }
}

void GestureEngine::reroute_axes(const std::set<std::string>& previous,
                                 const std::string& previous_mode, OutputChanges& changes) {
    // A cached baseline is inert on connection. A modifier change can activate
    // it, but an unrelated modifier must not create an ordinary axis output.
    for (auto& [source, state] : axes_) {
        const InputKey input{source.first, ControlKind::AbsoluteAxis, source.second};
        const auto selected = select(input, held_modifiers_, mode_);
        if (!state.active) {
            if (select(input, previous, previous_mode) == selected) continue;
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
                                               event.code}, held_modifiers_, mode_), changes);
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
            for (const auto& name : modifiers->second)
                if (--held_modifier_counts_.at(name) == 0) {
                    held_modifier_counts_.erase(name);
                    held_modifiers_.erase(name);
                }
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
    if (previous != held_modifiers_) reroute_axes(previous, mode_, changes);
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
                for (const auto& name : modifiers->second)
                    if (held_modifier_counts_[name]++ == 0) held_modifiers_.insert(name);
                reroute_axes(previous, mode_, changes);
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
    for (const auto& claim : tap_outputs_) release_claim(claim, changes);
    tap_outputs_.clear();
    tap_deadline_.reset();
    for (const auto& [source, gesture] : down_) end_gesture(gesture, changes);
    down_.clear();
    held_modifiers_.clear();
    held_modifier_counts_.clear();
    for (const auto& [source, state] : hats_)
        if (state.gesture) end_gesture(*state.gesture, changes);
    hats_.clear();
    for (const auto& [source, state] : axes_)
        for (const auto& claim : state.outputs) release_claim(claim, changes);
    axes_.clear();
    return changes.finish();
}

std::optional<Clock::TimePoint> GestureEngine::next_deadline() const {
    std::optional<Clock::TimePoint> next;
    for (const auto& [source, gesture] : down_)
        if (gesture.deadline && !gesture.hold_activated && (!next || *gesture.deadline < *next))
            next = gesture.deadline;
    if (tap_deadline_ && (!next || *tap_deadline_ < *next)) next = tap_deadline_;
    return next;
}

std::vector<OutputEvent> GestureEngine::process_timers() {
    OutputChanges changes;
    std::vector<Key> due;
    const auto now = clock_.now();
    for (const auto& [source, gesture] : down_)
        if (gesture.deadline && !gesture.hold_activated && now >= *gesture.deadline)
            due.push_back(source);
    std::stable_sort(due.begin(), due.end(), [this](const Key& a, const Key& b) {
        return *down_.at(a).deadline < *down_.at(b).deadline;
    });
    for (const auto& source : due) {
        auto& gesture = down_.at(source);
        gesture.hold_activated = true;
        activate(bindings_[*gesture.binding_index].tap_hold->hold, gesture.outputs, changes);
    }
    if (tap_deadline_ && now >= *tap_deadline_) {
        for (const auto& claim : tap_outputs_) release_claim(claim, changes);
        tap_outputs_.clear();
        tap_deadline_.reset();
    }
    return changes.finish();
}

} // namespace joystick_penguin
