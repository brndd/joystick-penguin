#pragma once

#include "joystick_penguin/config.hpp"
#include "joystick_penguin/io.hpp"

#include <expected>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace joystick_penguin {

// Coalesces the final value for each virtual control within a physical frame.
class OutputFrames {
public:
    OutputFrames(OutputSink& sink, const Config& config);
    void add(const std::vector<OutputEvent>& events);
    std::expected<void, std::string> flush();

private:
    using Key = std::tuple<std::string, OutputEventKind, int>;
    OutputSink& sink_;
    std::map<Key, int> neutral_;
    std::map<Key, int> sent_;
    std::map<Key, int> pending_;
    std::vector<Key> order_;
};

} // namespace joystick_penguin
