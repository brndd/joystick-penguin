#include "joystick_penguin/button_router.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace joystick_penguin;

namespace {

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void expect_button(const std::vector<OutputEvent>& events, int code, int value,
                   const std::string& reason) {
    check(events.size() == 1 && events[0].device == "virtual_b" &&
          events[0].kind == OutputEventKind::Button && events[0].code == code &&
          events[0].value == value, reason);
}

Config profile() {
    Config config;
    config.initial_mode = "default";
    config.modes = {"default"};
    config.devices.emplace("physical_a", Device{DeviceKind::Evdev, "/dev/input/by-id/a"});
    config.devices.emplace("physical_b", Device{DeviceKind::Evdev, "/dev/input/by-id/b"});
    config.devices.emplace("virtual_b", Device{DeviceKind::Uinput, "", true, "joystick"});
    config.bindings = {
        {{"physical_a", ControlKind::Button, 304}, {"default"}, {}, {"virtual_b", 305}},
        {{"physical_b", ControlKind::Button, 307}, {"default"}, {}, {"virtual_b", 305}},
        {{"physical_a", ControlKind::Button, 308}, {"default"}, {}, {"virtual_b", 306}},
    };
    return config;
}

void run() {
    const auto config = profile();
    ButtonRouter router(config);
    expect_button(router.process({"physical_a", InputEventKind::Button, 304, 1}), 305, 1,
                  "first owner presses");
    check(router.process({"physical_a", InputEventKind::Button, 304, 1}).empty(),
          "duplicate press ignored");
    check(router.process({"physical_a", InputEventKind::Button, 304, 2}).empty(),
          "repeat ignored");
    check(router.process({"physical_b", InputEventKind::Button, 307, 1}).empty(),
          "second owner does not duplicate the press");
    expect_button(router.process({"physical_a", InputEventKind::Button, 308, 1}), 306, 1,
                  "cross-controller output");
    expect_button(router.process({"physical_a", InputEventKind::Disconnected}), 306, 0,
                  "device loss releases only unique output");
    check(router.process({"physical_a", InputEventKind::Button, 304, 0}).empty(),
          "stale release cannot affect another owner");
    check(router.process({"physical_a", InputEventKind::Button, 304, 1}).empty(),
          "reconnected input shares the held output");
    check(router.process({"physical_b", InputEventKind::SyncLost}).empty(),
          "sync loss retains remaining owner's output");
    expect_button(router.process({"physical_a", InputEventKind::Button, 304, 0}), 305, 0,
                  "last owner releases");
    check(router.process({"physical_b", InputEventKind::Button, 307, 0}).empty(),
          "release after sync loss does not reassert");
    expect_button(router.process({"physical_b", InputEventKind::Button, 307, 1}), 305, 1,
                  "fresh press after release works");
    expect_button(router.release_all(), 305, 0, "shutdown releases outputs");
    check(router.release_all().empty(), "shutdown is idempotent");

    auto multiple_outputs = profile();
    multiple_outputs.devices.emplace("virtual_a", Device{DeviceKind::Uinput, "", true, "joystick"});
    multiple_outputs.bindings.push_back(
        {{"physical_a", ControlKind::Button, 309}, {"default"}, {}, {"virtual_a", 310}});
    ButtonRouter multi(multiple_outputs);
    const auto press = multi.process({"physical_a", InputEventKind::Button, 309, 1});
    check(press.size() == 1 && press[0].device == "virtual_a" && press[0].code == 310 &&
          press[0].value == 1, "routing to another named virtual device");
    const auto cleanup = multi.process({"physical_a", InputEventKind::Disconnected});
    check(cleanup.size() == 1 && cleanup[0].device == "virtual_a" && cleanup[0].code == 310 &&
          cleanup[0].value == 0, "cleanup on another virtual device");

    auto unsupported = profile();
    unsupported.modes.push_back("alternate");
    try {
        (void)ButtonRouter(unsupported);
        throw std::runtime_error("accepted a mode that the hardware bridge cannot implement");
    } catch (const ConfigError&) {
        // The hardware bridge must not silently run only part of a profile.
    }
}

} // namespace

int main() {
    try {
        run();
        std::cout << "button router tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
