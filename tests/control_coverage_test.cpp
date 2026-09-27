#include "joystick_penguin/config.hpp"
#include "joystick_penguin/gesture_engine.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace joystick_penguin;

namespace {

using Expected = std::tuple<std::string, OutputEventKind, int, int>;

void expect(const std::vector<OutputEvent>& actual, std::vector<Expected> wanted,
            const std::string& reason) {
    if (actual.size() != wanted.size())
        throw std::runtime_error(reason + ": wrong output count (got " +
                                 std::to_string(actual.size()) + ", expected " +
                                 std::to_string(wanted.size()) + ")");
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (std::tie(actual[i].device, actual[i].kind, actual[i].code, actual[i].value) != wanted[i])
            throw std::runtime_error(reason + ": unexpected output at index " + std::to_string(i));
    }
}

constexpr auto abs = OutputEventKind::AbsoluteAxis;
constexpr auto key = OutputEventKind::Button;

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
      2: {min: 0, max: 255, neutral: 0}
      3: {min: 0, max: 255, neutral: 0}
  vh: {kind: uinput, preset: joystick}
modes: {initial: default, names: [default]}
modifiers:
  shift: {input: {device: b, button_code: 307}}
bindings:
  - input: {device: a, axis: 0}
    modes: [default]
    action: {type: axis, device: v, axis: 0}
  - input: {device: a, axis: 0}
    modes: [default]
    modifiers: [shift]
    action: {type: axis, device: v, axis: 1}
  - input: {device: a, axis: 2}
    modes: [default]
    action: {type: axis, device: v, axis: 2}
  - input: {device: a, axis: 2}
    modes: [default]
    modifiers: [shift]
    action: {type: axis, device: v, axis: 3}
  - input: {device: b, hat: {axis: 16, direction: -1}}
    modes: [default]
    action: {type: hat, device: vh, axis: 16, direction: -1}
  - input: {device: b, hat: {axis: 16, direction: 1}}
    modes: [default]
    action: {type: hat, device: vh, axis: 16, direction: 1}
  - input: {device: b, hat: {axis: 16, direction: -1}}
    modes: [default]
    modifiers: [shift]
    action: {type: button, device: vh, button_code: 304}
  - input: {device: b, hat: {axis: 17, direction: -1}}
    modes: [default]
    action: {type: hat, device: vh, axis: 17, direction: -1}
  - input: {device: b, hat: {axis: 17, direction: 1}}
    modes: [default]
    action: {type: hat, device: vh, axis: 17, direction: 1}
  - input: {device: b, button_code: 308}
    modes: [default]
    action: {type: hat, device: vh, axis: 16, direction: -1}
)yaml";

InputEvent movement(const std::string& device, int code, int value, int minimum, int maximum) {
    return {device, InputEventKind::AbsoluteAxis, code, value, minimum, maximum};
}

InputEvent baseline(const std::string& device, int code, int value, int minimum, int maximum) {
    return {device, InputEventKind::AxisBaseline, code, value, minimum, maximum};
}

void axes_switch_live() {
    GestureEngine engine(load_config(profile));
    expect(engine.process(baseline("a", 0, 75, 0, 100)), {}, "cached position emits nothing");
    expect(engine.process(movement("a", 0, 75, 0, 100)), {{"v", abs, 0, 50}},
           "ordinary axis scales to virtual range");
    expect(engine.process({"b", InputEventKind::Button, 307, 1}),
           {{"v", abs, 0, 0}, {"v", abs, 1, 50}},
           "modifier switches destinations without moving stick");
    expect(engine.process(movement("a", 0, 0, 0, 100)), {{"v", abs, 1, -100}},
           "new destination receives later motion");
    expect(engine.process({"b", InputEventKind::Button, 307, 0}),
           {{"v", abs, 1, 0}, {"v", abs, 0, -100}},
           "releasing modifier immediately restores ordinary destination");
    expect(engine.process(movement("a", 0, 100, 0, 100)), {{"v", abs, 0, 100}},
           "endpoint scales exactly");
    expect(engine.process({"a", InputEventKind::Disconnected}), {{"v", abs, 0, 0}},
           "axis loss returns to configured neutral");

    expect(engine.process(baseline("a", 2, 120, 0, 2047)), {},
           "throttle baseline does not assert output on reconnect");
    expect(engine.process(movement("a", 2, 2047, 0, 2047)), {{"v", abs, 2, 255}},
           "non-centering throttle scales to unsigned range");
    expect(engine.process({"b", InputEventKind::Button, 307, 1}),
           {{"v", abs, 2, 0}, {"v", abs, 3, 255}}, "throttle switches live");
    expect(engine.process({"b", InputEventKind::Button, 307, 0}),
           {{"v", abs, 3, 0}, {"v", abs, 2, 255}}, "throttle switches back without centering");
    expect(engine.release_all(), {{"v", abs, 2, 0}}, "shutdown neutralizes throttle");
}

void cached_position_and_modifier_loss() {
    GestureEngine engine(load_config(profile));
    expect(engine.process(baseline("a", 0, 75, 0, 100)), {}, "initial axis baseline");
    expect(engine.process({"b", InputEventKind::Button, 307, 1}), {{"v", abs, 1, 50}},
           "modifier may route cached axis before its first motion");
    expect(engine.process({"b", InputEventKind::Disconnected}),
           {{"v", abs, 1, 0}, {"v", abs, 0, 50}},
           "loss of modifier device reroutes surviving axis");
    expect(engine.process({"a", InputEventKind::SyncLost}), {{"v", abs, 0, 0}},
           "sync loss neutralizes only the affected source");
    expect(engine.process(baseline("a", 0, 25, 0, 100)), {}, "sync baseline is inert");
    expect(engine.process(movement("a", 0, 50, 0, 100)), {},
           "physical center equals neutral even after sync");
}

void hats_and_diagonals() {
    GestureEngine engine(load_config(profile));
    expect(engine.process(baseline("b", 16, -1, -1, 1)), {},
           "held hat direction on reconnect does not activate");
    expect(engine.process(movement("b", 16, 0, -1, 1)), {}, "release stale direction");
    expect(engine.process(movement("b", 16, -1, -1, 1)), {{"vh", abs, 16, -1}},
           "left direction begins gesture");
    expect(engine.process(movement("b", 17, 1, -1, 1)), {{"vh", abs, 17, 1}},
           "second hat axis forms a diagonal");
    expect(engine.process(movement("b", 16, 1, -1, 1)), {{"vh", abs, 16, 1}},
           "direct direction change emits final hat value, not intermediate neutral");
    expect(engine.process({"b", InputEventKind::Button, 307, 1}), {},
           "modifier does not alter captured hat gestures");
    expect(engine.process(movement("b", 16, -1, -1, 1)),
           {{"vh", abs, 16, 0}, {"vh", key, 304, 1}},
           "new hat direction selects modified binding");
    expect(engine.process({"b", InputEventKind::Button, 307, 0}), {},
           "modifier release cannot change held hat binding");
    expect(engine.process(movement("b", 16, 0, -1, 1)), {{"vh", key, 304, 0}},
           "hat neutral releases its button");
    expect(engine.process(movement("b", 17, 0, -1, 1)), {{"vh", abs, 17, 0}},
           "other component remains independent");

    expect(engine.process({"b", InputEventKind::Button, 308, 1}), {{"vh", abs, 16, -1}},
           "button can hold a hat direction");
    expect(engine.process(movement("b", 16, 1, -1, 1)), {{"vh", abs, 16, 1}},
           "later hat update wins against button");
    expect(engine.process(movement("b", 16, 0, -1, 1)), {{"vh", abs, 16, -1}},
           "ending hat restores button's previous direction");
    expect(engine.process({"b", InputEventKind::Button, 308, 0}), {{"vh", abs, 16, 0}},
           "last owner restores hat neutral");
}

void axis_arbitration_and_neutral() {
    auto config = load_config(profile);
    config.bindings.push_back({{"b", ControlKind::AbsoluteAxis, 0}, {"default"}, {},
                               {AxisAction{"v", 0}}});
    GestureEngine engine(config);
    expect(engine.process(movement("a", 0, 25, 0, 100)), {{"v", abs, 0, -50}},
           "first axis owns target");
    expect(engine.process(movement("b", 0, 75, 0, 100)), {{"v", abs, 0, 50}},
           "last updated axis wins");
    expect(engine.process({"b", InputEventKind::Disconnected}), {{"v", abs, 0, -50}},
           "loss of winner restores previous owner");
    expect(engine.process({"a", InputEventKind::Disconnected}), {{"v", abs, 0, 0}},
           "no owner restores configured neutral");

    auto inverted = load_config(profile);
    inverted.bindings[1].actions = {AxisAction{"v", 1, true}};
    GestureEngine inversion(inverted);
    expect(inversion.process(baseline("a", 0, 75, 0, 100)), {}, "inversion baseline");
    expect(inversion.process({"b", InputEventKind::Button, 307, 1}), {{"v", abs, 1, -50}},
           "axis inversion flips scaled polarity");

    auto nonzero = load_config(profile);
    nonzero.devices.at("v").axes.at(2) = {100, 200, 100};
    GestureEngine calibrated(nonzero);
    expect(calibrated.process(movement("a", 2, 100, 0, 200)), {{"v", abs, 2, 150}},
           "configured output range defines scaling");
    expect(calibrated.process({"a", InputEventKind::Disconnected}), {{"v", abs, 2, 100}},
           "nonzero neutral is respected");
}

} // namespace

int main() {
    try {
        axes_switch_live();
        cached_position_and_modifier_loss();
        hats_and_diagonals();
        axis_arbitration_and_neutral();
        std::cout << "control coverage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
