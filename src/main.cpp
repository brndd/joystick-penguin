// SPDX-License-Identifier: GPL-3.0-or-later
#include "joystick_penguin/config.hpp"

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: joystick-penguin <profile.yaml>\n";
        return 2;
    }

    try {
        const auto config = joystick_penguin::load_config_file(argv[1]);
        std::cout << "Valid profile: " << config.devices.size() << " devices, "
                  << config.bindings.size() << " bindings; initial mode: "
                  << config.initial_mode << '\n';
        return 0;
    } catch (const joystick_penguin::ConfigError& error) {
        std::cerr << "Profile error: " << error.what() << '\n';
        return 1;
    }
}
