#include "joystick_penguin/output_frames.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace joystick_penguin;

namespace {

struct RecordingSink final : OutputSink {
    std::vector<std::vector<OutputEvent>> frames;
    bool fail_next = false;

    std::expected<void, std::string> write_frame(const std::vector<OutputEvent>& events) override {
        if (fail_next) {
            fail_next = false;
            return std::unexpected("test write failure");
        }
        frames.push_back(events);
        return {};
    }
};

void check(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error(reason);
}

void run() {
    Config config;
    config.devices.emplace("virtual", Device{DeviceKind::Uinput, "", true, "joystick",
                                             {{0, {-100, 100, 0}}, {2, {100, 200, 100}}}});
    config.bindings.push_back({{"physical", ControlKind::HatDirection, 16, -1}, {"default"}, {},
                               {HatAction{"virtual", 16, -1}}});
    config.bindings.push_back({{"physical", ControlKind::HatDirection, 17, 1}, {"default"}, {},
                               {HatAction{"virtual", 17, 1}}});
    RecordingSink sink;
    OutputFrames frames(sink, config);

    frames.add({{"virtual", OutputEventKind::AbsoluteAxis, 0, 50},
                {"virtual", OutputEventKind::AbsoluteAxis, 16, -1}});
    frames.add({{"virtual", OutputEventKind::AbsoluteAxis, 0, 0},
                {"virtual", OutputEventKind::AbsoluteAxis, 17, 1}});
    check(bool(frames.flush()), "first frame writes");
    check(sink.frames.size() == 1 && sink.frames[0].size() == 2 &&
          sink.frames[0][0].code == 16 && sink.frames[0][0].value == -1 &&
          sink.frames[0][1].code == 17 && sink.frames[0][1].value == 1,
          "diagonal outputs share a frame; axis movement that returned to neutral is omitted");

    frames.add({{"virtual", OutputEventKind::AbsoluteAxis, 16, 0},
                {"virtual", OutputEventKind::AbsoluteAxis, 16, 1},
                {"virtual", OutputEventKind::AbsoluteAxis, 2, 100}});
    check(bool(frames.flush()), "reversal frame writes");
    check(sink.frames.size() == 2 && sink.frames[1].size() == 1 &&
          sink.frames[1][0].code == 16 && sink.frames[1][0].value == 1,
          "direct hat reversal has no spurious neutral frame; configured neutral is honored");

    frames.add({{"virtual", OutputEventKind::AbsoluteAxis, 2, 150}});
    sink.fail_next = true;
    check(!frames.flush() && sink.frames.size() == 2,
          "write error is returned and the pending frame is retained");
    check(bool(frames.flush()) && sink.frames.size() == 3 &&
          sink.frames[2][0].code == 2 && sink.frames[2][0].value == 150,
          "retry writes the unchanged pending frame");
    frames.add({{"virtual", OutputEventKind::AbsoluteAxis, 2, 100}});
    check(bool(frames.flush()) && sink.frames.size() == 4 &&
          sink.frames[3][0].value == 100, "last owner returns to a nonzero neutral");
    check(bool(frames.flush()) && sink.frames.size() == 4, "empty frame emits no SYN_REPORT");
}

} // namespace

int main() {
    try {
        run();
        std::cout << "output frames tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
