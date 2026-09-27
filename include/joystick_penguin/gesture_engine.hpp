#pragma once

#include "joystick_penguin/config.hpp"
#include "joystick_penguin/io.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace joystick_penguin {

// The supplied clock must outlive the engine. All methods run in the serialized
// engine loop. Every physical down is recorded, even if it has no eligible
// binding, so it cannot be remapped
// retroactively when a modifier changes.
class GestureEngine {
public:
    explicit GestureEngine(const Config& config);
    GestureEngine(const Config& config, const Clock& clock);
    std::vector<OutputEvent> process(const InputEvent& event);
    std::optional<Clock::TimePoint> next_deadline() const;
    std::vector<OutputEvent> process_timers();
    bool has_tap_release() const { return !tap_outputs_.empty(); }
    std::vector<OutputEvent> finish_tap();
    const std::string& mode() const { return mode_; }
    std::vector<OutputEvent> release_all();

private:
    using Key = std::pair<std::string, int>;
    struct InputKey {
        std::string device;
        ControlKind kind;
        int code;
        int direction = 0;
        auto operator<=>(const InputKey&) const = default;
    };
    struct OutputKey {
        std::string device;
        OutputEventKind kind;
        int code;
        auto operator<=>(const OutputKey&) const = default;
    };
    struct OutputClaim {
        OutputKey target;
        std::uint64_t owner;
    };
    struct Gesture {
        std::optional<std::size_t> binding_index;
        std::vector<OutputClaim> outputs;
        std::optional<Clock::TimePoint> deadline;
        bool hold_activated = false;
    };
    struct HatState {
        int direction = 0;
        std::optional<Gesture> gesture;
    };
    struct AxisState {
        int value = 0;
        int minimum = 0;
        int maximum = 0;
        bool active = false;
        std::optional<std::size_t> binding;
        std::vector<OutputClaim> outputs;
    };
    struct AbsOwner {
        int value;
        std::uint64_t sequence;
    };
    struct OutputChanges {
        std::vector<OutputKey> order;
        std::map<OutputKey, std::pair<int, int>> values;
        void note(const OutputKey& key, int before, int after);
        std::vector<OutputEvent> finish() const;
    };

    std::string mode_;
    const Clock& clock_;
    std::vector<Binding> bindings_;
    std::map<InputKey, std::vector<std::size_t>> candidates_;
    std::map<Key, std::vector<std::string>> modifiers_by_input_;
    std::set<Key> hat_inputs_;
    std::set<Key> axis_inputs_;
    std::set<std::string> held_modifiers_;
    std::map<Key, Gesture> down_;
    std::map<Key, HatState> hats_;
    std::map<Key, AxisState> axes_;
    std::map<OutputKey, std::size_t> button_owners_;
    std::map<OutputKey, std::map<std::uint64_t, AbsOwner>> abs_owners_;
    std::map<OutputKey, AxisRange> output_ranges_;
    std::map<OutputKey, int> neutrals_;
    std::uint64_t next_owner_ = 1;
    std::uint64_t next_update_ = 1;
    std::vector<OutputClaim> tap_outputs_;

    std::optional<std::size_t> select(const InputKey& source,
                                      const std::set<std::string>& modifiers,
                                      const std::string& mode) const;
    int effective_abs(const OutputKey& key) const;
    void assert_button(const OutputKey& key, OutputChanges& changes);
    void update_abs(const OutputKey& key, std::uint64_t owner, int value, OutputChanges& changes);
    void release_claim(const OutputClaim& claim, OutputChanges& changes);
    Gesture begin_gesture(const InputKey& source, OutputChanges& changes);
    void activate(const std::vector<Action>& actions, std::vector<OutputClaim>& outputs,
                  OutputChanges& changes);
    void change_mode(const std::string& mode, OutputChanges& changes);
    void end_gesture(const Gesture& gesture, OutputChanges& changes);
    void release_button(const Key& source, OutputChanges& changes);
    void process_hat(const InputEvent& event, OutputChanges& changes);
    void process_axis(const InputEvent& event, OutputChanges& changes);
    void reroute_axes(const std::set<std::string>& previous, const std::string& previous_mode,
                     OutputChanges& changes);
    void route_axis(const Key& source, AxisState& state,
                    std::optional<std::size_t> selected, OutputChanges& changes);
    void release_device(const std::string& device, OutputChanges& changes);
};

} // namespace joystick_penguin
