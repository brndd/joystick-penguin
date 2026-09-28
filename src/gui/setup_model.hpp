#pragma once

#include "joystick_penguin/config.hpp"

#include <string>

namespace profile_setup {

void rename_device(joystick_penguin::Config& config, const std::string& from, const std::string& to);
void remove_device(joystick_penguin::Config& config, const std::string& name);
void rename_mode(joystick_penguin::Config& config, const std::string& from, const std::string& to);
void remove_mode(joystick_penguin::Config& config, const std::string& name);
void rename_modifier(joystick_penguin::Config& config, const std::string& from, const std::string& to);
void remove_modifier(joystick_penguin::Config& config, const std::string& name);
void remove_axis(joystick_penguin::Config& config, const std::string& device, int code);

} // namespace profile_setup
