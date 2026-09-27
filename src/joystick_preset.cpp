#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>

namespace joystick_penguin {

int joystick_button_code(int number) {
    if (number < 1 || number > joystick_button_count)
        throw ConfigError("joystick button number must be between 1 and 79");
    return number <= 16 ? BTN_TRIGGER + number - 1 : BTN_TRIGGER_HAPPY + number - 17;
}

std::map<int, AxisRange> joystick_axes() {
    std::map<int, AxisRange> result;
    for (int code = ABS_X; code <= ABS_RUDDER; ++code)
        result.emplace(code, AxisRange{-32768, 32767, 0});
    return result;
}

std::set<int> joystick_hats() {
    std::set<int> result;
    for (int code = ABS_HAT0X; code <= ABS_HAT3Y; ++code) result.insert(code);
    return result;
}

} // namespace joystick_penguin
