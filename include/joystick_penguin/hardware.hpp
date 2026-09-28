#pragma once

#include "joystick_penguin/config.hpp"
#include "joystick_penguin/speech.hpp"

#include <string>

namespace joystick_penguin {

// The optional path enables SIGHUP and interactive reload of the running
// profile. The optional speaker announces persistent-mode changes.
int run_hardware(const Config& config, const std::string& profile_path = {},
                 Speaker* speaker = nullptr);

} // namespace joystick_penguin
