#include "profile_document.hpp"
#include <iostream>
#include <stdexcept>

using namespace joystick_penguin;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void run() {
    ProfileDocument document;
    auto* address = &document.config();
    auto profile = document.config();
    profile.modes.push_back("shift");
    document.replace(profile, "profile.yaml");
    check(&document.config() == address && !document.dirty(), "replacement preserves the config address and saved state");

    Binding binding;
    binding.modes = {"default"};
    check(document.insert(binding), "inserting a binding changes the document");
    check(document.dirty() && document.canUndo(), "insertion is dirty and undoable");
    check(document.editMapping(0, [](Binding& b) { b.modes = {"shift"}; }), "first mapping edit changes the document");
    check(document.editMapping(0, [](Binding& b) { b.modes = {"default", "shift"}; }), "second mapping edit changes the document");
    check(document.undo(), "coalesced mapping edits can be undone");
    check(document.config().bindings[0].modes == std::vector<std::string>{"default"}, "mapping edits coalesce into one undo step");
    check(document.undo(), "insertion can be undone");
    check(document.config().bindings.empty(), "undo removes the inserted binding");
    check(document.redo() && document.redo(), "insertion and mapping edits can be redone");
    document.saved("saved.yaml");
    check(!document.dirty() && document.path() == "saved.yaml", "saving updates the path and clean state");
    check(document.editMapping(0, [](Binding& b) { b.modes = {"shift"}; }), "post-save edit changes the document");
    check(document.undo() && !document.dirty(), "undoing a post-save edit restores the clean state");

    // Navigating away and back starts another undoable edit session.
    document.breakMappingSession();
    check(document.editMapping(0, [](Binding& b) { b.modes = {"shift"}; }), "new mapping session records its first edit");
    document.breakMappingSession();
    check(document.editMapping(0, [](Binding& b) { b.modes = {"default"}; }), "new mapping session records its second edit");
    check(document.undo(), "second mapping session can be undone");
    check(document.config().bindings[0].modes == std::vector<std::string>{"shift"}, "session boundary preserves the earlier edit");

    int begins = 0, ends = 0;
    document.observe([&](ProfileDocument::Change change, int row, bool before) {
        check(change == ProfileDocument::Change::Remove && row == 0, "removal notifies the correct row");
        if (before) { ++begins; check(document.config().bindings.size() == 1, "removal begins before mutation"); }
        else { ++ends; check(document.config().bindings.empty(), "removal ends after mutation"); }
    });
    check(document.erase(0), "removal changes the document");
    check(begins == 1 && ends == 1, "removal sends both notifications");
}
} // namespace

int main() {
    try {
        run();
        std::cout << "Profile document tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Profile document test failed: " << error.what() << '\n';
        return 1;
    }
}
