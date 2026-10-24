#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Sounds plugins ship (reskate.audio): a RIFF WAVE file read whole and made into what the game's
// positioned sound takes, 16 kHz mono 16-bit (as Board Break's sounds are). 8, 16, 24 or 32-bit PCM
// or 32-bit float, any rate and channels; at most `most_seconds` long. Portable: no game here.
namespace dingosdk::plugins {
inline constexpr unsigned wave_rate = 16000;
struct Wave {
    std::vector<std::int16_t> samples; // 16 kHz mono
    float seconds() const noexcept { return static_cast<float>(samples.size()) / wave_rate; }
};
// Empty, and `error` says why, for a file that is not one it can play.
std::shared_ptr<const Wave> decode_wave(std::span<const std::uint8_t> bytes, float most_seconds, std::string& error);
}
