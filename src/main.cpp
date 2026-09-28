#include "joystick_penguin/config.hpp"
#include "joystick_penguin/hardware.hpp"
#include "joystick_penguin/speech.hpp"

#include <CLI/CLI.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char* argv[]) {
    joystick_penguin::SpeechParameters speech;
    bool check_only = false;
    bool no_tts = false;
    std::string profile;

    CLI::App app{"Linux joystick remapper"};
    app.add_flag("--check", check_only, "Validate the profile and exit without opening devices");
    app.add_flag("--no-tts", no_tts, "Disable spoken mode announcements");
    app.add_option("-v,--voice", speech.voice, "espeak-ng voice name (default English)");
    app.add_option("-s,--speed", speech.speed, "Speaking speed in words per minute")
        ->check(CLI::Range(80, 450));
    app.add_option("-p,--pitch", speech.pitch, "Base pitch, 0-100")->check(CLI::Range(0, 100));
    app.add_option("-r,--range", speech.range, "Pitch range, 0-100")->check(CLI::Range(0, 100));
    app.add_option("profile", profile, "Versioned YAML profile")->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        app.exit(error);
        return error.get_exit_code() == 0 ? 0 : 2;
    }

#if !defined(JOYSTICK_PENGUIN_ENABLE_TTS)
    if (app.count("--voice") + app.count("--speed") + app.count("--pitch") + app.count("--range") > 0) {
        std::cerr << "This build has no espeak-ng support; reconfigure with -DJP_ENABLE_TTS=ON\n"
                  << app.help();
        return 2;
    }
#endif

    try {
        const auto config = joystick_penguin::load_config_file(profile);
        if (check_only) {
            std::cout << "Valid profile: " << config.devices.size() << " devices, "
                      << config.bindings.size() << " bindings; initial mode: "
                      << config.initial_mode << '\n';
            return 0;
        }
#if defined(JOYSTICK_PENGUIN_ENABLE_TTS)
        std::unique_ptr<joystick_penguin::EspeakSpeaker> speaker;
        if (!no_tts) speaker = std::make_unique<joystick_penguin::EspeakSpeaker>(speech);
        return joystick_penguin::run_hardware(config, profile, speaker.get());
#else
        return joystick_penguin::run_hardware(config, profile);
#endif
    } catch (const joystick_penguin::ConfigError& error) {
        std::cerr << "Profile error: " << error.what() << '\n';
        return 1;
    }
}