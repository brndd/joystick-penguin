#pragma once

#include "joystick_penguin/config.hpp"

namespace joystick_penguin {

// Runs until SIGINT/SIGTERM. Reports startup or output failures via stderr.
int run_hardware(const Config& config);

} // namespace joystick_penguin
