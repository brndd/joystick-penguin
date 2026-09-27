#include "joystick_penguin/output_frames.hpp"

namespace joystick_penguin {

OutputFrames::OutputFrames(OutputSink& sink, const Config& config) : sink_(sink) {
    for (const auto& [name, device] : config.devices)
        if (device.kind == DeviceKind::Uinput)
            for (const auto& [code, range] : device.axes)
                neutral_.emplace(Key{name, OutputEventKind::AbsoluteAxis, code}, range.neutral);
    for (const auto& binding : config.bindings)
        for (const auto& action : binding.actions)
            if (const auto* hat = std::get_if<HatAction>(&action))
                neutral_.emplace(Key{hat->device, OutputEventKind::AbsoluteAxis, hat->code}, 0);
}

void OutputFrames::add(const std::vector<OutputEvent>& events) {
    for (const auto& event : events) {
        const Key key{event.device, event.kind, event.code};
        if (!pending_.contains(key)) order_.push_back(key);
        pending_[key] = event.value;
    }
}

std::expected<void, std::string> OutputFrames::flush() {
    std::vector<OutputEvent> events;
    for (const auto& key : order_) {
        const int before = sent_.contains(key) ? sent_.at(key) :
            (neutral_.contains(key) ? neutral_.at(key) : 0);
        const int after = pending_.at(key);
        if (before != after)
            events.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), after});
    }
    if (!events.empty()) {
        if (const auto result = sink_.write_frame(events); !result) return result;
    }
    for (const auto& key : order_) sent_[key] = pending_.at(key);
    pending_.clear();
    order_.clear();
    return {};
}

} // namespace joystick_penguin
