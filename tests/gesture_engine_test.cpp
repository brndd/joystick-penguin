#include "joystick_penguin/gesture_engine.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace joystick_penguin;

namespace {

using Expected = std::tuple<std::string, int, int>;

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void expect(const std::vector<OutputEvent>& actual, std::vector<Expected> wanted,
            const std::string& reason) {
    check(actual.size() == wanted.size(), reason + ": output count");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto& [device, code, value] = wanted[index];
        check(actual[index].kind == OutputEventKind::Button &&
              actual[index].device == device && actual[index].code == code &&
              actual[index].value == value, reason + ": unexpected output");
    }
}

InputEvent down(const std::string& device, int code) {
    return {device, InputEventKind::Button, code, 1};
}

InputEvent up(const std::string& device, int code) {
    return {device, InputEventKind::Button, code, 0};
}

Binding bind(const std::string& device, int code, std::vector<std::string> modifiers,
             std::vector<ButtonAction> actions) {
    std::vector<Action> outputs;
    for (auto& action : actions) outputs.emplace_back(std::move(action));
    return {{device, ControlKind::Button, code}, {"default"},
            std::move(modifiers), std::move(outputs)};
}

Config profile() {
    Config config;
    config.initial_mode = "default";
    config.modes = {"default"};
    config.devices.emplace("a", Device{DeviceKind::Evdev, "/dev/input/by-id/a"});
    config.devices.emplace("b", Device{DeviceKind::Evdev, "/dev/input/by-id/b"});
    config.devices.emplace("v1", Device{DeviceKind::Uinput, "", true, "joystick"});
    config.devices.emplace("v2", Device{DeviceKind::Uinput, "", true, "joystick"});
    config.modifiers.emplace("shift", Control{"a", ControlKind::Button, 307});
    config.modifiers.emplace("layer", Control{"b", ControlKind::Button, 311});
    config.bindings = {
        bind("a", 304, {}, {{"v1", 305}}),
        bind("a", 304, {"shift"}, {{"v1", 306}}),
        bind("a", 307, {}, {{"v1", 312}}),
        bind("a", 307, {"layer"}, {{"v1", 313}}),
        bind("a", 308, {}, {{"v1", 309}}),
        bind("a", 308, {"shift"}, {{"v2", 310}, {"v1", 311}}),
        bind("a", 308, {"shift", "layer"}, {{"v2", 314}}),
        bind("a", 315, {"shift"}, {{"v1", 316}}),
        bind("b", 304, {}, {{"v1", 305}}),
        bind("b", 308, {"shift"}, {{"v2", 320}}),
    };
    return config;
}

void test_required_sequence() {
    GestureEngine engine(profile());
    expect(engine.process(down("a", 304)), {{"v1", 305, 1}}, "A ordinary");
    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "modifier's own binding");
    expect(engine.process(down("a", 308)), {{"v2", 310, 1}, {"v1", 311, 1}},
           "B modified multi-action cross-output");
    expect(engine.process(up("a", 307)), {{"v1", 312, 0}}, "release modifier alone");
    expect(engine.process(up("a", 304)), {{"v1", 305, 0}}, "release A's captured output");
    expect(engine.process(up("a", 308)), {{"v2", 310, 0}, {"v1", 311, 0}},
           "release B's captured actions");
    expect(engine.release_all(), {}, "no output left after sequence");

    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "hold modifier again");
    expect(engine.process(down("a", 304)), {{"v1", 306, 1}}, "new A is modified");
    expect(engine.process(up("a", 307)), {{"v1", 312, 0}}, "modifier releases first");
    expect(engine.process(up("a", 304)), {{"v1", 306, 0}}, "modified A remains captured");
}

void test_pre_press_snapshot_and_specificity() {
    GestureEngine engine(profile());
    expect(engine.process(down("b", 311)), {}, "layer modifier has no own output");
    expect(engine.process(down("a", 307)), {{"v1", 313, 1}},
           "shift's own binding sees the previously held layer");
    expect(engine.process(down("a", 308)), {{"v2", 314, 1}},
           "both modifiers beat the single-modifier binding");
    expect(engine.process(up("b", 311)), {}, "other modifier's release cannot remap B");
    expect(engine.process(up("a", 307)), {{"v1", 313, 0}},
           "shift's own output releases without affecting B");
    expect(engine.process(up("a", 308)), {{"v2", 314, 0}},
           "most-specific binding remains captured");
}

void test_unmapped_and_repeats() {
    GestureEngine engine(profile());
    expect(engine.process(down("a", 315)), {}, "unmapped press recorded");
    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "press shift");
    expect(engine.process(down("a", 315)), {}, "duplicate down cannot activate held input");
    expect(engine.process({"a", InputEventKind::Button, 315, 2}), {}, "repeat cannot activate it");
    expect(engine.process(up("a", 315)), {}, "unmapped release does not emit a release");
    expect(engine.process(down("a", 315)), {{"v1", 316, 1}}, "fresh press now uses shift");
    expect(engine.process(up("a", 307)), {{"v1", 312, 0}}, "modifier output cleanup");
    expect(engine.process(up("a", 315)), {{"v1", 316, 0}}, "captured modified release");

    expect(engine.process(down("a", 304)), {{"v1", 305, 1}}, "fallback ordinary");
    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "modifier changes selection only");
    expect(engine.process(down("a", 304)), {}, "duplicate A must stay ordinary");
    expect(engine.process({"a", InputEventKind::Button, 304, 2}), {}, "A repeat ignored");
    expect(engine.process(up("a", 307)), {{"v1", 312, 0}}, "no retroactive remapping");
    expect(engine.process(up("a", 304)), {{"v1", 305, 0}}, "ordinary release");
}

void test_ownership_and_loss() {
    GestureEngine engine(profile());
    expect(engine.process(down("a", 304)), {{"v1", 305, 1}}, "first virtual owner");
    expect(engine.process(down("b", 304)), {}, "second owner shares virtual button");
    expect(engine.process({"a", InputEventKind::Disconnected}), {},
           "lost device cannot release another owner's button");
    expect(engine.process(up("a", 304)), {}, "stale release ignored");
    expect(engine.process(up("b", 304)), {{"v1", 305, 0}}, "last owner releases");

    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "shift press");
    expect(engine.process(down("b", 308)), {{"v2", 320, 1}}, "cross-controller modifier");
    expect(engine.process({"a", InputEventKind::SyncLost}), {{"v1", 312, 0}},
           "sync loss cleans only a's own actions and modifier");
    expect(engine.process(up("a", 307)), {}, "release after loss is inert");
    expect(engine.process(up("b", 308)), {{"v2", 320, 0}},
           "b retains the binding it selected while shift was held");
    expect(engine.process(down("b", 308)), {}, "shift was cleared by a's loss");
    expect(engine.process(up("b", 308)), {}, "no release for never-asserted output");

    expect(engine.process(down("a", 307)), {{"v1", 312, 1}}, "fresh modifier after loss");
    expect(engine.process(down("b", 308)), {{"v2", 320, 1}}, "fresh cross-controller press");
    expect(engine.release_all(), {{"v1", 312, 0}, {"v2", 320, 0}},
           "shutdown releases all owned outputs");
    expect(engine.release_all(), {}, "shutdown cleanup is idempotent");
}

void test_multiple_modes_are_supported() {
    auto config = profile();
    config.modes.push_back("alternate");
    GestureEngine engine(config);
    expect(engine.process(down("a", 304)), {{"v1", 305, 1}}, "initial mode remains usable");
    expect(engine.process(up("a", 304)), {{"v1", 305, 0}}, "multiple modes release");
}

} // namespace

int main() {
    try {
        test_required_sequence();
        test_pre_press_snapshot_and_specificity();
        test_unmapped_and_repeats();
        test_ownership_and_loss();
        test_multiple_modes_are_supported();
        std::cout << "gesture engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
