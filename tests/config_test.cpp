#include "joystick_penguin/config.hpp"
#include "joystick_penguin/gesture_engine.hpp"
#include "joystick_penguin/io.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>

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
  shift: {inputs: [{device: physical, button: 4}]}
bindings:
  - input: {device: physical, button: 1}
    modes: [default, alternate]
    action: {type: button, device: virtual, button: 2}
  - input: {device: physical, button: 1}
    modes: [default]
    modifiers: [shift]
    action: {type: button, device: virtual, button: 3}
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
    const auto controls_example = load_config_file(CONTROLS_PROFILE);
    check(example.devices.size() == 2 && example.bindings.size() == 2,
          "example profile should load from disk");
    check(gesture_example.modifiers.at("shift").front().code == -5 &&
          gesture_example.bindings.size() == 5 && gesture_example.bindings[4].actions.size() == 2,
          "manual gesture profile should load from disk");
    check(controls_example.devices.at("virtual_stick").axes.at(2).maximum == 255 &&
          controls_example.bindings.size() == 7,
          "manual control coverage profile should load from disk");
    GestureEngine control_demo(controls_example);
    check(control_demo.process({"right", InputEventKind::AbsoluteAxis, 16, -1, -1, 1}).size() == 1,
          "manual hat mapping is runnable");
    GestureEngine demo(gesture_example);
    check(demo.process({"physical", InputEventKind::Button, -1, 1}).size() == 1,
          "manual gesture profile is runnable");
    const auto modifier_output = demo.process({"physical", InputEventKind::Button, -5, 1});
    check(modifier_output.size() == 1 && modifier_output[0].code == 293 &&
          modifier_output[0].value == 1, "manual modifier has its own output");
    const auto modified_b = demo.process({"physical", InputEventKind::Button, -3, 1});
    check(modified_b.size() == 2 && modified_b[0].code == 291 &&
          modified_b[1].code == 294, "manual B uses two captured actions");
    check(config.devices.size() == 3 && config.bindings.size() == 2, "profile size");
    check(config.devices.at("physical").grab, "default grab");
    check(config.devices.at("virtual").bus == VirtualBus::Usb, "default virtual joystick uses USB bus identity");
    check(!config.devices.at("shared").grab, "explicit shared device");
    check(config.initial_mode == "default", "initial mode");
    check(config.modifiers.at("shift").front().code == -4, "modifier index");
    const auto multipleInputs = replace(profile, "inputs: [{device: physical, button: 4}]",
        "inputs: [{device: physical, button: 4}, {device: shared, button: 5}]");
    check(load_config(multipleInputs).modifiers.at("shift").size() == 2,
          "modifier accepts buttons from multiple controllers");
    rejected(replace(profile, "inputs: [{device: physical, button: 4}]",
        "inputs: [{device: physical, button: 4}, {device: physical, button: 4}]"), "repeats a button");
    rejected(replace(profile, "inputs: [{device: physical, button: 4}]",
        "inputs: [{device: physical, axis: 0}]"), "must use buttons");
    const auto orphan = load_config(replace(profile, "inputs: [{device: physical, button: 4}]", "inputs: []"));
    check(orphan.modifiers.at("shift").empty() && load_config(serialize_config(orphan)) == orphan,
          "unassigned modifier remains valid and round trips with dependent mappings");
    check(config.bindings[0].actions.size() == 1 &&
          std::get<ButtonAction>(config.bindings[0].actions[0]).code == 289,
          "output code");
    check(config.bindings[1].modifiers == std::vector<std::string>{"shift"},
          "modified binding");
    const auto high_button = replace(profile, "device: virtual, button: 2",
                                     "device: virtual, button: 79");
    check(std::get<ButtonAction>(load_config(high_button).bindings[0].actions[0]).code == 766,
          "highest preset button maps below KEY_MAX");
    check(std::get<ButtonAction>(load_config(replace(profile, "device: virtual, button: 2",
          "device: virtual, button: 17")).bindings[0].actions[0]).code == 704,
          "button 17 crosses the evdev code gap");

    const auto full_virtual = replace(profile, "virtual: {kind: uinput, preset: joystick}",
        "virtual: {kind: uinput, preset: joystick, name: joyful-virtual, bus: usb, "
        "vendor_id: 0x4711, product_id: 0x0817}");
    const auto explicit_device = load_config(full_virtual).devices.at("virtual");
    check(explicit_device.virtual_name == "joyful-virtual" && explicit_device.bus == VirtualBus::Usb &&
          explicit_device.vendor_id == 0x4711 && explicit_device.product_id == 0x0817 &&
          explicit_device.axes.contains(7), "explicit virtual identity and preset axes");
    rejected(replace(full_virtual, "bus: usb", "bus: invalid"), "bus must be");
    check(load_config(replace(full_virtual, "bus: usb", "bus: virtual")).devices.at("virtual").bus == VirtualBus::Virtual,
          "explicit virtual bus identity remains available");
    rejected(replace(full_virtual, "product_id: 0x0817", "product_id: 0x10000"),
             "out of permitted range");

    const auto multiple_actions = replace(profile,
        "action: {type: button, device: virtual, button: 2}",
        "actions:\n      - {type: button, device: virtual, button: 2}\n"
        "      - {type: button, device: virtual, button: 4}");
    const auto multi = load_config(multiple_actions);
    check(multi.bindings[0].actions.size() == 2 &&
          std::get<ButtonAction>(multi.bindings[0].actions[1]).code == 291,
          "action list and original shorthand decode to the same typed representation");

    rejected(replace(profile, "version: 1", "version: 2"), "unsupported profile version");
    rejected(replace(profile, "device: virtual, button: 2", "device: missing, button: 2"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "device: virtual, button: 2", "device: physical, button: 2"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "modifiers: [shift]", "modifiers: [unknown]"),
             "unknown modifier");
    rejected(replace(profile, "modes: [default, alternate]", "modes: [unknown]"),
             "unknown mode");
    rejected(replace(profile, "modifiers: [shift]\n    action:", "action:"),
             "conflicts with bindings[0]");
    rejected(replace(profile, "button: 1", "button: 0"), "out of permitted range");
    rejected(replace(profile, "button: 1", "button: 256"), "out of permitted range");
    rejected(replace(profile, "device: virtual, button: 2", "device: virtual, button: 80"),
             "out of permitted range");
    check(load_config(replace(profile, "button: 1", "button_code: 704")).bindings[0].input.code == 704,
          "literal physical button code escape hatch");
    rejected(replace(profile, "type: button, device: virtual", "type: macro, device: virtual"),
             "unsupported");
    rejected(replace(profile, "grab: false", "grab: maybe"), "must be a boolean");
    rejected(replace(profile, "version: 1", "version: 1\nversion: 1"), "duplicate key");
    std::ifstream controls_file(CONTROLS_PROFILE);
    check(controls_file.good(), "read control coverage profile for validation checks");
    const std::string controls_text(std::istreambuf_iterator<char>{controls_file}, {});
    rejected(replace(controls_text, "0: {min: -32768, max: 32767, neutral: 0}",
             "0: {min: -32768, max: 32767, neutral: 50000}"),
             "min <= neutral <= max");
    rejected(replace(controls_text, "action: {type: axis, device: virtual_stick, axis: 0}",
             "action: {type: axis, device: virtual_stick, axis: 8}"),
             "requires declared output axis");
    rejected(replace(controls_text, "input: {device: left, axis: 0}",
             "input: {device: left, axis: 16}"), "hat axis; use 'hat'");
    rejected(replace(controls_text, "hat: {axis: 16, direction: -1}",
             "hat: {axis: 16, direction: 0}"), "must be -1 or 1");
    rejected(replace(controls_text, "hat: {axis: 16, direction: -1}",
             "hat: {axis: 0, direction: -1}"), "ABS_HAT* axis code");
    rejected(replace(controls_text, "action: {type: axis, device: virtual_stick, axis: 0}",
             "action: {type: button, device: virtual_hat, button: 1}"),
             "absolute-axis inputs require axis actions");
    rejected(replace(controls_text, "2: {min: 0, max: 255, neutral: 0}",
             "16: {min: -1, max: 1, neutral: 0}"), "hat axis; use 'hat'");
    rejected(replace(profile, "action: {type: button, device: virtual, button: 2}",
              "actions: []"), "actions cannot be empty");
    rejected(replace(profile, "action: {type: button, device: virtual, button: 2}",
              "action: {type: button, device: virtual, button: 2}\n"
              "    actions: [{type: button, device: virtual, button: 4}]"),
             "exactly one of 'action' or 'actions'");
    rejected(replace(multiple_actions, "- {type: button, device: virtual, button: 4}",
              "- {type: button, device: virtual, button: 2}"),
             "repeats output control");
    rejected(replace(multiple_actions, "- {type: button, device: virtual, button: 4}",
              "- {type: button, device: missing, button: 4}"),
             "unknown or wrong-kind device");
    rejected(replace(profile, "input: {device: physical, button: 1}\n    modes: [default]\n"
              "    modifiers: [shift]", "input: {device: physical, button: 4}\n"
             "    modes: [default]\n    modifiers: [shift]"),
             "cannot require its own modifier");

    const auto disjoint = profile +
        "  - input: {device: physical, button: 1}\n"
        "    modes: [alternate]\n    modifiers: [shift]\n"
        "    action: {type: button, device: virtual, button: 4}\n";
    check(load_config(disjoint).bindings.size() == 3,
          "equally specific bindings in different modes do not conflict");
    rejected(replace(disjoint, "modes: [alternate]\n    modifiers: [shift]",
                     "modes: [default]\n    modifiers: [shift]"),
             "conflicts with bindings[1]");
    const auto two_modifiers = replace(profile,
        "  shift: {inputs: [{device: physical, button: 4}]}\nbindings:",
        "  shift: {inputs: [{device: physical, button: 4}]}\n"
        "  layer: {inputs: [{device: shared, button: 5}]}\nbindings:");
    rejected(two_modifiers +
              "  - input: {device: physical, button: 1}\n"
             "    modes: [default]\n    modifiers: [layer]\n"
              "    action: {type: button, device: virtual, button: 4}\n",
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
