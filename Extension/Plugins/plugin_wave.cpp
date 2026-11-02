#include "plugin_wave.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dingosdk::plugins {
namespace {
constexpr std::uint16_t format_pcm = 1, format_float = 3, format_ima_adpcm = 0x11, format_extensible = 0xfffe;
template <class T> T get(const std::uint8_t* at) {
    T value{};
    std::memcpy(&value, at, sizeof value);
    return value;
}
constexpr int ima_steps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int ima_index_moves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
// One channel's IMA ADPCM state, as each block's header starts it.
struct Ima {
    int predictor{}, index{};
    int next(unsigned nibble) noexcept {
        const int step = ima_steps[index];
        int diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
        predictor = std::clamp(nibble & 8 ? predictor - diff : predictor + diff, -32768, 32767);
        index = std::clamp(index + ima_index_moves[nibble & 7], 0, 88);
        return predictor;
    }
};
// The frames of IMA ADPCM blocks (Microsoft's layout, as ffmpeg's adpcm_ima_wav writes it), mixed to mono:
// each block opens with every channel's predictor and step index (4 bytes), which is its first frame, then
// 4 bytes of each channel in turn, 8 nibbles each, low nibble first. A block gives at most `block_frames`
// frames, and a short last block what it holds.
std::vector<float> decode_ima(std::span<const std::uint8_t> data, unsigned channels, std::size_t block_align,
    std::size_t block_frames, std::size_t frames) {
    std::vector<float> mono(frames);
    std::vector<Ima> state(channels);
    for (std::size_t block = 0, frame = 0; frame < frames && block + 4 * channels <= data.size(); block += block_align) {
        const auto* p = data.data() + block;
        const std::size_t size = std::min(block_align, data.size() - block);
        const std::size_t n = std::min({block_frames, 1 + (size - 4 * channels) / (4 * channels) * 8, frames - frame});
        for (unsigned c = 0; c < channels; ++c) {
            auto& ima = state[c];
            ima.predictor = get<std::int16_t>(p + 4 * c);
            ima.index = std::min<int>(p[4 * c + 2], 88);
            mono[frame] += static_cast<float>(ima.predictor);
            for (std::size_t i = 1; i < n; ++i) {
                const auto k = i - 1; // the nibble's place after the header: group k / 8, then k % 8 in it
                const auto byte = p[4 * channels + ((k / 8) * channels + c) * 4 + (k % 8) / 2];
                mono[frame + i] += static_cast<float>(ima.next((byte >> (4 * (k & 1))) & 15));
            }
        }
        frame += n;
    }
    for (auto& v : mono) v /= 32768.0f * static_cast<float>(channels);
    return mono;
}
}

std::shared_ptr<const Wave> decode_wave(std::span<const std::uint8_t> bytes, float most_seconds, std::string& error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        error = "it is not a WAVE file";
        return nullptr;
    }
    std::span<const std::uint8_t> format, data, fact;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        auto size = static_cast<std::size_t>(get<std::uint32_t>(bytes.data() + at + 4));
        const auto body = at + 8;
        size = std::min(size, bytes.size() - body);
        if (!std::memcmp(bytes.data() + at, "fmt ", 4)) format = bytes.subspan(body, size);
        else if (!std::memcmp(bytes.data() + at, "data", 4)) data = bytes.subspan(body, size);
        else if (!std::memcmp(bytes.data() + at, "fact", 4)) fact = bytes.subspan(body, size);
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
    const bool floats = tag == format_float, ima = tag == format_ima_adpcm;
    const std::size_t block_align = get<std::uint16_t>(format.data() + 12), header = 4u * channels;
    if (!channels || channels > 8 || rate < 4000 || rate > 192000 || (tag != format_pcm && !floats && !ima) || (floats && bits != 32) ||
        (ima && (bits != 4 || block_align <= header || (block_align - header) % header)) ||
        (tag == format_pcm && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        error = "its format is not 8 to 32-bit PCM, 32-bit float or IMA ADPCM";
        return nullptr;
    }
    std::size_t stride = 0, frames = 0, block_frames = 0;
    if (ima) {
        // Frames a block holds: what its size gives, or fewer when the format's extension says so.
        block_frames = 1 + (block_align - header) / header * 8;
        if (format.size() >= 20 && get<std::uint16_t>(format.data() + 16) >= 2)
            if (const std::size_t declared = get<std::uint16_t>(format.data() + 18)) block_frames = std::min(block_frames, declared);
        frames = data.size() / block_align * block_frames;
        if (const auto rest = data.size() % block_align; rest >= header)
            frames += std::min(block_frames, 1 + (rest - header) / header * 8);
        if (fact.size() >= 4) frames = std::min<std::size_t>(frames, get<std::uint32_t>(fact.data())); // the padding cut off
    } else {
        stride = channels * (bits / 8);
        frames = data.size() / stride;
    }
    if (static_cast<double>(frames) / rate > most_seconds) {
        error = "it is longer than " + std::to_string(static_cast<int>(most_seconds)) + " s";
        return nullptr;
    }
    const auto mono = ima ? decode_ima(data, channels, block_align, block_frames, frames) : std::vector<float>{};
    const auto at = [&](std::size_t frame) {
        if (ima) return mono[frame];
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
