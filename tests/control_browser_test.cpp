#include "control_browser.hpp"
#include "button_selector.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace joystick_penguin;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void wait(int ms = 100) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
QTreeWidgetItem* item(ControlBrowser& browser, const QString& text) {
    auto* tree = browser.findChild<QTreeWidget*>("controlTree");
    auto matches = tree->findItems(text, (text == "X" ? Qt::MatchExactly : Qt::MatchContains) | Qt::MatchRecursive);
    check(!matches.empty(), "control is listed before input is observed");
    return matches.front();
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        std::unique_ptr<libevdev, decltype(&libevdev_free)> device(libevdev_new(), libevdev_free);
        libevdev_set_name(device.get(), "JP GUI monitor test");
        libevdev_set_id_bustype(device.get(), BUS_USB);
        libevdev_set_id_vendor(device.get(), 1);
        libevdev_set_id_product(device.get(), 1);
        libevdev_enable_event_code(device.get(), EV_KEY, BTN_TRIGGER, nullptr);
        input_absinfo range{}; range.minimum = -32768; range.maximum = 32767;
        libevdev_enable_event_code(device.get(), EV_ABS, ABS_X, &range);
        range.minimum = -1; range.maximum = 1;
        libevdev_enable_event_code(device.get(), EV_ABS, ABS_HAT0X, &range);
        libevdev_uinput* raw = nullptr;
        int rc = libevdev_uinput_create_from_device(device.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &raw);
        if (rc < 0) { std::cout << "uinput unavailable; monitor hardware test skipped\n"; return 77; }
        std::unique_ptr<libevdev_uinput, decltype(&libevdev_uinput_destroy)> source(raw, libevdev_uinput_destroy);
        const char* node = libevdev_uinput_get_devnode(raw);
        check(node != nullptr, "synthetic device node exists");
        int reader = -1;
        for (int tries = 0; tries < 50 && reader < 0; ++tries) { reader = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (reader < 0) wait(20); }
        if (reader < 0) { std::cout << "evdev unreadable; monitor hardware test skipped\n"; return 77; }
        Config config;
        config.devices.emplace("stick", Device{DeviceKind::Evdev, node, true, ""});
        auto virtualDevice = Device{DeviceKind::Uinput, "", true, "joystick"};
        virtualDevice.virtual_name = "JP GUI monitor test";
        config.devices.emplace("vjoy", virtualDevice);
        config.modifiers.emplace("shift", std::vector<Control>{{"stick", ControlKind::Button, -1}});
        config.input_labels.push_back({{"stick", ControlKind::Button, -1}, "Trigger"});
        ButtonSelector selector(config, ButtonSelector::Target::Physical);
        selector.setDevice("stick", 1);
        check(selector.count() == 1 && selector.currentText() == "Trigger (Button 1)",
              "online selector lists only observed buttons with their physical label");
        selector.setNumber(3, true);
        check(selector.number() == 3 && selector.currentText().contains("unavailable"),
              "existing unavailable indexed button stays visible");
        selector.setDevice("stick", 3);
        check(selector.count() == 1 && selector.number() == 1,
              "changing controller drops an unavailable choice");
        ControlBrowser browser(config);
        browser.show(); wait();
        auto* tree = browser.findChild<QTreeWidget*>("controlTree");
        check(tree->topLevelItemCount() == 2 && tree->topLevelItem(1)->text(0).contains("Virtual"),
              "physical and virtual controllers appear together");
        auto* output = tree->topLevelItem(1)->child(0)->child(0);
        check(output->text(0) == "Button 1" && output->text(2).contains("Released"),
              "matching running virtual controller reports live output activity");
        check(item(browser, "Trigger")->text(1).contains("Modifier"), "modifier-only control is identified");
        check(item(browser, "Trigger")->text(2).contains("Released"), "idle button has passive indicator");
        auto send = [&](int type, int code, int value) {
            check(libevdev_uinput_write_event(raw, type, code, value) == 0, "emit input");
            check(libevdev_uinput_write_event(raw, EV_SYN, SYN_REPORT, 0) == 0, "emit frame"); wait();
        };
        send(EV_KEY, BTN_TRIGGER, 1);
        check(item(browser, "Trigger")->text(2).contains("Held"), "button stays lit while held");
        check(output->text(2).contains("Held"), "virtual output button stays lit while held");
        wait(300);
        check(item(browser, "Trigger")->text(2).contains("Held"), "held indicator does not time out");
        send(EV_KEY, BTN_TRIGGER, 0);
        check(item(browser, "Trigger")->text(2).contains("Released"), "button release clears indicator");
        check(output->text(2).contains("Released"), "virtual output release clears indicator");
        send(EV_ABS, ABS_X, 12000);
        check(item(browser, "X")->text(2).contains("12000"), "axis displays current value");
        wait(250);
        check(item(browser, "X")->text(2).startsWith("○"), "axis motion highlight expires");
        send(EV_ABS, ABS_X, 12001);
        check(item(browser, "X")->text(2).startsWith("○"), "minor axis noise does not perpetually illuminate control");
        send(EV_ABS, ABS_HAT0X, -1);
        check(item(browser, "Negative")->text(2).startsWith("●") && item(browser, "Positive")->text(2).startsWith("○"), "hat component sign is shown");
        check(ioctl(reader, EVIOCGRAB, 1) == 0, "GUI monitoring did not take exclusive grab");
        send(EV_KEY, BTN_TRIGGER, 1);
        check(item(browser, "Trigger")->text(2).contains("Released"), "exclusive grab suppresses events to GUI");
        auto status = browser.findChild<QLabel*>("monitorStatus");
        check(status->toolTip().contains("may be hidden while another app"), "monitor describes activity limits plainly");
        ioctl(reader, EVIOCGRAB, 0); close(reader);
        source.reset(); wait();
        check(browser.findChild<QLabel*>("monitorStatus")->text().contains("Disconnected"), "unplug is reported without blocking");
        check(item(browser, "Trigger")->text(2) == "○" && item(browser, "Trigger")->toolTip(2) == "Offline", "unplug clears held activity");
        check(output->text(2) == "○", "unplug clears virtual activity");
        std::cout << "control browser activity tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
