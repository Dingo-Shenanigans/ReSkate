#include "plugin_wave.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dingosdk::plugins {
namespace {
constexpr std::uint16_t format_pcm = 1, format_float = 3, format_extensible = 0xfffe;
template <class T> T get(const std::uint8_t* at) {
    T value{};
    std::memcpy(&value, at, sizeof value);
    return value;
}
}

std::shared_ptr<const Wave> decode_wave(std::span<const std::uint8_t> bytes, float most_seconds, std::string& error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        error = "it is not a WAVE file";
        return nullptr;
    }
    std::span<const std::uint8_t> format, data;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        auto size = static_cast<std::size_t>(get<std::uint32_t>(bytes.data() + at + 4));
        const auto body = at + 8;
        size = std::min(size, bytes.size() - body);
        if (!std::memcmp(bytes.data() + at, "fmt ", 4)) format = bytes.subspan(body, size);
        else if (!std::memcmp(bytes.data() + at, "data", 4)) data = bytes.subspan(body, size);
        at = body + size + (size & 1);
    }
    if (format.size() < 16 || data.empty()) {
        error = "it has no format or no sound";
        return nullptr;
    }
    auto tag = get<std::uint16_t>(format.data());
    const unsigned channels = get<std::uint16_t>(format.data() + 2), rate = get<std::uint32_t>(format.data() + 4),
                   bits = get<std::uint16_t>(format.data() + 14);
    if (tag == format_extensible && format.size() >= 26) tag = get<std::uint16_t>(format.data() + 24); // its SubFormat
    const bool floats = tag == format_float;
    if (!channels || channels > 8 || rate < 4000 || rate > 192000 || (tag != format_pcm && !floats) || (floats && bits != 32) ||
        (!floats && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        error = "its format is not 8 to 32-bit PCM or 32-bit float";
        return nullptr;
    }
    const std::size_t stride = channels * (bits / 8), frames = data.size() / stride;
    if (static_cast<double>(frames) / rate > most_seconds) {
        error = "it is longer than " + std::to_string(static_cast<int>(most_seconds)) + " s";
        return nullptr;
    }
    const auto at = [&](std::size_t frame) {
        float sum = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            const auto* p = data.data() + frame * stride + c * (bits / 8);
            float v{};
            if (floats) v = get<float>(p);
            else if (bits == 8) v = (static_cast<float>(*p) - 128.0f) / 128.0f;
            else if (bits == 16) v = get<std::int16_t>(p) / 32768.0f;
            else if (bits == 24) v = static_cast<float>(static_cast<std::int32_t>((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8) / 8388608.0f;
            else v = static_cast<float>(get<std::int32_t>(p)) / 2147483648.0f;
            sum += std::isfinite(v) ? v : 0.0f;
        }
        return sum / static_cast<float>(channels);
    };
    // Each output sample is the mean of the input samples it spans (a plain low-pass).
    const double step = static_cast<double>(rate) / wave_rate;
    const auto count = static_cast<std::size_t>(static_cast<double>(frames) / step);
    auto wave = std::make_shared<Wave>();
    wave->samples.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto from = static_cast<std::size_t>(static_cast<double>(i) * step);
        const auto to = std::min(frames, std::max(from + 1, static_cast<std::size_t>(static_cast<double>(i + 1) * step)));
        float sum = 0.0f;
        for (auto f = from; f < to; ++f) sum += at(f);
        wave->samples[i] = static_cast<std::int16_t>(std::clamp(sum / static_cast<float>(to - from), -1.0f, 1.0f) * 32767.0f);
    }
    if (wave->samples.empty()) {
        error = "it holds no sound";
        return nullptr;
    }
    return wave;
}
}
