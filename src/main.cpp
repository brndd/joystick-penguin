#include "joystick_penguin/config.hpp"
#include "joystick_penguin/hardware.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    const bool check_only = argc == 3 && std::string_view(argv[1]) == "--check";
    if ((argc != 2 || std::string_view(argv[1]) == "--check") && !check_only) {
        std::cerr << "Usage: joystick-penguin [--check] <profile.yaml>\n";
        return 2;
    }

    try {
        const auto config = joystick_penguin::load_config_file(argv[check_only ? 2 : 1]);
        if (check_only) {
            std::cout << "Valid profile: " << config.devices.size() << " devices, "
                      << config.bindings.size() << " bindings; initial mode: "
                      << config.initial_mode << '\n';
            return 0;
        }
        return joystick_penguin::run_hardware(config, argv[1]);
    } catch (const joystick_penguin::ConfigError& error) {
        std::cerr << "Profile error: " << error.what() << '\n';
        return 1;
    }
}
