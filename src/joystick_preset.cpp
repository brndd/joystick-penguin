#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>

namespace joystick_penguin {

AxisRange axis_resolution(int bits, bool zero_neutral) {
    if (bits < 8 || bits > 16) throw ConfigError("axis resolution must be between 8 and 16 bits");
    const int half = 1 << (bits - 1);
    return zero_neutral ? AxisRange{-half, half - 1, 0} : AxisRange{0, 2 * half - 1, half};
}

std::optional<std::pair<int, bool>> axis_resolution_settings(AxisRange range) {
    for (int bits = 8; bits <= 16; ++bits)
        for (bool zero : {false, true})
            if (range == axis_resolution(bits, zero)) return std::pair{bits, zero};
    return std::nullopt;
}

int joystick_button_code(int number) {
    if (number < 1 || number > joystick_button_count)
        throw ConfigError("joystick button number must be between 1 and 79");
    return number <= 16 ? BTN_TRIGGER + number - 1 : BTN_TRIGGER_HAPPY + number - 17;
}

std::map<int, AxisRange> joystick_axes() {
    std::map<int, AxisRange> result;
    for (int code = ABS_X; code <= ABS_RUDDER; ++code)
        result.emplace(code, axis_resolution(12, false));
    return result;
}

std::set<int> joystick_hats() {
    std::set<int> result;
    for (int code = ABS_HAT0X; code <= ABS_HAT3Y; ++code) result.insert(code);
    return result;
}

} // namespace joystick_penguin
