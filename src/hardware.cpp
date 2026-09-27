#include "joystick_penguin/hardware.hpp"

#include "joystick_penguin/gesture_engine.hpp"
#include "joystick_penguin/io.hpp"
#include "joystick_penguin/output_frames.hpp"

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/stat.h>
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

void stop_signal(int) { stopping = 1; }

std::string error_text(const std::string& context, int error) {
    return context + ": " + std::strerror(error);
}

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
        for (const int code : codes_)
            if (!libevdev_has_event_code(dev_, EV_KEY, code))
                return fail(path_ + " lacks configured EV_KEY code " + std::to_string(code));
        for (const int code : axes_) {
            if (!libevdev_has_event_code(dev_, EV_ABS, code))
                return fail(path_ + " lacks configured EV_ABS code " + std::to_string(code));
            const auto* info = libevdev_get_abs_info(dev_, code);
            if (!info || info->minimum >= info->maximum)
                return fail(path_ + " has an invalid range for EV_ABS code " + std::to_string(code));
            if (code >= ABS_HAT0X && code <= ABS_HAT3Y &&
                (info->minimum != -1 || info->maximum != 1))
                return fail(path_ + " requires a ternary range for hat axis " + std::to_string(code));
        }
        if (grab_) {
            if (const int rc = libevdev_grab(dev_, LIBEVDEV_GRAB); rc < 0)
                return fail(error_text(path_ + " EVIOCGRAB failed", -rc));
            grabbed_ = true;
        }

        // libevdev's initial snapshot can already include queued events. Drain
        // them without forwarding, then resynchronize and suppress held keys.
        input_event event{};
        while (true) {
            const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_NORMAL, &event);
            if (rc == -EAGAIN) break;
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (const auto result = sync(); !result) return fail(result.error());
            } else if (rc < 0) {
                return fail(error_text(path_ + " initial read", -rc));
            }
        }
        const int rc = libevdev_next_event(dev_, LIBEVDEV_READ_FLAG_FORCE_SYNC, &event);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            if (const auto result = sync(); !result) return fail(result.error());
        } else if (rc < 0 && rc != -EAGAIN) {
            return fail(error_text(path_ + " initial sync", -rc));
        }
        suppress_held();
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
            } else if (event.type == EV_KEY && codes_.contains(event.code)) {
                if (event.value == 0) {
                    suppressed_.erase(event.code);
                    pending_frame_.push_back({name_, InputEventKind::Button, event.code, 0});
                } else if (!suppressed_.contains(event.code)) {
                    pending_frame_.push_back({name_, InputEventKind::Button, event.code, event.value});
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
        pending_frame_.clear();
    }

private:
    struct Identity {
        int bus, vendor, product, version;
        std::string unique;
        bool operator==(const Identity&) const = default;
    };

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
        for (const int code : codes_)
            if (libevdev_get_event_value(dev_, EV_KEY, code) == 1)
                suppressed_.insert(code);
    }

    std::string name_, path_;
    bool grab_;
    std::set<int> codes_;
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
            const std::string label = "JP " + name;
            if (label.size() >= UINPUT_MAX_NAME_SIZE)
                return std::unexpected("virtual device name is too long: " + name);
            std::unique_ptr<libevdev, decltype(&libevdev_free)> description(libevdev_new(), libevdev_free);
            if (!description) return std::unexpected("cannot allocate uinput device description");
            libevdev_set_name(description.get(), label.c_str());
            libevdev_set_id_bustype(description.get(), BUS_VIRTUAL);
            libevdev_set_id_vendor(description.get(), 0x1);
            libevdev_set_id_product(description.get(), 0x1);
            libevdev_set_id_version(description.get(), 1);
            if (const int rc = libevdev_enable_event_code(description.get(), EV_KEY, BTN_JOYSTICK, nullptr);
                rc < 0) return std::unexpected(error_text(label + " capabilities", -rc));
            for (const auto& [code, range] : device.axes) {
                input_absinfo info{};
                info.value = range.neutral;
                info.minimum = range.minimum;
                info.maximum = range.maximum;
                if (const int rc = libevdev_enable_event_code(description.get(), EV_ABS, code, &info);
                    rc < 0) return std::unexpected(error_text(label + " axis capabilities", -rc));
            }
            std::set<int> hats;
            auto advertise = [&](const std::vector<Action>& actions) -> std::expected<void, std::string> {
                for (const auto& action : actions) {
                    if (const auto* button = std::get_if<ButtonAction>(&action)) {
                        if (button->device != name) continue;
                        if (const int rc = libevdev_enable_event_code(description.get(), EV_KEY,
                                                                       button->code, nullptr); rc < 0)
                            return std::unexpected(error_text(label + " capabilities", -rc));
                    } else if (const auto* hat = std::get_if<HatAction>(&action)) {
                        if (hat->device != name || !hats.insert(hat->code).second) continue;
                        input_absinfo info{};
                        info.minimum = -1;
                        info.maximum = 1;
                        if (const int rc = libevdev_enable_event_code(description.get(), EV_ABS,
                                                                       hat->code, &info); rc < 0)
                            return std::unexpected(error_text(label + " hat capabilities", -rc));
                    }
                }
                return {};
            };
            for (const auto& binding : config.bindings) {
                if (const auto result = advertise(binding.actions); !result) return result;
                if (binding.tap_hold) {
                    if (const auto result = advertise(binding.tap_hold->tap); !result) return result;
                    if (const auto result = advertise(binding.tap_hold->hold); !result) return result;
                }
            }
            libevdev_uinput* output = nullptr;
            if (const int rc = libevdev_uinput_create_from_device(description.get(),
                                            LIBEVDEV_UINPUT_OPEN_MANAGED, &output); rc < 0)
                return std::unexpected(error_text(label + " /dev/uinput", -rc));
            devices_.emplace(name, output);
            bool initialized = false;
            for (const auto& [code, range] : device.axes) {
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

int run_hardware(const Config& config) {
    GestureEngine engine(config);
    UinputOutput output;
    if (const auto result = output.create(config); !result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    OutputFrames frames(output, config);
    std::cerr << "Initial mode: " << engine.mode() << '\n';

    auto report_mode_change = [&](const std::string& previous) {
        if (engine.mode() != previous)
            std::cerr << "Mode changed: " << previous << " -> " << engine.mode() << '\n';
    };
    auto process = [&](const InputEvent& event) {
        const std::string previous = engine.mode();
        auto events = engine.process(event);
        report_mode_change(previous);
        return events;
    };

    std::map<std::string, std::unique_ptr<EvdevInput>> inputs;
    for (const auto& [name, device] : config.devices) {
        if (device.kind != DeviceKind::Evdev) continue;
        if (device.path.starts_with("/dev/input/event"))
            throw ConfigError(name + " must use a stable /dev/input/by-id or by-path link, not eventN");
        std::set<int> codes, axes;
        for (const auto& binding : config.bindings)
            if (binding.input.device == name) {
                if (binding.input.kind == ControlKind::Button) codes.insert(binding.input.code);
                else axes.insert(binding.input.code);
            }
        for (const auto& entry : config.modifiers)
            if (entry.second.device == name) codes.insert(entry.second.code);
        inputs.emplace(name, std::make_unique<EvdevInput>(name, device.path, device.grab,
                                                           std::move(codes), std::move(axes)));
    }

    stopping = 0;
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);
    std::map<std::string, std::string> reported_errors;
    int status = 0;
    while (!stopping) {
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
                for (const auto& baseline : input->baselines()) process(baseline);
                std::cerr << "Connected " << name << '\n';
            } else if (reported_errors[name] != result.error()) {
                reported_errors[name] = result.error();
                std::cerr << name << ": " << result.error() << " (retrying)\n";
            }
        }

        std::vector<pollfd> fds;
        std::vector<EvdevInput*> ready;
        for (auto& [name, input] : inputs) {
            if (!input->connected()) continue;
            fds.push_back({input->descriptor(), POLLIN, 0});
            ready.push_back(input.get());
        }
        int timeout = 500;
        if (const auto deadline = engine.next_deadline()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now()).count();
            timeout = static_cast<int>(std::clamp<long long>(remaining + 1, 0, 500));
        }
        const int rc = ::poll(fds.data(), fds.size(), timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::cerr << error_text("poll", errno) << '\n';
            status = 1;
            break;
        }
        const std::string previous_mode = engine.mode();
        frames.add(engine.process_timers());
        report_mode_change(previous_mode);
        if (const auto written = frames.flush(); !written) {
            std::cerr << written.error() << '\n';
            status = 1;
            break;
        }
        for (std::size_t index = 0; index < fds.size(); ++index) {
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
                        if (engine.has_tap_release()) {
                            if (const auto written = frames.flush(); !written) {
                                std::cerr << written.error() << '\n';
                                status = 1;
                                stopping = 1;
                                break;
                            }
                            frames.add(engine.finish_tap());
                            if (const auto written = frames.flush(); !written) {
                                std::cerr << written.error() << '\n';
                                status = 1;
                                stopping = 1;
                                break;
                            }
                        }
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
    }
    frames.add(engine.release_all());
    if (const auto result = frames.flush(); !result) {
        std::cerr << result.error() << '\n';
        status = 1;
    }
    return status;
}

} // namespace joystick_penguin
