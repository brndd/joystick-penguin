#pragma once

#include "joystick_penguin/config.hpp"

#include <string>

namespace joystick_penguin {

// The optional path enables SIGHUP and interactive reload of the running profile.
int run_hardware(const Config& config, const std::string& profile_path = {});

} // namespace joystick_penguin
