#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace dingosdk::plugins {
struct MusicLoop {
    std::uint32_t first{}, frames{};
};
// Read-only file-backed PCM; the mapping stays alive until its source voice is destroyed.
// Long music files do not occupy the decoded short-sound cache.
struct StreamWave {
    unsigned rate{}, channels{};
    std::span<const std::uint8_t> pcm;
    std::shared_ptr<void> storage;
    std::size_t frames() const noexcept { return pcm.size() / (channels * 2); }
};
std::shared_ptr<const StreamWave> open_stream_wave(const std::filesystem::path& file, std::string& why);
}
