#include "joystick_penguin/config.hpp"
#include "joystick_penguin/gesture_engine.hpp"
#include "joystick_penguin/io.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace joystick_penguin;

namespace {

const std::string profile = R"(
version: 1
devices:
  physical: {kind: evdev, path: /dev/input/by-id/test-event-joystick}
  shared: {kind: evdev, path: /dev/input/by-path/test-event-joystick, grab: false}
  virtual: {kind: uinput, preset: joystick}
modes: {initial: default, names: [default, alternate]}
modifiers:
  shift: {input: {device: physical, button: 307}}
bindings:
  - input: {device: physical, button: 304}
    modes: [default, alternate]
    action: {type: button, device: virtual, button: 305}
  - input: {device: physical, button: 304}
    modes: [default]
    modifiers: [shift]
    action: {type: button, device: virtual, button: 306}
)";

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string replace(std::string source, const std::string& before, const std::string& after) {
    const auto at = source.find(before);
    check(at != std::string::npos, "test fixture is missing '" + before + "'");
    source.replace(at, before.size(), after);
    return source;
}

void rejected(const std::string& source, const std::string& reason) {
    try {
        (void)load_config(source);
    } catch (const ConfigError& error) {
        check(std::string(error.what()).find(reason) != std::string::npos,
              "wrong error for '" + reason + "': " + error.what());
        return;
    }
    throw std::runtime_error("accepted invalid profile: " + reason);
}

class ManualClock final : public Clock {
public:
    TimePoint now() const override { return time; }
    void advance(std::chrono::milliseconds duration) { time += duration; }
private:
    TimePoint time{};
};

void run() {
    const auto config = load_config(profile);
    const auto example = load_config_file(EXAMPLE_PROFILE);
    const auto gesture_example = load_config_file(GESTURES_PROFILE);
    check(example.devices.size() == 2 && example.bindings.size() == 2,
          "example profile should load from disk");
    check(gesture_example.modifiers.at("shift").code == 292 &&
          gesture_example.bindings.size() == 5 && gesture_example.bindings[4].actions.size() == 2,
          "manual gesture profile should load from disk");
    GestureEngine demo(gesture_example);
    check(demo.process({"physical", InputEventKind::Button, 288, 1}).size() == 1,
          "manual gesture profile is runnable");
    const auto modifier_output = demo.process({"physical", InputEventKind::Button, 292, 1});
    check(modifier_output.size() == 1 && modifier_output[0].code == 293 &&
          modifier_output[0].value == 1, "manual modifier has its own output");
    const auto modified_b = demo.process({"physical", InputEventKind::Button, 290, 1});
    check(modified_b.size() == 2 && modified_b[0].code == 291 &&
          modified_b[1].code == 294, "manual B uses two captured actions");
    check(config.devices.size() == 3 && config.bindings.size() == 2, "profile size");
    check(config.devices.at("physical").grab, "default grab");
    check(!config.devices.at("shared").grab, "explicit shared device");
    check(config.initial_mode == "default", "initial mode");
    check(config.modifiers.at("shift").code == 307, "modifier code");
    check(config.bindings[0].actions.size() == 1 && config.bindings[0].actions[0].code == 305,
          "output code");
    check(config.bindings[1].modifiers == std::vector<std::string>{"shift"},
          "modified binding");

    const auto multiple_actions = replace(profile,
        "action: {type: button, device: virtual, button: 305}",
        "actions:\n      - {type: button, device: virtual, button: 305}\n"
        "      - {type: button, device: virtual, button: 307}");
    const auto multi = load_config(multiple_actions);
    check(multi.bindings[0].actions.size() == 2 && multi.bindings[0].actions[1].code == 307,
          "action list and original shorthand decode to the same typed representation");

    rejected(replace(profile, "version: 1", "version: 2"), "unsupported profile version");
    rejected(replace(profile, "device: virtual, button: 305", "device: missing, button: 305"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "device: virtual, button: 305", "device: physical, button: 305"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "modifiers: [shift]", "modifiers: [unknown]"),
             "unknown modifier");
    rejected(replace(profile, "modes: [default, alternate]", "modes: [unknown]"),
             "unknown mode");
    rejected(replace(profile, "modifiers: [shift]\n    action:", "action:"),
             "conflicts with bindings[0]");
    rejected(replace(profile, "button: 304", "button: 4"), "event-code range");
    rejected(replace(profile, "type: button, device: virtual", "type: macro, device: virtual"),
             "unsupported");
    rejected(replace(profile, "grab: false", "grab: maybe"), "must be a boolean");
    rejected(replace(profile, "version: 1", "version: 1\nversion: 1"), "duplicate key");
    rejected(replace(profile, "action: {type: button, device: virtual, button: 305}",
             "actions: []"), "actions cannot be empty");
    rejected(replace(profile, "action: {type: button, device: virtual, button: 305}",
             "action: {type: button, device: virtual, button: 305}\n"
             "    actions: [{type: button, device: virtual, button: 307}]"),
             "exactly one of 'action' or 'actions'");
    rejected(replace(multiple_actions, "- {type: button, device: virtual, button: 307}",
             "- {type: button, device: virtual, button: 305}"),
             "repeats output button");
    rejected(replace(multiple_actions, "- {type: button, device: virtual, button: 307}",
             "- {type: button, device: missing, button: 307}"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "input: {device: physical, button: 304}\n    modes: [default]\n"
             "    modifiers: [shift]", "input: {device: physical, button: 307}\n"
             "    modes: [default]\n    modifiers: [shift]"),
             "cannot require its own modifier");

    const auto disjoint = profile +
        "  - input: {device: physical, button: 304}\n"
        "    modes: [alternate]\n    modifiers: [shift]\n"
        "    action: {type: button, device: virtual, button: 307}\n";
    check(load_config(disjoint).bindings.size() == 3,
          "equally specific bindings in different modes do not conflict");
    rejected(replace(disjoint, "modes: [alternate]\n    modifiers: [shift]",
                     "modes: [default]\n    modifiers: [shift]"),
             "conflicts with bindings[1]");
    const auto two_modifiers = replace(profile,
        "  shift: {input: {device: physical, button: 307}}\nbindings:",
        "  shift: {input: {device: physical, button: 307}}\n"
        "  layer: {input: {device: shared, button: 308}}\nbindings:");
    rejected(two_modifiers +
             "  - input: {device: physical, button: 304}\n"
             "    modes: [default]\n    modifiers: [layer]\n"
             "    action: {type: button, device: virtual, button: 307}\n",
             "conflicts with bindings[1]");
    try {
        (void)load_config_file(std::string(EXAMPLE_PROFILE) + ".missing");
        throw std::runtime_error("accepted a nonexistent profile file");
    } catch (const ConfigError&) {
        // Missing files are presented to callers as configuration errors.
    }

    ManualClock clock;
    const auto start = clock.now();
    clock.advance(std::chrono::milliseconds(250));
    check(clock.now() - start == std::chrono::milliseconds(250), "controllable clock");
}

} // namespace

int main() {
    try {
        run();
        std::cout << "config and interface tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
