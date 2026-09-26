// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace joystick_penguin {

// A backend produces ordered events for one configured physical device. Hat axes
// are ordinary ABS_HAT* events. Sync loss and disconnection are explicit so the
// future engine can clear owned outputs instead of acting on stale state.
enum class InputEventKind { Button, AbsoluteAxis, SyncLost, Disconnected };

struct InputEvent {
    std::string device;
    InputEventKind kind;
    int code = 0;
    int value = 0;
};

class InputBackend {
public:
    virtual ~InputBackend() = default;
    // Suitable for a single engine poll loop; the backend owns its descriptor.
    virtual int descriptor() const = 0;
    // Drain pending events in arrival order, without mutating mapping state.
    virtual std::vector<InputEvent> read_events() = 0;
};

enum class OutputEventKind { Button, AbsoluteAxis };

struct OutputEvent {
    std::string device;
    OutputEventKind kind;
    int code;
    int value;
};

// One batch is one coherent frame. The output implementation owns the uinput
// devices and emits per-device SYN_REPORT after applying each batch.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void write_frame(const std::vector<OutputEvent>& events) = 0;
};

// Timer decisions use an injected monotonic clock; implementations of input
// and output interfaces never update engine state from another thread.
class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SteadyClock final : public Clock {
public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

} // namespace joystick_penguin
