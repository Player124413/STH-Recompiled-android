// The mixer's output through an SDL audio stream (AAudio underneath). The game mixes 48 kHz stereo floats;
// Android's own resampler and latency handling take it from there.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "host.h"

namespace audio {

namespace {

constexpr uint32_t kBlock = kRate / 100;  // frames the mixer renders a call: 10 ms
std::atomic<bool> g_paused{false};
SDL_AudioStream* g_stream = nullptr;

struct Pump {
    std::function<void(float*, uint32_t)> render;
    std::vector<float> out;  // stereo frames rendered, not yet handed to SDL
};

void SDLCALL Feed(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& p = *static_cast<Pump*>(user);
    constexpr int kFrameBytes = 2 * sizeof(float);
    size_t want = size_t(std::max(additional, 0) / kFrameBytes) * 2;
    while (p.out.size() < want) {
        size_t at = p.out.size();
        p.out.resize(at + kBlock * 2);
        p.render(p.out.data() + at, kBlock);
    }
    if (!want) return;
    SDL_PutAudioStreamData(stream, p.out.data(), int(want * sizeof(float)));
    p.out.erase(p.out.begin(), p.out.begin() + std::ptrdiff_t(want));
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    auto* pump = new Pump{std::move(render), {}};
    pump->out.reserve(kBlock * 8);
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, pump);
    if (!g_stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        delete pump;
        return;
    }
    LOGI("audio: %s", SDL_GetCurrentAudioDriver());
    if (!g_paused) SDL_ResumeAudioStreamDevice(g_stream);
}

}  // namespace audio

namespace port {

void PauseAudio(bool paused) {
    if (paused == audio::g_paused.load()) return;
    audio::g_paused = paused;
    SDL_AudioStream* stream = audio::g_stream;
    if (!stream) return;
    if (paused) SDL_PauseAudioStreamDevice(stream);
    else SDL_ResumeAudioStreamDevice(stream);
}

}  // namespace port
