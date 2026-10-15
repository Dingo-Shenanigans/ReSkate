#pragma once
// Synthesised FPV motor sound. The maths (MotorSynth) is plain C++ and testable anywhere;
// only the Windows output thread (winmm waveOut, already linked for the radio) is platform specific.
// Pitch follows the throttle, with a little lag like real motors, plus a rush of wind with speed.
#include "drone_flight.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#include <thread>
#endif

namespace dingosdk::drone::audio {

class MotorSynth {
public:
    static constexpr int rate = 44100;
    // Fills `count` mono 16-bit samples. throttle 0..1, speed in m/s, volume 0..1; pitch scales the
    // motor note (1 = normal; small drones spin faster and sound higher).
    void render(std::int16_t* out, int count, float throttle, float speed, float volume, float pitch = 1.0f) {
        const float note = std::isfinite(pitch) ? std::clamp(pitch, 0.25f, 4.0f) : 1.0f;
        const float target = std::isfinite(throttle) ? std::clamp(throttle, 0.0f, 1.0f) : 0.0f;
        const float vol = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
        const float wind = std::isfinite(speed) ? std::clamp(speed / 60.0f, 0.0f, 1.0f) : 0.0f;
        constexpr float tau = 0.08f;                         // motors spin up/down over ~80 ms
        const float k = 1.0f - std::exp(-1.0f / (tau * rate));
        constexpr float detune[4] = {1.0f, 1.013f, 0.989f, 1.027f};
        constexpr float tpi = 6.283185307179586f;
        for (int i = 0; i < count; ++i) {
            rpm_ += (target - rpm_) * k;
            const float base = (110.0f + 520.0f * rpm_) * note; // fundamental, Hz
            float sample = 0.0f;
            for (int m = 0; m < 4; ++m) {
                phase_[m] += base * detune[m] / rate;
                phase_[m] -= std::floor(phase_[m]);
                const float p = phase_[m] * tpi;
                sample += std::sin(p) + 0.55f * std::sin(2 * p) + 0.35f * std::sin(3 * p) + 0.2f * std::sin(5 * p);
            }
            // cheap noise, low-passed more for the wind, brighter for the props
            seed_ = seed_ * 1664525u + 1013904223u;
            const float white = static_cast<float>(static_cast<std::int32_t>(seed_)) / 2147483648.0f;
            lp_ += (white - lp_) * 0.12f;
            const float noise = (0.35f * rpm_ + 0.5f * wind) * lp_ + 0.12f * rpm_ * white;
            const float level = 0.03f + 0.07f * rpm_;         // idle whine is quieter than full power
            const float v = std::tanh((sample * level + noise * 0.25f) * 1.6f) * 0.85f * vol;
            out[i] = static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32000.0f);
        }
    }
    float rpm() const { return rpm_; }
private:
    float rpm_ = 0, lp_ = 0;
    float phase_[4] = {0, 0.25f, 0.5f, 0.75f};
    std::uint32_t seed_ = 12345u;
};

#ifdef _WIN32
namespace detail {
inline void run() {
    constexpr int frames = 1024, buffers = 4;
    MotorSynth synth;
    std::int16_t data[buffers][frames]{};
    WAVEHDR header[buffers]{};
    HWAVEOUT device{};
    bool open = false;
    long long idle_since = 0;
    for (;;) {
        const bool active = flying() && telemetry().armed.load(std::memory_order_relaxed) && knobs().value[kVolume].load(std::memory_order_relaxed) > 0.001f;
        const long long now = now_ms();
        if (!active) {
            if (open && now - idle_since > 600) {
                waveOutReset(device);
                for (auto& h : header) if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device, &h, sizeof(h));
                waveOutClose(device);
                open = false;
            }
            Sleep(40);
            continue;
        }
        idle_since = now;
        if (!open) {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = MotorSynth::rate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 2;
            format.nAvgBytesPerSec = MotorSynth::rate * 2;
            if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { Sleep(1000); continue; }
            for (int b = 0; b < buffers; ++b) {
                header[b] = {};
                header[b].lpData = reinterpret_cast<LPSTR>(data[b]);
                header[b].dwBufferLength = frames * 2;
                waveOutPrepareHeader(device, &header[b], sizeof(header[b]));
            }
            open = true;
        }
        for (int b = 0; b < buffers; ++b) {
            if (header[b].dwFlags & WHDR_INQUEUE) continue;
            auto& t = telemetry();
            synth.render(data[b], frames, t.throttle.load(std::memory_order_relaxed), t.speed.load(std::memory_order_relaxed),
                         knobs().value[kVolume].load(std::memory_order_relaxed),
                         knobs().value[kMotorPitch].load(std::memory_order_relaxed));
            waveOutWrite(device, &header[b], sizeof(header[b]));
        }
        Sleep(4);
    }
}
} // namespace detail

// Starts the sound thread once. Safe to call every tick.
inline void ensure_started() {
    static const bool started = [] { std::thread(detail::run).detach(); return true; }();
    (void)started;
}
#else
inline void ensure_started() {}
#endif
} // namespace dingosdk::drone::audio
