#pragma once
#include "capture_parallel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dingosdk::capture {
enum class ReadbackColour { rgb10, half_float };

inline float readback_half(std::uint16_t h) noexcept {
    const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 0x3ff;
    float value;
    if (!exponent)
        value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        value = mantissa ? 0.0f : 65504.0f;
    else
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}
inline std::uint8_t readback_srgb8(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    const float s = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}
// Preserve the existing SDR conversion byte for byte. HDR input has a finite
// channel domain, so lookup tables replace per-pixel division/pow; disjoint
// rows can then be converted concurrently before the mapped frame is released.
inline void readback_rgba(const std::uint8_t* input, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, ReadbackColour colour,
                          std::vector<std::uint8_t>& output) {
    output.resize(std::size_t{width} * height * 4);
    if (colour == ReadbackColour::rgb10) {
        static const auto lookup = [] {
            std::array<std::uint8_t, 1024> table{};
            for (std::size_t n = 0; n < table.size(); ++n)
                table[n] = static_cast<std::uint8_t>(n * 255 / 1023);
            return table;
        }();
        parallel_rows(height, width, [&](std::uint32_t first, std::uint32_t last) {
            for (auto y = first; y < last; ++y)
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint32_t packed;
                    std::memcpy(&packed, input + std::size_t{y} * pitch + std::size_t{x} * 4, 4);
                    auto* out = output.data() + (std::size_t{y} * width + x) * 4;
                    out[0] = lookup[packed & 0x3ff];
                    out[1] = lookup[(packed >> 10) & 0x3ff];
                    out[2] = lookup[(packed >> 20) & 0x3ff];
                    out[3] = 255;
                }
        });
    } else {
        static const auto lookup = [] {
            std::array<std::uint8_t, 65536> table{};
            for (std::size_t n = 0; n < table.size(); ++n)
                table[n] = readback_srgb8(readback_half(static_cast<std::uint16_t>(n)));
            return table;
        }();
        parallel_rows(height, width, [&](std::uint32_t first, std::uint32_t last) {
            for (auto y = first; y < last; ++y)
                for (std::uint32_t x = 0; x < width; ++x) {
                    std::uint16_t channels[4];
                    std::memcpy(channels, input + std::size_t{y} * pitch + std::size_t{x} * 8, 8);
                    auto* out = output.data() + (std::size_t{y} * width + x) * 4;
                    out[0] = lookup[channels[0]];
                    out[1] = lookup[channels[1]];
                    out[2] = lookup[channels[2]];
                    out[3] = 255;
                }
        });
    }
}
} // namespace dingosdk::capture
