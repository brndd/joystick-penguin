#pragma once

#include "joystick_penguin/config.hpp"
#include "joystick_penguin/io.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace joystick_penguin {

// All methods run in the serialized engine loop. Every physical down is
// recorded, even if it has no eligible binding, so it cannot be remapped
// retroactively when a modifier changes.
class GestureEngine {
public:
    explicit GestureEngine(const Config& config);
    std::vector<OutputEvent> process(const InputEvent& event);
    std::vector<OutputEvent> release_all();

private:
    using Key = std::pair<std::string, int>;
    struct Gesture {
        std::optional<std::size_t> binding;
        std::vector<Key> outputs;
    };

    std::string mode_;
    std::vector<Binding> bindings_;
    std::map<Key, std::vector<std::size_t>> candidates_;
    std::map<Key, std::vector<std::string>> modifiers_by_input_;
    std::set<std::string> held_modifiers_;
    std::map<Key, Gesture> down_;
    std::map<Key, std::size_t> owners_;

    std::optional<std::size_t> select(const Key& source) const;
    void release(const Key& source, std::vector<OutputEvent>& events);
    void release_device(const std::string& device, std::vector<OutputEvent>& events);
};

} // namespace joystick_penguin
