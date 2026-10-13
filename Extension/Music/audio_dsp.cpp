#include "audio_dsp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dingosdk::audio_dsp {
namespace {
std::array<std::atomic<float>, bands> wanted{};
std::atomic<bool> level{};
std::atomic<unsigned> changes{1};

// One filter of the equaliser, for both channels (the "Audio EQ Cookbook" forms).
struct Filter {
    float b0{1}, b1{}, b2{}, a1{}, a2{};
    float x1[2]{}, x2[2]{}, y1[2]{}, y2[2]{};
    bool flat{true};
    void set(int which, float decibels, float rate) {
        flat = std::fabs(decibels) < 0.05f;
        if (flat) return;
        const float pi = 3.14159265f;
        const float amount = std::pow(10.0f, decibels / 40.0f);
        const float angle = 2.0f * pi * std::min(centres[which], rate * 0.45f) / rate;
        const float cosine = std::cos(angle), sine = std::sin(angle);
        float a0{};
        if (which == 0 || which == bands - 1) {
            // A shelf: everything below the first band's centre, or above the last one's.
            const float alpha = sine / 2.0f * std::sqrt(2.0f), root = 2.0f * std::sqrt(amount) * alpha;
            const float sign = which == 0 ? 1.0f : -1.0f;
            b0 = amount * ((amount + 1) - sign * (amount - 1) * cosine + root);
            b1 = sign * 2 * amount * ((amount - 1) - sign * (amount + 1) * cosine);
            b2 = amount * ((amount + 1) - sign * (amount - 1) * cosine - root);
            a0 = (amount + 1) + sign * (amount - 1) * cosine + root;
            a1 = sign * -2 * ((amount - 1) + sign * (amount + 1) * cosine);
            a2 = (amount + 1) + sign * (amount - 1) * cosine - root;
        } else {
            const float alpha = sine / (2.0f * 0.9f);
            b0 = 1 + alpha * amount;
            b1 = -2 * cosine;
            b2 = 1 - alpha * amount;
            a0 = 1 + alpha / amount;
            a1 = -2 * cosine;
            a2 = 1 - alpha / amount;
        }
        b0 /= a0, b1 /= a0, b2 /= a0, a1 /= a0, a2 /= a0;
    }
    float run(float in, int channel) {
        const float out = b0 * in + b1 * x1[channel] + b2 * x2[channel] - a1 * y1[channel] - a2 * y2[channel];
        x2[channel] = x1[channel], x1[channel] = in;
        y2[channel] = y1[channel], y1[channel] = std::fabs(out) < 1e-20f ? 0.0f : out; // (no lingering in numbers too small to hear)
        return out;
    }
};

std::mutex mutex; // what is below: the sound comes from one thread at a time, but not always the same one
std::array<Filter, bands> filters;
unsigned set_for{}, set_rate{};
float power{0.02f}, gain{1.0f}; // the loudness of the last seconds (mean square), and what it is levelled by

// Nothing over full scale: what nears it is bent under it, not cut off.
float limited(float sample) {
    const float size = std::fabs(sample);
    if (size <= 0.8f) return sample;
    return std::copysign(0.8f + 0.2f * std::tanh((size - 0.8f) / 0.2f), sample);
}
}

void set_band(int which, float decibels) noexcept {
    if (which < 0 || which >= bands || !std::isfinite(decibels)) return;
    wanted[static_cast<std::size_t>(which)].store(std::clamp(decibels, -most_decibels, most_decibels), std::memory_order_relaxed);
    changes.fetch_add(1, std::memory_order_relaxed);
}
float band(int which) noexcept { return which < 0 || which >= bands ? 0.0f : wanted[static_cast<std::size_t>(which)].load(std::memory_order_relaxed); }
void set_levelling(bool on) noexcept { level.store(on, std::memory_order_relaxed); }
bool levelling() noexcept { return level.load(std::memory_order_relaxed); }

void process(float* interleaved, unsigned frames, unsigned rate) noexcept {
    if (!interleaved || !frames || rate < 8000) return;
    std::lock_guard lock(mutex);
    if (const auto now = changes.load(std::memory_order_relaxed); now != set_for || rate != set_rate) {
        for (int i = 0; i < bands; ++i) filters[static_cast<std::size_t>(i)].set(i, band(i), static_cast<float>(rate));
        set_for = now;
        set_rate = rate;
    }
    const bool equalised = std::any_of(filters.begin(), filters.end(), [](const Filter& filter) { return !filter.flat; });
    const bool levelled = level.load(std::memory_order_relaxed);
    if (!equalised && !levelled) {
        gain = 1.0f;
        return;
    }
    if (equalised)
        for (auto& filter : filters) {
            if (filter.flat) continue;
            for (unsigned i = 0; i < frames; ++i) {
                interleaved[i * 2] = filter.run(interleaved[i * 2], 0);
                interleaved[i * 2 + 1] = filter.run(interleaved[i * 2 + 1], 1);
            }
        }
    float aim = 1.0f;
    if (levelled) {
        // How loud this piece is, into how loud the last three seconds were; silence between
        // songs is left out, so that the next one does not begin too loud.
        double sum{};
        for (unsigned i = 0; i < frames * 2; ++i) sum += static_cast<double>(interleaved[i]) * interleaved[i];
        const float piece = static_cast<float>(sum / (frames * 2.0));
        if (piece > 1e-5f) {
            const float share = 1.0f - std::exp(-static_cast<float>(frames) / (static_cast<float>(rate) * 3.0f));
            power += (piece - power) * share;
        }
        // Towards a loudness of about -16 dB, by no more than three times up or down.
        aim = std::clamp(0.16f / std::sqrt(std::max(power, 1e-6f)), 1.0f / 3.0f, 3.0f);
    }
    // (It comes down in a fifth of a second and goes up in a second, so it is not heard working.)
    const float down = 1.0f - std::exp(-1.0f / (static_cast<float>(rate) * 0.2f)), up = 1.0f - std::exp(-1.0f / static_cast<float>(rate));
    for (unsigned i = 0; i < frames; ++i) {
        gain += (aim - gain) * (aim < gain ? down : up);
        interleaved[i * 2] = limited(interleaved[i * 2] * gain);
        interleaved[i * 2 + 1] = limited(interleaved[i * 2 + 1] * gain);
    }
}
}
