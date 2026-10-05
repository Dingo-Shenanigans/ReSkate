#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// A dedicated server's radio (Server/server_radio.h): 48 kHz stereo Opus in 20 ms frames, a few
// to a batch. Each batch travels as one PacketKind::radio (protocol 43) and the game plays it
// (Extension/Multiplayer/Voice/radio_player.h).
namespace dingosdk::multiplayer {
constexpr unsigned radio_rate = 48000, radio_channels = 2;
constexpr unsigned radio_frame_samples = 960; // per channel: 20 ms
constexpr std::size_t max_radio_frame = 1275;  // the largest Opus frame
constexpr std::size_t max_radio_batch = 10;    // frames
struct RadioBatch {
    std::uint32_t track{}; // one per song; a new one restarts the listener's decoder
    std::uint32_t first{}; // the batch's first frame within its track
    std::vector<std::vector<std::uint8_t>> frames;
};
inline std::vector<std::uint8_t> encode_radio_batch(const RadioBatch &batch) {
    if (!batch.track || batch.frames.empty() || batch.frames.size() > max_radio_batch)
        throw std::invalid_argument("Invalid radio batch");
    std::vector<std::uint8_t> out;
    const auto integer = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    integer(batch.track, 4);
    integer(batch.first, 4);
    integer(batch.frames.size(), 1);
    for (const auto &frame : batch.frames) {
        if (frame.empty() || frame.size() > max_radio_frame) throw std::invalid_argument("Invalid radio frame");
        integer(frame.size(), 2);
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}
inline std::optional<RadioBatch> decode_radio_batch(std::span<const std::uint8_t> bytes) noexcept {
    std::size_t at{};
    const auto integer = [&](unsigned width, std::uint64_t &value) {
        if (width > bytes.size() - at) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{bytes[at++]} << (8 * i);
        return true;
    };
    try {
        RadioBatch batch;
        std::uint64_t track{}, first{}, count{};
        if (!integer(4, track) || !integer(4, first) || !integer(1, count) || !track || !count || count > max_radio_batch)
            return {};
        batch.track = static_cast<std::uint32_t>(track);
        batch.first = static_cast<std::uint32_t>(first);
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t size{};
            if (!integer(2, size) || !size || size > max_radio_frame || size > bytes.size() - at) return {};
            batch.frames.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += static_cast<std::size_t>(size);
        }
        if (at != bytes.size()) return {};
        return batch;
    } catch (...) {
        return {};
    }
}
} // namespace dingosdk::multiplayer
