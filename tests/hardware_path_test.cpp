#include "joystick_penguin/hardware.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace joystick_penguin;

namespace {

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct DeviceAccessDenied : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Device = std::unique_ptr<libevdev_uinput, decltype(&libevdev_uinput_destroy)>;

Device synthetic(const char* name) {
    std::unique_ptr<libevdev, decltype(&libevdev_free)> description(libevdev_new(), libevdev_free);
    check(bool(description), "allocating synthetic input");
    libevdev_set_name(description.get(), name);
    libevdev_set_id_bustype(description.get(), BUS_USB);
    libevdev_set_id_vendor(description.get(), 0x1234);
    libevdev_set_id_product(description.get(), 0x5678);
    check(libevdev_enable_event_code(description.get(), EV_KEY, BTN_TRIGGER, nullptr) == 0,
          "enabling synthetic button");
    check(libevdev_enable_event_code(description.get(), EV_KEY, BTN_TRIGGER_HAPPY1, nullptr) == 0,
          "enabling non-contiguous synthetic button");
    libevdev_uinput* raw = nullptr;
    const int rc = libevdev_uinput_create_from_device(description.get(),
                                                       LIBEVDEV_UINPUT_OPEN_MANAGED, &raw);
    check(rc == 0, std::string("creating synthetic input: ") + std::strerror(-rc));
    return Device(raw, libevdev_uinput_destroy);
}

std::string node(Device& device) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (const char* path = libevdev_uinput_get_devnode(device.get())) return path;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("synthetic input has no event node");
}

void button(Device& device, int value) {
    check(libevdev_uinput_write_event(device.get(), EV_KEY, BTN_TRIGGER, value) == 0,
          "writing synthetic button");
    check(libevdev_uinput_write_event(device.get(), EV_SYN, SYN_REPORT, 0) == 0,
          "writing synthetic frame");
}

void extra_button(Device& device, int value) {
    check(libevdev_uinput_write_event(device.get(), EV_KEY, BTN_TRIGGER_HAPPY1, value) == 0,
          "writing synthetic non-contiguous button");
    check(libevdev_uinput_write_event(device.get(), EV_SYN, SYN_REPORT, 0) == 0,
          "writing synthetic frame");
}

int open_reader(const std::string& path) {
    int last_error = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) return fd;
        last_error = errno;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto message = "opening " + path + ": " + std::strerror(last_error);
    if (last_error == EACCES || last_error == EPERM) throw DeviceAccessDenied(message);
    throw std::runtime_error(message);
}

struct Session {
    std::string dir, a, b;
    pid_t child = -1;
    int log_fd = -1;
    std::string logs;

    ~Session() {
        if (child > 0) {
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
        }
        if (log_fd >= 0) close(log_fd);
        if (!a.empty()) unlink(a.c_str());
        if (!b.empty()) unlink(b.c_str());
        if (!dir.empty()) rmdir(dir.c_str());
    }

    void read_logs() {
        char buffer[2048];
        while (true) {
            const ssize_t count = read(log_fd, buffer, sizeof(buffer));
            if (count > 0) logs.append(buffer, count);
            else break;
        }
    }

    void wait_for(const std::string& text, int occurrences = 1) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{log_fd, POLLIN, 0};
            poll(&pfd, 1, 50);
            read_logs();
            std::size_t count = 0, pos = 0;
            while ((pos = logs.find(text, pos)) != std::string::npos) {
                ++count;
                pos += text.size();
            }
            if (count >= static_cast<std::size_t>(occurrences)) return;
        }
        throw std::runtime_error("waiting for '" + text + "': " + logs);
    }
};

bool next_button(int fd, int value, int milliseconds, int code = BTN_TRIGGER) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(remaining)) <= 0) continue;
        input_event event{};
        const auto count = read(fd, &event, sizeof(event));
        if (count == sizeof(event) && event.type == EV_KEY &&
            event.code == code && event.value == value) return true;
        if (count < 0 && errno != EAGAIN) throw std::runtime_error("reading virtual output");
    }
    return false;
}

void run() {
    Device input_a = synthetic("Joystick Penguin test input A");
    Device input_b = synthetic("Joystick Penguin test input B");
    char directory[] = "/tmp/joystick-penguin-test-XXXXXX";
    Session session;
    const char* created = mkdtemp(directory);
    check(created != nullptr, "creating test directory");
    session.dir = created;
    session.a = session.dir + "/a-event-joystick";
    session.b = session.dir + "/b-event-joystick";
    check(symlink(node(input_a).c_str(), session.a.c_str()) == 0, "linking first input");
    check(symlink(node(input_b).c_str(), session.b.c_str()) == 0, "linking second input");

    const int observer_a = open_reader(session.a);
    const int observer_b = open_reader(session.b);

    Config config;
    config.initial_mode = "default";
    config.modes = {"default"};
    config.devices.emplace("a", joystick_penguin::Device{DeviceKind::Evdev, session.a, true, ""});
    config.devices.emplace("b", joystick_penguin::Device{DeviceKind::Evdev, session.b, false, ""});
    config.devices.emplace("virtual", joystick_penguin::Device{DeviceKind::Uinput, "", true, "joystick"});
    config.bindings = {
        {{"a", ControlKind::Button, -1}, {"default"}, {}, {ButtonAction{"virtual", BTN_TRIGGER}}},
        {{"a", ControlKind::Button, button_index_key(2)}, {"default"}, {},
         {ButtonAction{"virtual", joystick_button_code(79)}}},
        {{"b", ControlKind::Button, BTN_TRIGGER}, {"default"}, {}, {ButtonAction{"virtual", BTN_TRIGGER}}},
    };

    int pipefd[2];
    check(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0, "creating log pipe");
    session.child = fork();
    check(session.child >= 0, "forking remapper");
    if (session.child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        // The child must not keep the parent's synthetic uinput devices alive.
        close(libevdev_uinput_get_fd(input_a.get()));
        close(libevdev_uinput_get_fd(input_b.get()));
        _exit(run_hardware(config));
    }
    close(pipefd[1]);
    session.log_fd = pipefd[0];
    session.wait_for("Connected a");
    session.wait_for("Connected b");
    const std::string prefix = "Created JP virtual at ";
    session.wait_for(prefix);
    const auto start = session.logs.find(prefix) + prefix.size();
    auto end = session.logs.find('\n', start);
    for (int attempt = 0; end == std::string::npos && attempt < 100; ++attempt) {
        pollfd log{session.log_fd, POLLIN, 0};
        poll(&log, 1, 20);
        session.read_logs();
        end = session.logs.find('\n', start);
    }
    check(end != std::string::npos, "virtual output node in log");
    const auto output_path = session.logs.substr(start, end - start);
    const int output_fd = open_reader(output_path);

    unsigned long buttons[(KEY_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)]{};
    check(ioctl(output_fd, EVIOCGBIT(EV_KEY, sizeof(buttons)), buttons) >= 0 &&
          (buttons[766 / (sizeof(unsigned long) * 8)] &
           (1UL << (766 % (sizeof(unsigned long) * 8)))) != 0 &&
          (buttons[767 / (sizeof(unsigned long) * 8)] &
           (1UL << (767 % (sizeof(unsigned long) * 8)))) == 0,
          "joystick preset advertises button 79 but not Wine-invisible KEY_MAX");

    button(input_a, 1);
    check(next_button(output_fd, 1, 2000), "a presses the virtual button");
    extra_button(input_a, 1);
    check(next_button(output_fd, 1, 2000, joystick_button_code(79)),
          "second indexed physical button resolves across EV_KEY gap to button 79");
    extra_button(input_a, 0);
    check(next_button(output_fd, 0, 2000, joystick_button_code(79)), "indexed button releases");
    check(!next_button(observer_a, 1, 150), "grab suppresses other evdev readers");
    button(input_b, 1);
    check(next_button(observer_b, 1, 1000), "grab:false permits shared evdev input");
    check(!next_button(output_fd, 1, 150), "overlapping button ownership");

    input_a.reset();
    session.wait_for("Disconnected a");
    check(!next_button(output_fd, 0, 150), "other controller keeps output held");
    button(input_b, 0);
    check(next_button(output_fd, 0, 2000), "last owner releases output");

    Device replacement = synthetic("Joystick Penguin test input A");
    button(replacement, 1); // Held before reconnect: never replay this press.
    unlink(session.a.c_str());
    check(symlink(node(replacement).c_str(), session.a.c_str()) == 0, "relinking input");
    session.wait_for("Connected a", 2);
    check(!next_button(output_fd, 1, 150), "reconnect does not replay held button");
    button(replacement, 0);
    button(replacement, 1);
    check(next_button(output_fd, 1, 2000), "new press after reconnect works");

    kill(session.child, SIGINT);
    int status = 0;
    check(waitpid(session.child, &status, 0) == session.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "orderly shutdown");
    session.child = -1;
    close(output_fd);
    close(observer_a);
    close(observer_b);
}

} // namespace

int main() {
    if (access("/dev/uinput", W_OK) != 0) {
        std::cout << "hardware integration skipped: no /dev/uinput access\n";
        return 77;
    }
    try {
        run();
        std::cout << "hardware integration passed\n";
        return 0;
    } catch (const DeviceAccessDenied& error) {
        std::cout << "hardware integration skipped: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "hardware integration failed: " << error.what() << '\n';
        return 1;
    }
}
