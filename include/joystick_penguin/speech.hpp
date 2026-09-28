#pragma once

#include <memory>
#include <string>

namespace joystick_penguin {

// Parameters for mode-change announcements. The defaults select espeak-ng's
// default English voice at a slow, monotone delivery.
struct SpeechParameters {
    std::string voice; // espeak-ng voice name; empty selects the default voice.
    int speed = 180;   // Words per minute, 80-450.
    int pitch = 20;    // Base pitch, 0-100 (50 is normal).
    int range = 0;     // Pitch range, 0-100 (0 is monotone).
};

// Announces short phrases without blocking the caller. speak() may be called
// from the serialized engine loop; an implementation performs synthesis on its
// own thread.
class Speaker {
public:
    virtual ~Speaker() = default;
    virtual void speak(const std::string& text) = 0;
};

#if defined(JOYSTICK_PENGUIN_ENABLE_TTS)

// espeak-ng-backed speaker. Construction starts a worker thread that owns all
// espeak-ng state. Announcements are best-effort: initialization or synthesis
// failures are reported on stderr and never stop the remapper.
class EspeakSpeaker final : public Speaker {
public:
    explicit EspeakSpeaker(SpeechParameters parameters);
    ~EspeakSpeaker() override;
    EspeakSpeaker(const EspeakSpeaker&) = delete;
    EspeakSpeaker& operator=(const EspeakSpeaker&) = delete;
    void speak(const std::string& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

} // namespace joystick_penguin