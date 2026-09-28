#include "document.hpp"

#include <iostream>
#include <stdexcept>

using namespace joystick_penguin;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void run() {
    Config original;
    original.modes = {"default"};
    original.initial_mode = "default";
    ProfileHistory history;
    history.reset(original);
    check(!history.canUndo() && !history.canRedo(), "new profile has no history");

    Config first = original;
    first.modes.push_back("alternate");
    history.commitCoalesced(first, 0);
    Config second = first;
    second.modes.push_back("flight");
    history.commitCoalesced(second, 0);
    check(history.undo() == original, "successive edits to one mapping undo together");
    check(history.redo() == second, "redo restores the final coalesced edit");

    Config otherMapping = second;
    otherMapping.initial_mode = "alternate";
    history.commitCoalesced(otherMapping, 1);
    check(history.undo() == second, "editing a different mapping starts an undo step");
    check(history.redo() == otherMapping, "redo restores a different mapping edit");

    Config setup = otherMapping;
    setup.devices.emplace("controller", Device{DeviceKind::Evdev, "/dev/input/by-id/controller"});
    history.commit(setup);
    check(history.undo() == otherMapping, "setup transaction has its own undo step");
    check(history.undo() == second, "previous mapping edit remains undoable");
    check(history.redo() == otherMapping && history.redo() == setup,
          "redo restores mapping and setup transactions in order");

    history.undo();
    history.breakCoalescing();
    Config replacement = otherMapping;
    replacement.modes.push_back("landing");
    history.commitCoalesced(replacement, 1);
    check(!history.canRedo(), "editing after undo drops the old redo branch");
    check(history.undo() == otherMapping, "new edit after undo remains reversible");

    history.reset(original);
    check(!history.canUndo() && !history.canRedo() && history.undo() == original,
          "opening another profile clears the previous history");
}
} // namespace

int main() {
    try {
        run();
        std::cout << "Profile history tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Profile history test failed: " << error.what() << '\n';
        return 1;
    }
}
