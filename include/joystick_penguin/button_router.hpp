#pragma once

#include "joystick_penguin/config.hpp"
#include "joystick_penguin/io.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace joystick_penguin {

// Temporary button-only routing for the hardware milestone. Gestures and
// modifier selection will replace this in the next milestone.
class ButtonRouter {
public:
    explicit ButtonRouter(const Config& config);
    std::vector<OutputEvent> process(const InputEvent& event);
    std::vector<OutputEvent> release_all();

private:
    using Key = std::pair<std::string, int>;
    std::map<Key, Key> bindings_;
    std::map<Key, Key> pressed_;
    std::map<Key, std::size_t> owners_;

    void release(const Key& source, std::vector<OutputEvent>& output);
    void release_device(const std::string& device, std::vector<OutputEvent>& output);
};

} // namespace joystick_penguin
