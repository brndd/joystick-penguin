// SPDX-License-Identifier: GPL-3.0-or-later
#include "joystick_penguin/config.hpp"
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
    check(example.devices.size() == 2 && example.bindings.size() == 2,
          "example profile should load from disk");
    check(config.devices.size() == 3 && config.bindings.size() == 2, "profile size");
    check(config.devices.at("physical").grab, "default grab");
    check(!config.devices.at("shared").grab, "explicit shared device");
    check(config.initial_mode == "default", "initial mode");
    check(config.modifiers.at("shift").code == 307, "modifier code");
    check(config.bindings[0].action.code == 305, "output code");
    check(config.bindings[1].modifiers == std::vector<std::string>{"shift"},
          "modified binding");

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
