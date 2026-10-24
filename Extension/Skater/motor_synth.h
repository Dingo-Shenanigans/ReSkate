#pragma once
// A synthesised motor: four detuned rotors and a rush of wind, its pitch following the throttle with a
// little lag, as real motors spin up. Plain C++, testable anywhere. Plugins' motor sounds
// (reskate.audio "motor", Extension/Plugins/plugin_sound.cpp) play it.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dingosdk::audio {
class MotorSynth {
public:
    static constexpr int rate = 44100; // plugins' sounds play at 16 kHz
    explicit MotorSynth(int sample_rate = rate)
        : rate_(sample_rate > 0 ? sample_rate : rate),
          // the wind's low-pass at about 900 Hz whatever the rate (0.12 a sample at 44.1 kHz)
          wind_k_(1.0f - std::exp(-6.2831853f * 900.0f / static_cast<float>(rate_))) {}
    // Fills `count` mono 16-bit samples. throttle 0..1, speed in m/s, volume 0..1; pitch scales the
    // motor note (1 = normal; small drones spin faster and sound higher).
    void render(std::int16_t* out, int count, float throttle, float speed, float volume, float pitch = 1.0f) {
        const float note = std::isfinite(pitch) ? std::clamp(pitch, 0.25f, 4.0f) : 1.0f;
        const float target = std::isfinite(throttle) ? std::clamp(throttle, 0.0f, 1.0f) : 0.0f;
        const float vol = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
        const float wind = std::isfinite(speed) ? std::clamp(speed / 60.0f, 0.0f, 1.0f) : 0.0f;
        constexpr float tau = 0.08f;                         // motors spin up/down over ~80 ms
        const float k = 1.0f - std::exp(-1.0f / (tau * static_cast<float>(rate_)));
        constexpr float detune[4] = {1.0f, 1.013f, 0.989f, 1.027f};
        constexpr float tpi = 6.283185307179586f;
        for (int i = 0; i < count; ++i) {
            rpm_ += (target - rpm_) * k;
            const float base = (110.0f + 520.0f * rpm_) * note; // fundamental, Hz
            float sample = 0.0f;
            for (int m = 0; m < 4; ++m) {
                phase_[m] += base * detune[m] / static_cast<float>(rate_);
                phase_[m] -= std::floor(phase_[m]);
                const float p = phase_[m] * tpi;
                sample += std::sin(p) + 0.55f * std::sin(2 * p) + 0.35f * std::sin(3 * p) + 0.2f * std::sin(5 * p);
            }
            // cheap noise, low-passed more for the wind, brighter for the props
            seed_ = seed_ * 1664525u + 1013904223u;
            const float white = static_cast<float>(static_cast<std::int32_t>(seed_)) / 2147483648.0f;
            lp_ += (white - lp_) * wind_k_;
            const float noise = (0.35f * rpm_ + 0.5f * wind) * lp_ + 0.12f * rpm_ * white;
            const float level = 0.03f + 0.07f * rpm_;         // idle whine is quieter than full power
            const float v = std::tanh((sample * level + noise * 0.25f) * 1.6f) * 0.85f * vol;
            out[i] = static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32000.0f);
        }
    }
    float rpm() const { return rpm_; }
private:
    int rate_;
    float wind_k_;
    float rpm_ = 0, lp_ = 0;
    float phase_[4] = {0, 0.25f, 0.5f, 0.75f};
    std::uint32_t seed_ = 12345u;
};
} // namespace dingosdk::audio
