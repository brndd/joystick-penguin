#include "joystick_penguin/config.hpp"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace joystick_penguin;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path);
    check(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename F>
void rejected(F&& operation, const std::string& expected) {
    try {
        operation();
    } catch (const ConfigError& error) {
        check(std::string(error.what()).find(expected) != std::string::npos,
              "unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("accepted invalid edit/write: " + expected);
}

struct TestDirectory {
    std::filesystem::path path;
    TestDirectory() {
        const auto pattern = (std::filesystem::temp_directory_path() / "jp-save-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        auto* result = mkdtemp(buffer.data());
        check(result != nullptr, "cannot create test directory");
        path = result;
    }
    ~TestDirectory() { std::filesystem::remove_all(path); }
};

void run() {
    TestDirectory temporary;
    for (const auto* example : {"basic.yaml", "gestures.yaml", "controls.yaml",
                                "modes.yaml", "hardware.yaml", "star_citizen.yaml"}) {
        const auto original_path = std::filesystem::path(EXAMPLES_DIR) / example;
        const auto original = load_config_file(original_path.string());
        const auto serialized = serialize_config(original);
        check(load_config(serialized) == original, std::string(example) + " semantic round trip");
        const auto output = temporary.path / example;
        save_config_file(original, output.string());
        check(load_config_file(output.string()) == original, std::string(example) + " disk round trip");
        if (std::string(example) == "star_citizen.yaml") {
            check(contents(original_path).find('&') != std::string::npos,
                  "anchor fixture should contain anchors");
            check(serialized.find('&') == std::string::npos && serialized.find('*') == std::string::npos,
                  "serialized profile should expand aliases");
        }
    }

    const auto path = temporary.path / "basic.yaml";
    const auto baseline = contents(path);
    auto edited = load_config_file(path.string());
    edited.bindings[0].modes = {"missing"};
    rejected([&] { validate_edited_config(edited); }, "bindings[0] references unknown mode");
    rejected([&] { save_config_file(edited, path.string()); }, "bindings[0] references unknown mode");
    check(contents(path) == baseline, "invalid edit replaced original bytes");

    edited = load_config_file(path.string());
    edited.devices.begin()->second.path = "";
    rejected([&] { serialize_config(edited); }, ".path must be a nonempty string");
    edited = load_config_file(path.string());
    edited.bindings[0].input.direction = 1;
    rejected([&] { validate_edited_config(edited); }, "cannot be represented");
    edited.bindings[0].input.code = std::numeric_limits<int>::min();
    rejected([&] { validate_edited_config(edited); }, "button index is out of permitted range");

    edited = load_config_file(path.string());
    edited.bindings[0].input.code = -2;
    struct stat before{};
    check(chmod(path.c_str(), 0600) == 0 && stat(path.c_str(), &before) == 0,
          "prepare existing permissions");
    save_config_file(edited, path.string());
    struct stat after{};
    check(stat(path.c_str(), &after) == 0 && (after.st_mode & 07777) == (before.st_mode & 07777),
          "save preserves permissions");
    check(load_config_file(path.string()) == edited, "valid edit is saved");

    const auto saved = contents(path);
    const auto invalid_path = temporary.path / "missing" / "basic.yaml";
    rejected([&] { save_config_file(edited, invalid_path.string()); }, "create temporary file");
    check(contents(path) == saved, "failed write left existing profile usable");
    const auto count = std::distance(std::filesystem::directory_iterator(temporary.path),
                                     std::filesystem::directory_iterator{});
    check(count == 6, "no temporary files left after saving or failure");
}

} // namespace

int main() {
    try {
        run();
        std::cout << "profile save tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}
