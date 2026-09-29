#include "joystick_penguin/config.hpp"

#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace joystick_penguin {
namespace {

YAML::Node strings(const std::vector<std::string>& values) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto& value : values) node.push_back(value);
    return node;
}

YAML::Node control(const Control& value) {
    YAML::Node node(YAML::NodeType::Map);
    node["device"] = value.device;
    switch (value.kind) {
    case ControlKind::Button:
        if (value.code == std::numeric_limits<int>::min())
            throw ConfigError("button index is out of permitted range");
        if (value.code < 0) node["button"] = -value.code;
        else node["button_code"] = value.code;
        break;
    case ControlKind::AbsoluteAxis:
        node["axis"] = value.code;
        break;
    case ControlKind::HatDirection:
        node["hat"]["axis"] = value.code;
        node["hat"]["direction"] = value.direction;
        break;
    }
    return node;
}

YAML::Node action(const Action& value) {
    YAML::Node node(YAML::NodeType::Map);
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, ModeAction>) {
            node["type"] = "mode";
            node["mode"] = item.mode;
        } else {
            node["device"] = item.device;
            if constexpr (std::is_same_v<T, ButtonAction>) {
                node["type"] = "button";
                // Literal codes also cover preset-indexed virtual buttons.
                node["button_code"] = item.code;
            } else if constexpr (std::is_same_v<T, HatAction>) {
                node["type"] = "hat";
                node["axis"] = item.code;
                node["direction"] = item.direction;
            } else {
                node["type"] = "axis";
                node["axis"] = item.code;
                node["invert"] = item.invert;
            }
        }
    }, value);
    return node;
}

void actions(YAML::Node& node, const std::vector<Action>& values) {
    YAML::Node list(YAML::NodeType::Sequence);
    for (const auto& value : values) list.push_back(action(value));
    node["actions"] = list;
}

std::string emit_config(const Config& config) {
    YAML::Node root(YAML::NodeType::Map);
    root["version"] = 1;
    YAML::Node devices(YAML::NodeType::Map);
    for (const auto& [name, device] : config.devices) {
        YAML::Node entry(YAML::NodeType::Map);
        if (device.kind == DeviceKind::Evdev) {
            entry["kind"] = "evdev";
            entry["path"] = device.path;
            entry["grab"] = device.grab;
        } else {
            entry["kind"] = "uinput";
            entry["preset"] = device.preset;
            // Empty name is the loader's implicit default and cannot be a YAML field.
            if (!device.virtual_name.empty()) entry["name"] = device.virtual_name;
            entry["bus"] = device.bus == VirtualBus::Usb ? "usb" : "virtual";
            entry["vendor_id"] = device.vendor_id;
            entry["product_id"] = device.product_id;
            YAML::Node axes(YAML::NodeType::Map);
            for (const auto& [code, range] : device.axes) {
                YAML::Node axis(YAML::NodeType::Map);
                axis["min"] = range.minimum;
                axis["max"] = range.maximum;
                axis["neutral"] = range.neutral;
                axes[std::to_string(code)] = axis;
            }
            entry["axes"] = axes;
        }
        devices[name] = entry;
    }
    root["devices"] = devices;
    root["modes"]["initial"] = config.initial_mode;
    root["modes"]["names"] = strings(config.modes);
    YAML::Node modifiers(YAML::NodeType::Map);
    for (const auto& [name, inputs] : config.modifiers) {
        YAML::Node entries(YAML::NodeType::Sequence);
        for (const auto& input : inputs) entries.push_back(control(input));
        modifiers[name]["inputs"] = entries;
    }
    root["modifiers"] = modifiers;
    if (!config.input_labels.empty()) {
        YAML::Node labels(YAML::NodeType::Sequence);
        for (const auto& label : config.input_labels) {
            YAML::Node entry(YAML::NodeType::Map);
            entry["input"] = control(label.input);
            entry["label"] = label.label;
            labels.push_back(entry);
        }
        root["input_labels"] = labels;
    }
    if (!config.output_labels.empty()) {
        YAML::Node labels(YAML::NodeType::Sequence);
        for (const auto& label : config.output_labels) {
            YAML::Node entry(YAML::NodeType::Map);
            entry["device"] = label.device;
            entry["button"] = label.button;
            entry["label"] = label.label;
            labels.push_back(entry);
        }
        root["output_labels"] = labels;
    }
    YAML::Node bindings(YAML::NodeType::Sequence);
    for (const auto& binding : config.bindings) {
        YAML::Node entry(YAML::NodeType::Map);
        entry["input"] = control(binding.input);
        entry["modes"] = strings(binding.modes);
        if (!binding.modifiers.empty()) entry["modifiers"] = strings(binding.modifiers);
        if (binding.tap_hold) {
            const auto& timing = *binding.tap_hold;
            entry["threshold_ms"] = timing.threshold_ms;
            entry["tap_ms"] = timing.tap_ms;
            if (!timing.tap.empty()) {
                YAML::Node branch(YAML::NodeType::Map);
                actions(branch, timing.tap);
                entry["tap"] = branch;
            }
            if (!timing.hold.empty()) {
                YAML::Node branch(YAML::NodeType::Map);
                actions(branch, timing.hold);
                entry["hold"] = branch;
            }
        } else {
            actions(entry, binding.actions);
        }
        bindings.push_back(entry);
    }
    root["bindings"] = bindings;
    YAML::Emitter emitter;
    emitter << root;
    if (!emitter.good()) throw ConfigError("cannot serialize profile: " + emitter.GetLastError());
    return emitter.c_str();
}

std::string checked_yaml(const Config& config) {
    const auto yaml = emit_config(config);
    const auto reloaded = load_config(yaml);
    if (reloaded != config)
        throw ConfigError("edited profile contains values that cannot be represented by version 1 YAML");
    return yaml;
}

[[noreturn]] void file_error(const std::string& path, const char* operation) {
    throw ConfigError(path + ": " + operation + ": " + std::strerror(errno));
}

} // namespace

std::string serialize_config(const Config& config) { return checked_yaml(config); }

void validate_edited_config(const Config& config) { (void)checked_yaml(config); }

std::vector<ConfigIssue> config_issues(const Config& config) {
    std::vector<ConfigIssue> issues;
    Config isolated = config;
    isolated.bindings.clear();
    bool nonBindingIssue = false;
    try { validate_edited_config(isolated); }
    catch (const ConfigError& error) { issues.push_back({error.what(), {}}); nonBindingIssue = true; }
    // Fast path: a profile whose full serialization round-trips has no
    // binding-local or precedence issues. Avoid the per-binding serialization
    // loop below, which is what made issue recalculation expensive.
    try { validate_edited_config(config); return issues; }
    catch (const ConfigError&) {}
    // Validate each mapping independently through the same serializer and loader.
    // Locations are attached by construction, never extracted from error prose.
    if (!nonBindingIssue) {
        for (std::size_t i = 0; i < config.bindings.size(); ++i) {
            isolated.bindings = {config.bindings[i]};
            try { validate_edited_config(isolated); }
            catch (const ConfigError& error) {
                std::string message = error.what();
                if (message.starts_with("bindings[0]")) message.replace(0, 11, "bindings[" + std::to_string(i) + "]");
                issues.push_back({std::move(message), {i}});
            }
        }
    }
    for (std::size_t i = 0; i < config.bindings.size(); ++i) {
        const auto& binding = config.bindings[i];
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = config.bindings[j];
            bool overlap = std::any_of(binding.modes.begin(), binding.modes.end(), [&](const auto& mode) {
                return std::find(other.modes.begin(), other.modes.end(), mode) != other.modes.end();
            });
            if (binding.input == other.input && binding.modifiers.size() == other.modifiers.size() && overlap)
                issues.push_back({"Mapping conflicts at equal specificity in overlapping modes. Change In modes or While held, or remove a duplicate.", {i, j}});
        }
    }
    return issues;
}

void save_config_file(const Config& config, const std::string& path) {
    const auto yaml = checked_yaml(config);
    const std::filesystem::path destination(path);
    auto temporary = (destination.parent_path() / (destination.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) file_error(path, "create temporary file");
    const std::string temp_path(name.data());
    try {
        struct stat existing{};
        if (stat(path.c_str(), &existing) == 0) {
            if (fchmod(fd, existing.st_mode & 07777) != 0) file_error(path, "preserve permissions");
        } else if (errno != ENOENT) {
            file_error(path, "inspect existing file");
        }
        std::size_t offset = 0;
        while (offset < yaml.size()) {
            const auto written = write(fd, yaml.data() + offset, yaml.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) file_error(path, "write temporary file");
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(fd) != 0) file_error(path, "sync temporary file");
        if (close(fd) != 0) { fd = -1; file_error(path, "close temporary file"); }
        fd = -1;
        // Validation on disk catches any discrepancy before replacing the original.
        if (load_config_file(temp_path) != config)
            throw ConfigError(path + ": temporary profile changed during write");
        if (rename(temp_path.c_str(), path.c_str()) != 0) file_error(path, "replace profile");
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(temp_path.c_str());
        throw;
    }
}

} // namespace joystick_penguin
