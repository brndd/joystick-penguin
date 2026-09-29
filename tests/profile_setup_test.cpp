#include "discovery.hpp"
#include "setup_model.hpp"
#include "joystick_penguin/config.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <vector>

using namespace joystick_penguin;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> void rejected(F&& change, const std::string& expected) {
    try { change(); }
    catch (const ConfigError& error) {
        check(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("expected rejection: " + expected);
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "jp-setup-test-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        check(mkdtemp(name.data()) != nullptr, "create fixture directory");
        path = name.data();
    }
    ~Directory() { std::filesystem::remove_all(path); }
};

void run() {
    check(axis_resolution(8, false) == AxisRange{0, 255, 128} &&
          axis_resolution(8, true) == AxisRange{-128, 127, 0} &&
          axis_resolution(10, false) == AxisRange{0, 1023, 512} &&
          axis_resolution(12, true) == AxisRange{-2048, 2047, 0} &&
          axis_resolution(16, false) == AxisRange{0, 65535, 32768}, "preset resolutions and neutrals");
    check(axis_resolution_settings({0, 4095, 2048}) == std::pair{12, false} &&
          axis_resolution_settings({-2048, 2047, 0}) == std::pair{12, true} &&
          !axis_resolution_settings({0, 2046, 1024}), "non-preset cloned axes need advanced mode");
    auto config = load_config_file(std::string(EXAMPLES_DIR) + "/star_citizen.yaml");
    const auto original = config;
    config.input_labels.push_back({{"left", ControlKind::Button, -1}, "Trigger"});
    profile_setup::rename_device(config, "left", "main-stick");
    check(config.input_labels.front().input.device == "main-stick", "device rename preserves labels on exact controls");
    profile_setup::rename_device(config, "left-vjoy", "main-vjoy");
    check(config.modifiers.at("leftmod").front().device == "main-stick" &&
          config.bindings.front().input.device == "main-stick" &&
          std::get<AxisAction>(config.bindings.front().actions.front()).device == "main-vjoy",
          "device renames update modifiers and actions");
    check(std::get<ButtonAction>(config.bindings[17].tap_hold->tap.front()).device == "main-vjoy",
          "device rename updates tap branch");
    profile_setup::rename_mode(config, "SCM Mode", "Combat Mode");
    check(config.initial_mode == "Combat Mode" && config.bindings.front().modes.front() == "Combat Mode" &&
          std::get<ModeAction>(config.bindings[18].tap_hold->hold.back()).mode == "Combat Mode",
          "mode rename updates initial, bindings, and hold action");
    profile_setup::rename_modifier(config, "leftmod", "shift");
    check(config.modifiers.contains("shift") && config.bindings[13].modifiers.front() == "shift",
          "modifier rename updates conditions");
    validate_edited_config(config);
    check(load_config(serialize_config(config)) == config, "renamed large profile round trips");
    rejected([&] { profile_setup::rename_device(config, "main-stick", "right"); }, "already in use");
    rejected([&] { profile_setup::remove_mode(config, "Combat Mode"); }, "initial mode");
    rejected([&] { profile_setup::remove_modifier(config, "shift"); }, "bindings[");
    check(config != original, "renames changed profile");

    auto physicalRemoval = config;
    physicalRemoval.modifiers.at("shift").push_back({"right", ControlKind::Button, -30});
    const auto originalBindings = physicalRemoval.bindings.size();
    profile_setup::remove_device(physicalRemoval, "main-stick");
    check(!physicalRemoval.devices.contains("main-stick") &&
          physicalRemoval.bindings.size() < originalBindings &&
          std::all_of(physicalRemoval.bindings.begin(), physicalRemoval.bindings.end(), [](const Binding& binding) {
              return binding.input.device == "right";
          }) && physicalRemoval.modifiers.at("shift") == std::vector<Control>{{"right", ControlKind::Button, -30}} &&
          physicalRemoval.input_labels.empty(), "physical removal drops its mappings and assignments, retaining other inputs");
    validate_edited_config(physicalRemoval);
    profile_setup::remove_device(physicalRemoval, "right");
    check(physicalRemoval.modifiers.contains("shift") && physicalRemoval.modifiers.at("shift").empty(),
          "modifier becomes orphaned after its last controller is removed");
    validate_edited_config(physicalRemoval);
    check(load_config(serialize_config(physicalRemoval)) == physicalRemoval, "orphaned modifier round trips");

    auto virtualRemoval = config;
    virtualRemoval.bindings.front().actions.push_back(AxisAction{"right-vjoy", 2});
    virtualRemoval.bindings[17].tap_hold->tap.push_back(ButtonAction{"right-vjoy", 300});
    const auto beforeVirtual = virtualRemoval.bindings.size();
    profile_setup::remove_device(virtualRemoval, "main-vjoy");
    check(virtualRemoval.bindings.size() < beforeVirtual &&
          virtualRemoval.bindings.front().actions == std::vector<Action>{AxisAction{"right-vjoy", 2}} &&
          std::any_of(virtualRemoval.bindings.begin(), virtualRemoval.bindings.end(), [](const Binding& binding) {
              return binding.tap_hold && binding.tap_hold->tap == std::vector<Action>{ButtonAction{"right-vjoy", 300}};
          }),
          "virtual removal preserves other output actions in ordinary and tap/hold mappings");
    for (const auto& binding : virtualRemoval.bindings) {
        auto noRemovedOutputs = [](const std::vector<Action>& actions) {
            return std::none_of(actions.begin(), actions.end(), [](const Action& action) {
                return std::visit([](const auto& value) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, ModeAction>) return false;
                    else return value.device == "main-vjoy";
                }, action);
            });
        };
        check(noRemovedOutputs(binding.actions) && (!binding.tap_hold ||
              (noRemovedOutputs(binding.tap_hold->tap) && noRemovedOutputs(binding.tap_hold->hold))),
              "virtual removal clears all references to the deleted output");
    }
    validate_edited_config(virtualRemoval);

    auto modes = load_config_file(std::string(EXAMPLES_DIR) + "/modes.yaml");
    profile_setup::rename_mode(modes, "alternate", "landing");
    check(std::get<ModeAction>(modes.bindings[4].tap_hold->hold.back()).mode == "landing",
          "rename updates mode action inside hold");
    rejected([&] { profile_setup::remove_mode(modes, "landing"); }, "bindings[");
    auto axes = load_config_file(std::string(EXAMPLES_DIR) + "/controls.yaml");
    axes.devices.at("virtual_stick").axes[8] = {-100, 100, 0};
    axes.bindings.push_back({{"left", ControlKind::AbsoluteAxis, 3}, {"default"}, {},
                             {AxisAction{"virtual_stick", 8}}});
    rejected([&] { profile_setup::remove_axis(axes, "virtual_stick", 8); }, "used by a binding");
    axes.bindings.pop_back();
    profile_setup::remove_axis(axes, "virtual_stick", 8);
    check(!axes.devices.at("virtual_stick").axes.contains(8), "unused extra axis removed");
    profile_setup::remove_axis(axes, "virtual_stick", 2);
    check(axes.devices.at("virtual_stick").axes.at(2) == joystick_axes().at(2), "preset axis resets to default");
    validate_edited_config(axes);
    axes.devices.emplace("unused", Device{DeviceKind::Evdev, "/dev/input/by-id/absent", true, ""});
    axes.input_labels.push_back({{"unused", ControlKind::Button, -2}, "Unmapped control"});
    profile_setup::remove_device(axes, "unused");
    check(axes.input_labels.empty(), "removing an unused controller removes its labels");

    Directory fixture;
    std::filesystem::create_directory(fixture.path / "by-id");
    std::filesystem::create_directory(fixture.path / "by-path");
    std::filesystem::create_symlink("../missing-event0", fixture.path / "by-id" / "joystick-event-joystick");
    const auto found = profile_setup::discover_devices(fixture.path.string());
    check(found.size() == 1 && found[0].path.find("by-id") != std::string::npos &&
          !found[0].issue.empty(), "discovery reports missing stable links without hardware");
    {
        std::ofstream fixtureNode(fixture.path / "event0");
        check(fixtureNode.good(), "create non-evdev fixture node");
    }
    std::filesystem::create_symlink("../event0", fixture.path / "by-id" / "second-event-joystick");
    std::filesystem::create_symlink("../event0", fixture.path / "by-path" / "second-event-joystick");
    const auto withAliases = profile_setup::discover_devices(fixture.path.string());
    check(withAliases.size() == 2 && withAliases[0].aliases.size() + withAliases[1].aliases.size() == 1,
          "by-id and by-path links to the same event node are grouped");
}

} // namespace

int main() {
    try { run(); std::cout << "profile setup tests passed\n"; }
    catch (const std::exception& error) { std::cerr << "setup test failed: " << error.what() << '\n'; return 1; }
}
