#include "plugin_stream_wave.h"
#include <cstring>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace dingosdk::plugins {
std::shared_ptr<const StreamWave> open_stream_wave(const std::filesystem::path& file, std::string& why) {
    why.clear();
#if defined(_WIN32)
    struct Mapping {
        HANDLE file{INVALID_HANDLE_VALUE}, mapping{};
        void* address{};
        ~Mapping() {
            if (address) UnmapViewOfFile(address);
            if (mapping) CloseHandle(mapping);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    };
    const auto bad = [&](const char* message) -> std::shared_ptr<const StreamWave> {
        why = message;
        return {};
    };
    auto mapped = std::make_shared<Mapping>();
    mapped->file = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (mapped->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(mapped->file, &size)) return bad("the music file cannot be opened");
    if (size.QuadPart < 44 || size.QuadPart > 128ll * 1024 * 1024) return bad("a streamed wave is 44 bytes to 128 MB");
    mapped->mapping = CreateFileMappingW(mapped->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapped->mapping) return bad("the music file cannot be mapped");
    mapped->address = MapViewOfFile(mapped->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!mapped->address) return bad("the music file cannot be mapped");
    const auto bytes = std::span(static_cast<const std::uint8_t*>(mapped->address), static_cast<std::size_t>(size.QuadPart));
    const auto u16 = [&](std::size_t p) { return static_cast<unsigned>(bytes[p]) | static_cast<unsigned>(bytes[p + 1]) << 8; };
    const auto u32 = [&](std::size_t p) { return u16(p) | (u16(p + 2) << 16); };
    if (std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4) || u32(4) != bytes.size() - 8)
        return bad("the music file is not a complete RIFF WAVE");
    auto wave = std::make_shared<StreamWave>();
    bool format{};
    unsigned chunks = 0;
    for (std::size_t at = 12; at < bytes.size();) {
        // Opening a track runs on the client thread. Bound the work spent skipping metadata.
        if (++chunks > 1024) return bad("too many chunks in the music file");
        if (bytes.size() - at < 8) return bad("incomplete music chunk header");
        const auto count = u32(at + 4);
        const auto start = at + 8;
        if (static_cast<std::size_t>(count) + (count & 1u) > bytes.size() - start) return bad("a music chunk ends outside the file");
        if (!std::memcmp(bytes.data() + at, "fmt ", 4)) {
            if (format || count < 16 || u16(start) != 1 || u16(start + 14) != 16) return bad("streamed music is 16-bit PCM");
            wave->channels = u16(start + 2);
            wave->rate = u32(start + 4);
            if (wave->channels < 1 || wave->channels > 2 || wave->rate < 8000 || wave->rate > 96000 ||
                u16(start + 12) != wave->channels * 2 || u32(start + 8) != wave->rate * wave->channels * 2)
                return bad("streamed music has one or two channels at 8 to 96 kHz");
            format = true;
        } else if (!std::memcmp(bytes.data() + at, "data", 4)) {
            if (!count || !wave->pcm.empty()) return bad("streamed music has one data chunk");
            wave->pcm = bytes.subspan(start, count);
        }
        at = start + count + (count & 1u);
    }
    if (!format || wave->pcm.empty() || wave->pcm.size() % (wave->channels * 2) || wave->frames() > static_cast<std::size_t>(wave->rate) * 1200)
        return bad("streamed music holds complete PCM frames, at most 20 minutes");
    wave->storage = std::move(mapped);
    return wave;
#else
    (void)file;
    why = "file-backed audio is available on Windows clients";
    return {};
#endif
}
}
