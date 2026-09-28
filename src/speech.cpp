#include "joystick_penguin/speech.hpp"

#if defined(JOYSTICK_PENGUIN_ENABLE_TTS)

#include <espeak-ng/speak_lib.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace joystick_penguin {

struct EspeakSpeaker::Impl {
    explicit Impl(SpeechParameters value) : parameters(std::move(value)) {}

    SpeechParameters parameters;
    std::mutex mutex;
    std::condition_variable queued;
    std::deque<std::string> phrases;
    bool stopping = false;
    std::thread worker;

    void run() {
        const int sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCH_PLAYBACK, 0, nullptr, 0);
        if (sample_rate <= 0) {
            std::fprintf(stderr, "espeak-ng: initialization failed; mode announcements disabled\n");
        } else {
            if (!parameters.voice.empty() && espeak_SetVoiceByName(parameters.voice.c_str()) != EE_OK)
                std::fprintf(stderr, "espeak-ng: unknown voice '%s'; using the default voice\n",
                             parameters.voice.c_str());
            espeak_SetParameter(espeakRATE, parameters.speed, 0);
            espeak_SetParameter(espeakPITCH, parameters.pitch, 0);
            espeak_SetParameter(espeakRANGE, parameters.range, 0);
        }

        std::unique_lock lock(mutex);
        while (true) {
            queued.wait(lock, [this] { return stopping || !phrases.empty(); });
            if (phrases.empty()) break; // Only reachable once stopping is set.
            std::string phrase = std::move(phrases.front());
            phrases.pop_front();
            if (stopping || sample_rate <= 0) continue;
            lock.unlock();
            espeak_Synth(phrase.c_str(), phrase.size() + 1, 0, POS_CHARACTER, 0,
                         espeakCHARS_UTF8, nullptr, nullptr);
            espeak_Synchronize();
            lock.lock();
        }
        lock.unlock();
        if (sample_rate > 0) espeak_Terminate();
    }
};

EspeakSpeaker::EspeakSpeaker(SpeechParameters parameters)
    : impl_(std::make_unique<Impl>(std::move(parameters))) {
    impl_->worker = std::thread([this] { impl_->run(); });
}

EspeakSpeaker::~EspeakSpeaker() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->queued.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void EspeakSpeaker::speak(const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return;
        impl_->phrases.push_back(text);
    }
    impl_->queued.notify_one();
}

} // namespace joystick_penguin

#endif