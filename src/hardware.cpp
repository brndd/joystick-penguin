#include "joystick_penguin/hardware.hpp"

#include "joystick_penguin/gesture_engine.hpp"
#include "joystick_penguin/io.hpp"
#include "joystick_penguin/joystick_preset.hpp"
#include "joystick_penguin/output_frames.hpp"

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <expected>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace joystick_penguin {
namespace {

volatile std::sig_atomic_t stopping = 0;
volatile std::sig_atomic_t reload_requested = 0;

void stop_signal(int) { stopping = 1; }
void reload_signal(int) { reload_requested = 1; }

std::string error_text(const std::string& context, int error) {
    return context + ": " + std::strerror(error);
}

struct InputRequirements {
    std::set<int> keys;
    std::set<int> axes;
};

InputRequirements input_requirements(const Config& config, const std::string& name) {
    InputRequirements result;
    for (const auto& binding : config.bindings)
        if (binding.input.device == name) {
            if (binding.input.kind == ControlKind::Button) result.keys.insert(binding.input.code);
            else result.axes.insert(binding.input.code);
        }
    for (const auto& [modifier, controls] : config.modifiers)
        for (const auto& control : controls)
            if (control.device == name) result.keys.insert(control.code);
    return result;
}

std::set<int> advertised_buttons(const Config& config, const std::string& name) {
    std::set<int> result;
    for (int index = 1; index <= joystick_button_count; ++index)
        result.insert(joystick_button_code(index));
    auto collect = [&](const std::vector<Action>& actions) {
        for (const auto& action : actions)
            if (const auto* button = std::get_if<ButtonAction>(&action); button && button->device == name)
                result.insert(button->code);
    };
    for (const auto& binding : config.bindings) {
        collect(binding.actions);
        if (binding.tap_hold) {
            collect(binding.tap_hold->tap);
            collect(binding.tap_hold->hold);
        }
    }
    return result;
}

std::expected<void, std::string> virtual_devices_compatible(const Config& current, const Config& next) {
    for (const auto& [name, device] : current.devices) {
        if (device.kind != DeviceKind::Uinput) continue;
        const auto found = next.devices.find(name);
        if (found == next.devices.end() || found->second.kind != DeviceKind::Uinput)
            return std::unexpected("virtual device '" + name + "' was removed or changed kind; restart required");
        const auto& replacement = found->second;
        if (device.preset != replacement.preset || device.axes != replacement.axes ||
            device.vendor_id != replacement.vendor_id || device.product_id != replacement.product_id ||
            device.bus != replacement.bus || device.virtual_name != replacement.virtual_name ||
            advertised_buttons(current, name) != advertised_buttons(next, name))
            return std::unexpected("virtual device '" + name + "' changed identity or capabilities; restart required");
    }
    for (const auto& [name, device] : next.devices)
        if (device.kind == DeviceKind::Uinput &&
            (!current.devices.contains(name) || current.devices.at(name).kind != DeviceKind::Uinput))
            return std::unexpected("virtual device '" + name + "' was added; restart required");
    return {};
}

class InteractiveTerminal {
public:
    explicit InteractiveTerminal(bool enabled) {
        if (!enabled || !isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) < 0) return;
        auto single_key = saved_;
        single_key.c_lflag &= ~(ICANON | ECHO);
        single_key.c_cc[VMIN] = 1;
        single_key.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &single_key) == 0;
    }
    ~InteractiveTerminal() {
        if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    InteractiveTerminal(const InteractiveTerminal&) = delete;
    InteractiveTerminal& operator=(const InteractiveTerminal&) = delete;
    bool active() const { return active_; }

private:
    termios saved_{};
    bool active_ = false;
};

// libevdev does not own an input fd supplied to libevdev_new_from_fd().
class EvdevInput final : public InputBackend {
public:
    EvdevInput(std::string name, std::string path, bool grab,
               std::set<int> keys, std::set<int> axes)
        : name_(std::move(name)), path_(std::move(path)), grab_(grab),
          codes_(std::move(keys)), axes_(std::move(axes)) {}
    ~EvdevInput() override { close_device(); }
    EvdevInput(const EvdevInput&) = delete;
    EvdevInput& operator=(const EvdevInput&) = delete;

    int descriptor() const override { return fd_; }
    dev_t node_id() const { return node_id_; }
    const std::string& name() const { return name_; }
    bool connected() const { return fd_ >= 0; }

    std::expected<void, std::string> validate_configuration(
        const std::set<int>& keys, const std::set<int>& axes) const {
        if (!connected()) return {};
        if (const auto result = resolve_keys(keys); !result) return std::unexpected(result.error());
        return validate_axes(axes);
    }

    std::expected<void, std::string> reconfigure(std::set<int> keys, std::set<int> axes) {
        codes_ = std::move(keys);
        axes_ = std::move(axes);
        original_keys_.reset(); // The profile may watch a different set of controls.
        if (!connected()) return {};
        const auto resolved = resolve_keys(codes_);
        if (!resolved) return std::unexpected(resolved.error());
        resolved_keys_ = *resolved;
        pending_frame_.clear();
        if (const auto drained = drain_and_sync(); !drained) {
            close_device();
            return drained;
        }
        suppress_held();
        original_keys_ = resolved_keys_;
        return {};
    }

    std::expected<void, std::string> connect() {
        const int fd = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return std::unexpected(error_text(path_, errno));
        fd_ = fd;
        auto fail = [this](std::string message) -> std::expected<void, std::string> {
            close_device();
            return std::unexpected(std::move(message));
        };
        struct stat info{};
        if (fstat(fd_, &info) < 0) return fail(error_text(path_, errno));
        node_id_ = info.st_rdev;
        libevdev* device = nullptr;
        if (const int rc = libevdev_new_from_fd(fd_, &device); rc < 0)
            return fail(error_text(path_ + " is not a readable evdev device", -rc));
        dev_ = device;

        // The path, rather than a model name, identifies the configured unit.
        // Reject a different identity if a symlink is ever repointed to one.
        const Identity current{libevdev_get_id_bustype(dev_), libevdev_get_id_vendor(dev_),
                               libevdev_get_id_product(dev_), libevdev_get_id_version(dev_),
                               libevdev_get_uniq(dev_) ? libevdev_get_uniq(dev_) : ""};
        if (identity_ && *identity_ != current)
            return fail(path_ + " now points to a different device identity");
        const auto resolved = resolve_keys(codes_);
        if (!resolved) return fail(resolved.error());
        resolved_keys_ = *resolved;
        if (original_keys_ && *original_keys_ != resolved_keys_)
            return fail(path_ + " button layout changed since the first connection");
        if (const auto result = validate_axes(axes_); !result) return fail(result.error());
        if (grab_) {
            if (const int rc = libevdev_grab(dev_, LIBEVDEV_GRAB); rc < 0)
                return fail(error_text(path_ + " EVIOCGRAB failed", -rc));
            grabbed_ = true;
        }

        // libevdev's initial snapshot can already include queued events. Drain
        // them without forwarding, then resynchronize and suppress held keys.
        if (const auto result = drain_and_sync(); !result) return fail(result.error());
        suppress_held();
        original_keys_ = resolved_keys_;
        identity_ = current;
        return {};
    }

    std::vector<InputEvent> baselines() const {
        std::vector<InputEvent> events;
        for (const int code : axes_) {
            const auto* info = libevdev_get_abs_info(dev_, code);
            events.push_back({name_, InputEventKind::AxisBaseline, code, info->value,
                              info->minimum, info->maximum});
        }
        return events;
    }

    std::expected<std::vector<InputEvent>, std::string> read_events() override {
        std::vector<InputEvent> events;
        input_event event{};
        while (true) {
            const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_NORMAL, &event);
            if (rc == -EAGAIN) return events;
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                pending_frame_.clear();
                events.push_back({name_, InputEventKind::SyncLost});
                if (const auto result = sync(); !result) return std::unexpected(result.error());
                suppress_held();
                const auto state = baselines();
                events.insert(events.end(), state.begin(), state.end());
            } else if (rc < 0) {
                return std::unexpected(error_text(path_ + " read", -rc));
            } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
                events.insert(events.end(), pending_frame_.begin(), pending_frame_.end());
                pending_frame_.clear();
                events.push_back({name_, InputEventKind::FrameEnd});
            } else if (event.type == EV_KEY && resolved_keys_.contains(event.code)) {
                if (event.value == 0) {
                    suppressed_.erase(event.code);
                    pending_frame_.push_back({name_, InputEventKind::Button, resolved_keys_.at(event.code), 0});
                } else if (!suppressed_.contains(event.code)) {
                    pending_frame_.push_back({name_, InputEventKind::Button,
                                              resolved_keys_.at(event.code), event.value});
                }
            } else if (event.type == EV_ABS && axes_.contains(event.code)) {
                const auto* info = libevdev_get_abs_info(dev_, event.code);
                pending_frame_.push_back({name_, InputEventKind::AbsoluteAxis, event.code, event.value,
                                          info->minimum, info->maximum});
            }
        }
    }

    void close_device() {
        if (dev_) {
            if (grabbed_) libevdev_grab(dev_, LIBEVDEV_UNGRAB);
            libevdev_free(dev_);
            dev_ = nullptr;
        }
        grabbed_ = false;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        node_id_ = 0;
        suppressed_.clear();
        resolved_keys_.clear();
        pending_frame_.clear();
    }

private:
    struct Identity {
        int bus, vendor, product, version;
        std::string unique;
        bool operator==(const Identity&) const = default;
    };

    std::expected<std::map<int, int>, std::string> resolve_keys(const std::set<int>& requested) const {
        std::map<int, int> resolved;
        for (const int code : requested)
            if (code > 0) {
                if (!libevdev_has_event_code(dev_, EV_KEY, code))
                    return std::unexpected(path_ + " lacks configured EV_KEY code " + std::to_string(code));
                resolved.emplace(code, code);
            }
        // Match SDL's joystick ordering. KEY_MAX is skipped by current SDL
        // and Wine; explicit button_code can still select it.
        int index = 0;
        auto enumerate = [&](int first, int last) -> std::expected<void, std::string> {
            for (int code = first; code < last; ++code) {
                if (!libevdev_has_event_code(dev_, EV_KEY, code)) continue;
                ++index;
                if (!requested.contains(button_index_key(index))) continue;
                if (!resolved.emplace(code, button_index_key(index)).second)
                    return std::unexpected(path_ + " button " + std::to_string(index) +
                                           " conflicts with configured button_code " + std::to_string(code));
            }
            return {};
        };
        if (const auto result = enumerate(BTN_JOYSTICK, KEY_MAX); !result)
            return std::unexpected(result.error());
        if (const auto result = enumerate(BTN_MISC, BTN_JOYSTICK); !result)
            return std::unexpected(result.error());
        for (const int code : requested)
            if (code < 0 && !std::any_of(resolved.begin(), resolved.end(),
                [code](const auto& entry) { return entry.second == code; }))
                return std::unexpected(path_ + " has no joystick button " + std::to_string(-code));
        return resolved;
    }

    std::expected<void, std::string> validate_axes(const std::set<int>& axes) const {
        for (const int code : axes) {
            if (!libevdev_has_event_code(dev_, EV_ABS, code))
                return std::unexpected(path_ + " lacks configured EV_ABS code " + std::to_string(code));
            const auto* info = libevdev_get_abs_info(dev_, code);
            if (!info || info->minimum >= info->maximum)
                return std::unexpected(path_ + " has an invalid range for EV_ABS code " + std::to_string(code));
            if (code >= ABS_HAT0X && code <= ABS_HAT3Y &&
                (info->minimum != -1 || info->maximum != 1))
                return std::unexpected(path_ + " requires a ternary range for hat axis " + std::to_string(code));
        }
        return {};
    }

    std::expected<void, std::string> drain_and_sync() {
        input_event event{};
        while (true) {
            const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_NORMAL, &event);
            if (rc == -EAGAIN) break;
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (const auto result = sync(); !result) return result;
            } else if (rc < 0) {
                return std::unexpected(error_text(path_ + " initial read", -rc));
            }
        }
        const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_FORCE_SYNC, &event);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) return sync();
        if (rc < 0 && rc != -EAGAIN)
            return std::unexpected(error_text(path_ + " initial sync", -rc));
        return {};
    }

    std::expected<void, std::string> sync() {
        input_event event{};
        while (true) {
            const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_SYNC, &event);
            if (rc == -EAGAIN) return {};
            if (rc < 0) return std::unexpected(error_text(path_ + " resync", -rc));
        }
    }

    void suppress_held() {
        suppressed_.clear();
        for (const auto& [code, normalized] : resolved_keys_)
            if (libevdev_get_event_value(dev_, EV_KEY, code) == 1)
                suppressed_.insert(code);
    }

    std::string name_, path_;
    bool grab_;
    std::set<int> codes_;
    std::map<int, int> resolved_keys_;
    std::optional<std::map<int, int>> original_keys_;
    std::set<int> axes_;
    std::set<int> suppressed_;
    std::vector<InputEvent> pending_frame_;
    std::optional<Identity> identity_;
    libevdev* dev_ = nullptr;
    int fd_ = -1;
    dev_t node_id_ = 0;
    bool grabbed_ = false;
};

class UinputOutput final : public OutputSink {
public:
    ~UinputOutput() override {
        for (auto& [name, device] : devices_) libevdev_uinput_destroy(device);
    }

    std::expected<void, std::string> create(const Config& config) {
        for (const auto& [name, device] : config.devices) {
            if (device.kind != DeviceKind::Uinput) continue;
            const std::string label = device.virtual_name.empty() ? "JP " + name : device.virtual_name;
            if (label.size() >= UINPUT_MAX_NAME_SIZE)
                return std::unexpected("virtual device name is too long: " + name);
            std::unique_ptr<libevdev, decltype(&libevdev_free)> description(libevdev_new(), libevdev_free);
            if (!description) return std::unexpected("cannot allocate uinput device description");
            libevdev_set_name(description.get(), label.c_str());
            libevdev_set_id_bustype(description.get(),
                                   device.bus == VirtualBus::Usb ? BUS_USB : BUS_VIRTUAL);
            libevdev_set_id_vendor(description.get(), device.vendor_id);
            libevdev_set_id_product(description.get(), device.product_id);
            libevdev_set_id_version(description.get(), 1);
            auto axes = joystick_axes();
            for (const auto& [code, range] : device.axes) axes.insert_or_assign(code, range);
            for (const auto& [code, range] : axes) {
                input_absinfo info{};
                info.value = range.neutral;
                info.minimum = range.minimum;
                info.maximum = range.maximum;
                if (const int rc = libevdev_enable_event_code(description.get(), EV_ABS, code, &info);
                    rc < 0) return std::unexpected(error_text(label + " axis capabilities", -rc));
            }
            std::set<int> buttons;
            for (int index = 1; index <= joystick_button_count; ++index)
                buttons.insert(joystick_button_code(index));
            std::set<int> hats = joystick_hats();
            auto collect = [&](const std::vector<Action>& actions) {
                for (const auto& action : actions) {
                    if (const auto* button = std::get_if<ButtonAction>(&action)) {
                        if (button->device == name) buttons.insert(button->code);
                    } else if (const auto* hat = std::get_if<HatAction>(&action)) {
                        if (hat->device == name) hats.insert(hat->code);
                    }
                }
            };
            for (const auto& binding : config.bindings) {
                collect(binding.actions);
                if (binding.tap_hold) {
                    collect(binding.tap_hold->tap);
                    collect(binding.tap_hold->hold);
                }
            }
            for (const int code : buttons)
                if (const int rc = libevdev_enable_event_code(description.get(), EV_KEY, code, nullptr);
                    rc < 0) return std::unexpected(error_text(label + " button capabilities", -rc));
            for (const int code : hats) {
                input_absinfo info{};
                info.minimum = -1;
                info.maximum = 1;
                if (const int rc = libevdev_enable_event_code(description.get(), EV_ABS, code, &info);
                    rc < 0) return std::unexpected(error_text(label + " hat capabilities", -rc));
            }
            libevdev_uinput* output = nullptr;
            if (const int rc = libevdev_uinput_create_from_device(description.get(),
                                            LIBEVDEV_UINPUT_OPEN_MANAGED, &output); rc < 0)
                return std::unexpected(error_text(label + " /dev/uinput", -rc));
            devices_.emplace(name, output);
            bool initialized = false;
            for (const auto& [code, range] : axes) {
                if (const int rc = libevdev_uinput_write_event(output, EV_ABS, code, range.neutral);
                    rc < 0) return std::unexpected(error_text(label + " initial axis state", -rc));
                initialized = true;
            }
            for (const int code : hats) {
                if (const int rc = libevdev_uinput_write_event(output, EV_ABS, code, 0); rc < 0)
                    return std::unexpected(error_text(label + " initial hat state", -rc));
                initialized = true;
            }
            if (initialized) {
                if (const int rc = libevdev_uinput_write_event(output, EV_SYN, SYN_REPORT, 0); rc < 0)
                    return std::unexpected(error_text(label + " initial SYN_REPORT", -rc));
            }
            std::cerr << "Created " << label;
            if (const char* node = libevdev_uinput_get_devnode(output)) std::cerr << " at " << node;
            std::cerr << '\n';
        }
        return {};
    }

    std::expected<void, std::string> write_frame(const std::vector<OutputEvent>& events) override {
        std::set<std::string> changed;
        for (const auto& event : events) {
            const auto device = devices_.find(event.device);
            if (device == devices_.end()) return std::unexpected("unknown output '" + event.device + "'");
            const unsigned int type = event.kind == OutputEventKind::Button ? EV_KEY : EV_ABS;
            if (const int rc = libevdev_uinput_write_event(device->second, type, event.code, event.value);
                rc < 0) return std::unexpected(error_text(event.device + " output", -rc));
            changed.insert(event.device);
        }
        for (const auto& name : changed) {
            if (const int rc = libevdev_uinput_write_event(devices_.at(name), EV_SYN, SYN_REPORT, 0);
                rc < 0) return std::unexpected(error_text(name + " SYN_REPORT", -rc));
        }
        return {};
    }

private:
    std::map<std::string, libevdev_uinput*> devices_;
};

} // namespace

int run_hardware(const Config& config, const std::string& profile_path, Speaker* speaker) {
    Config active_config = config;
    auto engine = std::make_unique<GestureEngine>(active_config);
    UinputOutput output;
    if (const auto result = output.create(active_config); !result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    OutputFrames frames(output, active_config);
    std::cerr << "Initial mode: " << engine->mode() << '\n';

    auto report_mode_change = [&](const std::string& previous) {
        if (engine->mode() == previous) return;
        std::cerr << "Mode changed: " << previous << " -> " << engine->mode() << '\n';
        if (speaker) speaker->speak(engine->mode());
    };
    auto process = [&](const InputEvent& event) {
        const std::string previous = engine->mode();
        auto events = engine->process(event);
        report_mode_change(previous);
        return events;
    };

    std::map<std::string, std::unique_ptr<EvdevInput>> inputs;
    for (const auto& [name, device] : active_config.devices) {
        if (device.kind != DeviceKind::Evdev) continue;
        if (device.path.starts_with("/dev/input/event"))
            throw ConfigError(name + " must use a stable /dev/input/by-id or by-path link, not eventN");
        auto [codes, axes] = input_requirements(active_config, name);
        inputs.emplace(name, std::make_unique<EvdevInput>(name, device.path, device.grab,
                                                           std::move(codes), std::move(axes)));
    }

    stopping = 0;
    reload_requested = 0;
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);
    if (!profile_path.empty()) std::signal(SIGHUP, reload_signal);
    InteractiveTerminal terminal(!profile_path.empty());
    if (terminal.active()) std::cerr << "Press r to reload the profile\n";
    std::map<std::string, std::string> reported_errors;
    int status = 0;
    bool poll_terminal = terminal.active();
    auto connect_inputs = [&](bool seed_baselines) {
        for (auto& [name, input] : inputs) {
            if (input->connected()) continue;
            auto result = input->connect();
            if (result) {
                for (const auto& [other_name, other] : inputs) {
                    if (other_name != name && other->connected() && other->node_id() == input->node_id()) {
                        result = std::unexpected(name + " and " + other_name + " resolve to the same evdev node");
                        input->close_device();
                        break;
                    }
                }
            }
            if (result) {
                reported_errors.erase(name);
                if (seed_baselines)
                    for (const auto& baseline : input->baselines()) process(baseline);
                std::cerr << "Connected " << name << '\n';
            } else if (reported_errors[name] != result.error()) {
                reported_errors[name] = result.error();
                std::cerr << name << ": " << result.error() << " (retrying)\n";
            }
        }
    };
    auto reload_profile = [&]() -> std::expected<void, std::string> {
        if (profile_path.empty()) return std::unexpected("no profile path was provided");

        // Build the replacement and check everything that can be checked
        // without affecting the running mappings or virtual devices.
        Config replacement;
        std::unique_ptr<GestureEngine> new_engine;
        try {
            replacement = load_config_file(profile_path);
            new_engine = std::make_unique<GestureEngine>(replacement);
        } catch (const ConfigError& error) {
            return std::unexpected(error.what());
        }
        if (const auto compatible = virtual_devices_compatible(active_config, replacement); !compatible)
            return compatible;

        std::map<std::string, std::unique_ptr<EvdevInput>> staged_inputs;
        for (const auto& [name, device] : replacement.devices) {
            if (device.kind != DeviceKind::Evdev) continue;
            if (device.path.starts_with("/dev/input/event"))
                return std::unexpected(name + " must use a stable by-id or by-path link, not eventN");
            auto [keys, axes] = input_requirements(replacement, name);
            const auto existing = inputs.find(name);
            const auto original = active_config.devices.find(name);
            if (existing != inputs.end() && original != active_config.devices.end() &&
                original->second.kind == DeviceKind::Evdev &&
                original->second.path == device.path && original->second.grab == device.grab) {
                if (const auto checked = existing->second->validate_configuration(keys, axes); !checked)
                    return checked;
            } else {
                staged_inputs.emplace(name, std::make_unique<EvdevInput>(
                    name, device.path, device.grab, std::move(keys), std::move(axes)));
            }
        }

        if (const auto written = frames.flush(); !written) {
            stopping = 1;
            status = 1;
            return std::unexpected(written.error());
        }
        const auto previous = engine->mode();
        frames.add(engine->release_all()); // Cancels pending taps without firing them.

        std::map<std::string, std::unique_ptr<EvdevInput>> updated_inputs;
        for (const auto& [name, device] : replacement.devices) {
            if (device.kind != DeviceKind::Evdev) continue;
            if (auto staged = staged_inputs.find(name); staged != staged_inputs.end()) {
                updated_inputs.emplace(name, std::move(staged->second));
            } else {
                auto input = std::move(inputs.at(name));
                auto [keys, axes] = input_requirements(replacement, name);
                if (const auto changed = input->reconfigure(std::move(keys), std::move(axes)); !changed)
                    std::cerr << name << ": " << changed.error() << " (retrying)\n";
                updated_inputs.emplace(name, std::move(input));
            }
        }
        inputs.swap(updated_inputs);
        updated_inputs.clear(); // Release grabs for removed/repointed physical inputs.
        engine.swap(new_engine);
        active_config = std::move(replacement);
        reported_errors.clear();
        connect_inputs(false);

        for (const auto& [name, input] : inputs) {
            if (!input->connected()) continue;
            const auto baselines = input->baselines();
            for (const auto& baseline : baselines) engine->process(baseline);
            for (const auto& baseline : baselines)
                if (baseline.code < ABS_HAT0X || baseline.code > ABS_HAT3Y) {
                    auto current = baseline;
                    current.kind = InputEventKind::AbsoluteAxis;
                    frames.add(engine->process(current));
                }
        }
        report_mode_change(previous);
        if (const auto written = frames.flush(); !written) {
            stopping = 1;
            status = 1;
            return std::unexpected(written.error());
        }
        std::cerr << "Reloaded profile: " << profile_path << " (mode: " << engine->mode() << ")\n";
        return {};
    };

    while (!stopping) {
        connect_inputs(true);

        std::vector<pollfd> fds;
        std::vector<EvdevInput*> ready;
        for (auto& [name, input] : inputs) {
            if (!input->connected()) continue;
            fds.push_back({input->descriptor(), POLLIN, 0});
            ready.push_back(input.get());
        }
        const auto terminal_index = fds.size();
        if (poll_terminal) fds.push_back({STDIN_FILENO, POLLIN, 0});
        int timeout = 500;
        if (const auto deadline = engine->next_deadline()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now()).count();
            timeout = static_cast<int>(std::clamp<long long>(remaining + 1, 0, 500));
        }
        int rc = ::poll(fds.data(), fds.size(), timeout);
        if (rc < 0) {
            if (errno == EINTR) rc = 0;
            else {
                std::cerr << error_text("poll", errno) << '\n';
                status = 1;
                break;
            }
        }
        if (stopping) break;
        const std::string previous_mode = engine->mode();
        frames.add(engine->process_timers());
        report_mode_change(previous_mode);
        if (const auto written = frames.flush(); !written) {
            std::cerr << written.error() << '\n';
            status = 1;
            break;
        }
        for (std::size_t index = 0; index < ready.size(); ++index) {
            if (!fds[index].revents) continue;
            auto& input = *ready[index];
            bool lost = false;
            if (fds[index].revents & POLLIN) {
                const auto result = input.read_events();
                if (result) {
                    for (const auto& event : *result) {
                        if (event.kind == InputEventKind::FrameEnd ||
                            event.kind == InputEventKind::SyncLost) {
                            if (const auto written = frames.flush(); !written) {
                                std::cerr << written.error() << '\n';
                                status = 1;
                                stopping = 1;
                                break;
                            }
                        }
                        if (event.kind == InputEventKind::FrameEnd) continue;
                        frames.add(process(event));
                        if (event.kind == InputEventKind::SyncLost) {
                            if (const auto written = frames.flush(); !written) {
                                std::cerr << written.error() << '\n';
                                status = 1;
                                stopping = 1;
                                break;
                            }
                        }
                    }
                    if (!stopping) {
                        if (const auto written = frames.flush(); !written) {
                            std::cerr << written.error() << '\n';
                            status = 1;
                            stopping = 1;
                        }
                    }
                } else {
                    std::cerr << result.error() << '\n';
                    lost = true;
                }
            }
            if (fds[index].revents & (POLLERR | POLLHUP | POLLNVAL)) lost = true;
            if (lost) {
                frames.add(process({input.name(), InputEventKind::Disconnected}));
                if (const auto written = frames.flush(); !written) {
                    std::cerr << written.error() << '\n';
                    status = 1;
                    stopping = 1;
                }
                input.close_device();
                std::cerr << "Disconnected " << input.name() << " (retrying)\n";
            }
            if (stopping) break;
        }
        if (!stopping && poll_terminal && fds[terminal_index].revents) {
            if (fds[terminal_index].revents & POLLIN) {
                char keys[32];
                const auto count = ::read(STDIN_FILENO, keys, sizeof(keys));
                if (count > 0) {
                    for (ssize_t i = 0; i < count; ++i)
                        if (keys[i] == 'r' || keys[i] == 'R') reload_requested = 1;
                } else if (count == 0 || errno != EINTR) {
                    poll_terminal = false;
                }
            }
            if (fds[terminal_index].revents & (POLLHUP | POLLERR | POLLNVAL)) poll_terminal = false;
        }
        if (!stopping && reload_requested) {
            reload_requested = 0;
            if (const auto result = reload_profile(); !result)
                std::cerr << "Reload failed: " << result.error() << '\n';
        }
    }
    frames.add(engine->release_all());
    if (const auto result = frames.flush(); !result) {
        std::cerr << result.error() << '\n';
        status = 1;
    }
    return status;
}

} // namespace joystick_penguin
