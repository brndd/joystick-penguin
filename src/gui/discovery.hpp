#pragma once

#include <string>
#include <map>
#include <vector>
#include "joystick_penguin/config.hpp"

namespace profile_setup {

struct DiscoveredDevice {
    std::string path;
    std::vector<std::string> aliases;
    std::string name;
    std::vector<int> buttons; // Joystick-order EV_KEY codes (one-based in profiles).
    std::vector<int> axes;    // Linux EV_ABS codes, including hats.
    std::map<int, joystick_penguin::AxisRange> axis_ranges; // Ordinary axes only; hats stay fixed.
    std::string issue;        // Read/permission error; manual profile editing remains available.
};

// Inspect stable links only. Never grab or consume device events.
std::vector<DiscoveredDevice> discover_devices(const std::string& input_root = "/dev/input");
// Inspect a configured controller path (including manually entered links).
// On failure, issue is nonempty and no capabilities are returned.
DiscoveredDevice inspect_device(const std::string& path);

} // namespace profile_setup
