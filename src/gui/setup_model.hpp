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
void mirror_button_labels(joystick_penguin::Config& config, const std::string& source, const std::string& target);
void refresh_virtual_device(joystick_penguin::Config& config, const std::string& source, const std::string& target,
                            const std::map<int, joystick_penguin::AxisRange>& axes);

} // namespace profile_setup
