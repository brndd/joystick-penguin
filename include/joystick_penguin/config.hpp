// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace joystick_penguin {

enum class DeviceKind { Evdev, Uinput };

struct Device {
    DeviceKind kind;
    std::string path;    // Stable evdev path, e.g. /dev/input/by-id/...
    bool grab = true;    // Applies only to evdev devices.
    std::string preset;  // Applies only to uinput devices.
};

// Codes are Linux evdev event codes, not ordinal button numbers.
enum class ControlKind { Button, AbsoluteAxis };

struct Control {
    std::string device;
    ControlKind kind;
    int code;
};

struct ButtonAction {
    std::string device;
    int code;
};

struct Binding {
    Control input;
    std::vector<std::string> modes;
    std::vector<std::string> modifiers;
    ButtonAction action;
};

struct Config {
    std::map<std::string, Device> devices;
    std::string initial_mode;
    std::vector<std::string> modes;
    std::map<std::string, Control> modifiers;
    std::vector<Binding> bindings;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Both entry points validate the entire document before returning a typed config.
Config load_config(const std::string& yaml_text);
Config load_config_file(const std::string& path);

} // namespace joystick_penguin
