#pragma once

#include "joystick_penguin/config.hpp"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace joystick_penguin {

// The Linux joystick EV_KEY range we expose consistently to Wine/SDL.
// KEY_MAX (767) is excluded because current Wine and SDL evdev paths skip it.
constexpr int joystick_button_count = 79;
int joystick_button_code(int number); // One-based; throws ConfigError if invalid.
std::map<int, AxisRange> joystick_axes();
AxisRange axis_resolution(int bits, bool zero_neutral);
std::optional<std::pair<int, bool>> axis_resolution_settings(AxisRange range);
std::set<int> joystick_hats();

// Indexed physical inputs are normalized to private, negative engine keys;
// positive keys remain literal Linux EV_KEY codes.
constexpr int button_index_key(int number) { return -number; }

} // namespace joystick_penguin
