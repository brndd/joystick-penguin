#include "joystick_penguin/config.hpp"
#include "joystick_penguin/gesture_engine.hpp"
#include "joystick_penguin/output_frames.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace joystick_penguin;

namespace {

struct ManualClock final : Clock {
    TimePoint time{};
    TimePoint now() const override { return time; }
    void advance(int ms) { time += std::chrono::milliseconds(ms); }
};

using Expected = std::tuple<std::string, OutputEventKind, int, int>;
void expect(const std::vector<OutputEvent>& actual, const std::vector<Expected>& wanted,
            const std::string& reason) {
    if (actual.size() != wanted.size()) throw std::runtime_error(reason + ": output count");
    for (std::size_t i = 0; i < wanted.size(); ++i)
        if (std::tie(actual[i].device, actual[i].kind, actual[i].code, actual[i].value) != wanted[i])
            throw std::runtime_error(reason + ": wrong output");
}

constexpr auto key = OutputEventKind::Button;
constexpr auto axis = OutputEventKind::AbsoluteAxis;
InputEvent down(const char* device, int code) { return {device, InputEventKind::Button, code, 1}; }
InputEvent up(const char* device, int code) { return {device, InputEventKind::Button, code, 0}; }

const char* profile = R"yaml(
version: 1
devices:
  a: {kind: evdev, path: /dev/input/by-id/a}
  b: {kind: evdev, path: /dev/input/by-id/b}
  v:
    kind: uinput
    preset: joystick
    axes:
      0: {min: -100, max: 100, neutral: 0}
      1: {min: -100, max: 100, neutral: 0}
modes: {initial: default, names: [default, alternate]}
modifiers:
  shift: {input: {device: b, button: 307}}
bindings:
  - input: {device: a, button: 304}
    modes: [default]
    action: {type: button, device: v, button: 304}
  - input: {device: a, button: 304}
    modes: [alternate]
    action: {type: button, device: v, button: 305}
  - input: {device: a, axis: 0}
    modes: [default]
    action: {type: axis, device: v, axis: 0}
  - input: {device: a, axis: 0}
    modes: [alternate]
    action: {type: axis, device: v, axis: 1}
  - input: {device: b, button: 308}
    modes: [default, alternate]
    threshold_ms: 200
    tap: {action: {type: button, device: v, button: 310}}
    hold:
      actions:
        - {type: button, device: v, button: 311}
        - {type: mode, mode: alternate}
  - input: {device: b, button: 309}
    modes: [default, alternate]
    action: {type: mode, mode: default}
  - input: {device: b, button: 314}
    modes: [default, alternate]
    action: {type: mode, mode: alternate}
  - input: {device: b, button: 307}
    modes: [default, alternate]
    threshold_ms: 100
    hold: {action: {type: button, device: v, button: 312}}
  - input: {device: b, button: 313}
    modes: [default, alternate]
    threshold_ms: 100
    tap: {action: {type: mode, mode: default}}
  - input: {device: a, hat: {axis: 16, direction: -1}}
    modes: [default]
    action: {type: hat, device: v, axis: 16, direction: -1}
)yaml";

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string replace(std::string text, const std::string& before, const std::string& after) {
    const auto at = text.find(before);
    check(at != std::string::npos, "missing test pattern: " + before);
    text.replace(at, before.size(), after);
    return text;
}

void rejected(const std::string& text, const std::string& reason) {
    try {
        (void)load_config(text);
    } catch (const ConfigError& error) {
        check(std::string(error.what()).find(reason) != std::string::npos,
              "unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("accepted invalid config: " + reason);
}

void validation() {
    rejected(replace(profile, "threshold_ms: 200", "threshold_ms: 0"), "out of permitted range");
    rejected(replace(profile, "threshold_ms: 200", "threshold_ms: -1"), "out of permitted range");
    rejected(replace(profile, "threshold_ms: 200", "threshold_ms: 1.5"), "integer");
    rejected(replace(profile, "threshold_ms: 200", "threshold_ms: 200\n    action: {type: mode, mode: default}"),
             "cannot mix");
    rejected(replace(profile, "    tap: {action: {type: button, device: v, button: 310}}\n"
                      "    hold:\n      actions:\n        - {type: button, device: v, button: 311}\n"
                      "        - {type: mode, mode: alternate}", ""), "requires tap or hold");
    rejected(replace(profile, "type: mode, mode: alternate", "type: mode, mode: missing"),
             "unknown mode");
    rejected(replace(profile, "tap: {action: {type: button, device: v, button: 310}}",
                     "tap: {actions: []}"), "actions cannot be empty");
    rejected(replace(profile, "tap: {action: {type: button, device: v, button: 310}}",
                     "tap: {action: {type: button, device: v, button: 310}, extra: 1}"),
             "unknown key");
    rejected(replace(profile, "tap: {action: {type: button, device: v, button: 310}}",
                     "tap: {action: {type: button, device: missing, button: 310}}"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "tap: {action: {type: button, device: v, button: 310}}",
                     "tap: {actions: [{type: button, device: v, button: 310},"
                     " {type: button, device: v, button: 310}]}"), "repeats output control");
    rejected(replace(profile, "  - input: {device: a, axis: 0}\n    modes: [default]",
                     "  - input: {device: a, axis: 0}\n    threshold_ms: 20\n    modes: [default]"),
             "tap/hold requires a button");
}

struct Sink final : OutputSink {
    std::vector<std::vector<OutputEvent>> frames;
    std::expected<void, std::string> write_frame(const std::vector<OutputEvent>& events) override {
        frames.push_back(events);
        return {};
    }
};

void run() {
    validation();
    const auto config = load_config(profile);
    ManualClock clock;
    GestureEngine engine(config, clock);
    Sink sink;
    OutputFrames frames(sink, config);

    expect(engine.process(down("a", 304)), {{"v", key, 304, 1}}, "initial captured press");
    expect(engine.process({"a", InputEventKind::AbsoluteAxis, 0, 75, 0, 100}),
           {{"v", axis, 0, 50}}, "initial axis");
    expect(engine.process({"a", InputEventKind::AbsoluteAxis, 16, -1, -1, 1}),
           {{"v", axis, 16, -1}}, "initial hat");
    expect(engine.process(down("b", 308)), {}, "tap/hold pending");
    check(engine.next_deadline() == clock.now() + std::chrono::milliseconds(200), "deadline");
    clock.advance(199);
    expect(engine.process_timers(), {}, "before threshold");
    expect(engine.process(down("b", 307)), {}, "other key and modifier do not commit hold");
    expect(engine.process(up("b", 307)), {}, "short hold-only press is inert");
    clock.advance(1);
    expect(engine.process_timers(), {{"v", key, 311, 1}, {"v", axis, 0, 0},
                                     {"v", axis, 1, 50}}, "threshold asserts hold and reroutes axis");
    check(engine.mode() == "alternate" && !engine.next_deadline(), "persistent mode and timer consumed");
    expect(engine.process(up("b", 308)), {{"v", key, 311, 0}}, "hold output releases");
    check(engine.mode() == "alternate", "release does not revert persistent mode");
    expect(engine.process(down("a", 304)), {}, "duplicate held press does not remap");
    expect(engine.process(up("a", 304)), {{"v", key, 304, 0}}, "old mode binding survives");
    expect(engine.process({"a", InputEventKind::AbsoluteAxis, 16, 0, -1, 1}),
           {{"v", axis, 16, 0}}, "old mode hat survives until release");
    expect(engine.process(down("a", 304)), {{"v", key, 305, 1}}, "new press uses new mode");

    expect(engine.process(down("b", 313)), {}, "tap-only pending");
    clock.advance(99);
    expect(engine.process(up("b", 313)), {{"v", axis, 1, 0}, {"v", axis, 0, 50}},
           "tap mode switch reroutes axes");
    check(!engine.has_tap_release() && engine.mode() == "default", "mode-only tap has no output pulse");
    expect(engine.process(up("a", 304)), {{"v", key, 305, 0}}, "alternate capture survives return");

    expect(engine.process(down("b", 308)), {}, "pending gesture survives mode change");
    expect(engine.process(down("b", 314)), {{"v", axis, 0, 0}, {"v", axis, 1, 50}},
           "immediate action changes mode while gesture pending");
    clock.advance(200);
    expect(engine.process(up("b", 308)), {},
           "release at exact deadline activates hold, not tap");
    check(engine.mode() == "alternate", "threshold hold action persists after release");
    expect(engine.process(up("b", 314)), {}, "mode button release");
    expect(engine.process(down("b", 313)), {}, "reset mode via tap");
    clock.advance(50);
    expect(engine.process(up("b", 313)), {{"v", axis, 1, 0}, {"v", axis, 0, 50}},
           "reset mode for tap pulse");

    expect(engine.process(down("b", 308)), {}, "second pending press");
    clock.advance(50);
    frames.add(engine.process(up("b", 308)));
    check(engine.has_tap_release(), "tap output awaits next frame");
    check(bool(frames.flush()), "flush tap assertion");
    frames.add(engine.finish_tap());
    check(bool(frames.flush()), "flush tap release");
    check(sink.frames.size() == 2 && sink.frames[0].size() == 1 &&
          sink.frames[0][0].code == 310 && sink.frames[0][0].value == 1 &&
          sink.frames[1].size() == 1 && sink.frames[1][0].value == 0,
          "tap pulses in separate frames");

    expect(engine.process(down("b", 307)), {}, "modifier tap/hold press");
    expect(engine.process(down("b", 308)), {}, "timed gesture captures before modifier change");
    expect(engine.process({"b", InputEventKind::Button, 308, 2}), {}, "repeat ignored");
    clock.advance(100);
    expect(engine.process_timers(), {{"v", key, 312, 1}}, "modifier hold threshold");
    expect(engine.process({"b", InputEventKind::Disconnected}), {{"v", key, 312, 0}},
           "device loss cancels pending and asserted holds");
    check(!engine.next_deadline(), "device loss clears timers");
    clock.advance(1000);
    expect(engine.process_timers(), {}, "lost device cannot fire timer");
    expect(engine.release_all(), {{"v", axis, 0, 0}}, "shutdown neutralizes axis");
}

void cancellation_and_mode_routing() {
    auto config = load_config(profile);
    ManualClock clock;
    GestureEngine engine(config, clock);
    expect(engine.process({"a", InputEventKind::AxisBaseline, 0, 75, 0, 100}), {},
           "connection baseline remains inert");
    expect(engine.process(down("b", 314)), {{"v", axis, 1, 50}},
           "mode switch activates previously cached position");
    expect(engine.process(down("b", 307)), {}, "modifier takes effect immediately");
    clock.advance(100);
    expect(engine.process_timers(), {{"v", key, 312, 1}}, "hold-only branch fires at threshold");
    expect(engine.process(up("b", 307)), {{"v", key, 312, 0}}, "hold-only branch cleans up");

    expect(engine.process(down("b", 308)), {}, "pending tap/hold");
    expect(engine.process({"b", InputEventKind::SyncLost}), {}, "sync loss cancels pending tap");
    check(!engine.next_deadline(), "sync loss removes deadline");
    clock.advance(500);
    expect(engine.process_timers(), {}, "cancelled hold does not fire");
    expect(engine.process(up("b", 308)), {}, "cancelled gesture cannot tap");
    expect(engine.release_all(), {{"v", axis, 1, 0}}, "unaffected physical axis clears on shutdown");

    GestureEngine tap_only(config, clock);
    expect(tap_only.process(down("b", 313)), {}, "tap-only press pending");
    clock.advance(100);
    expect(tap_only.process_timers(), {}, "tap-only threshold has no hold action");
    check(!tap_only.next_deadline(), "tap-only timer is consumed");
    expect(tap_only.process(up("b", 313)), {}, "tap-only release after threshold does not tap");
    check(!tap_only.has_tap_release(), "no late tap pulse");
}

} // namespace

int main() {
    try {
        run();
        cancellation_and_mode_routing();
        std::cout << "modes and tap/hold tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
