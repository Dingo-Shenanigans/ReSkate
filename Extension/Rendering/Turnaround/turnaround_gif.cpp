#include "turnaround_encode.h"
#include <cmath>
#include <numeric>
#include <stdexcept>

// Animated GIF (GIF89a) for board turnarounds: one median-cut palette shared by every frame.
namespace dingosdk::turnaround {
namespace {
constexpr int bins = 32 * 32 * 32; // 5 bits a channel

std::uint32_t key_of(int r, int g, int b) noexcept {
    return static_cast<std::uint32_t>((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
}

// 4x4 Bayer matrix centred on zero, in steps of one 5-bit level (8 of 255) across its range.
constexpr std::array<int, 16> bayer{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
int dither_offset(std::uint32_t x, std::uint32_t y) noexcept { return bayer[(y & 3) * 4 + (x & 3)] / 2 - 4; }

struct Histogram {
    std::vector<std::uint32_t> count = std::vector<std::uint32_t>(bins);
    std::vector<std::array<std::uint64_t, 3>> sum = std::vector<std::array<std::uint64_t, 3>>(bins);
};

struct Box {
    std::vector<std::uint32_t> keys; // occupied bins
    std::uint64_t weight{};
};

// The bin's channel `c` (0 r, 1 g, 2 b), 0..31.
int channel(std::uint32_t key, int c) noexcept { return static_cast<int>(key >> (10 - c * 5)) & 31; }

std::vector<Rgb> median_cut(const Histogram& h, std::size_t colours) {
    std::vector<Box> boxes(1);
    for (std::uint32_t k = 0; k < bins; ++k)
        if (h.count[k]) boxes[0].keys.push_back(k), boxes[0].weight += h.count[k];
    if (boxes[0].keys.empty()) return {Rgb{0, 0, 0}};
    while (boxes.size() < colours) {
        // Split the box with the most pixels times its widest spread.
        std::size_t pick = boxes.size();
        std::uint64_t best = 0;
        int best_channel = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].keys.size() < 2) continue;
            for (int c = 0; c < 3; ++c) {
                int lo = 31, hi = 0;
                for (const auto k : boxes[i].keys) lo = std::min(lo, channel(k, c)), hi = std::max(hi, channel(k, c));
                const auto score = boxes[i].weight * static_cast<std::uint64_t>(hi - lo);
                if (score > best) best = score, pick = i, best_channel = c;
            }
        }
        if (pick == boxes.size()) break; // nothing left to split
        auto& box = boxes[pick];
        std::sort(box.keys.begin(), box.keys.end(),
                  [c = best_channel](auto a, auto b) { return channel(a, c) < channel(b, c); });
        // Split at the weighted median.
        std::uint64_t running = 0;
        std::size_t cut = 1;
        for (; cut < box.keys.size(); ++cut) {
            running += h.count[box.keys[cut - 1]];
            if (running * 2 >= box.weight) break;
        }
        cut = std::clamp<std::size_t>(cut, 1, box.keys.size() - 1);
        Box other;
        other.keys.assign(box.keys.begin() + static_cast<std::ptrdiff_t>(cut), box.keys.end());
        box.keys.resize(cut);
        box.weight = 0;
        for (const auto k : box.keys) box.weight += h.count[k];
        for (const auto k : other.keys) other.weight += h.count[k];
        boxes.push_back(std::move(other));
    }
    std::vector<Rgb> palette;
    for (const auto& box : boxes) {
        std::array<std::uint64_t, 3> total{};
        for (const auto k : box.keys)
            for (int c = 0; c < 3; ++c) total[c] += h.sum[k][c];
        Rgb colour{};
        for (int c = 0; c < 3; ++c) colour[c] = static_cast<std::uint8_t>((total[c] + box.weight / 2) / box.weight);
        palette.push_back(colour);
    }
    return palette;
}

// Variable-length LZW (GIF's flavour) of 8-bit indices.
std::vector<std::uint8_t> lzw(const std::vector<std::uint8_t>& indices) {
    constexpr int min_bits = 8, clear = 1 << min_bits, end = clear + 1;
    std::vector<std::uint8_t> out;
    std::uint32_t bit_buffer = 0;
    int bit_count = 0, code_bits = min_bits + 1, next = end + 1;
    const auto emit = [&](int code) {
        bit_buffer |= static_cast<std::uint32_t>(code) << bit_count;
        bit_count += code_bits;
        while (bit_count >= 8) out.push_back(static_cast<std::uint8_t>(bit_buffer)), bit_buffer >>= 8, bit_count -= 8;
    };
    std::vector<std::int16_t> table(4096 * 256, -1); // (prefix code, next index) -> code
    const auto reset = [&] {
        std::fill(table.begin(), table.end(), static_cast<std::int16_t>(-1));
        code_bits = min_bits + 1;
        next = end + 1;
    };
    emit(clear);
    if (indices.empty()) {
        emit(end);
    } else {
        int prefix = indices[0];
        for (std::size_t i = 1; i < indices.size(); ++i) {
            const auto byte = indices[i];
            const auto slot = static_cast<std::size_t>(prefix) * 256 + byte;
            if (table[slot] >= 0) { prefix = table[slot]; continue; }
            emit(prefix);
            if (next < 4096) {
                table[slot] = static_cast<std::int16_t>(next++);
                if (next > (1 << code_bits) && code_bits < 12) ++code_bits;
            } else {
                emit(clear);
                reset();
            }
            prefix = byte;
        }
        emit(prefix);
        emit(end);
    }
    if (bit_count) out.push_back(static_cast<std::uint8_t>(bit_buffer));
    return out;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
} // namespace

std::vector<std::uint8_t> encode_gif(const Animation& animation, const GifOptions& options) {
    if (animation.frames.empty() || animation.frames.front().empty()) throw std::invalid_argument("No frames to save.");
    const auto width = animation.frames.front().width, height = animation.frames.front().height;
    if (width > 65535 || height > 65535) throw std::invalid_argument("Too large for a GIF.");
    for (const auto& frame : animation.frames)
        if (frame.width != width || frame.height != height) throw std::invalid_argument("The frames are not all the same size.");

    // Every kept pixel, its soft edge blended over the fringe colour.
    const auto kept = [&](const std::uint8_t* p, Rgb& colour) {
        if (p[3] < 128) return false;
        for (int c = 0; c < 3; ++c)
            colour[c] = static_cast<std::uint8_t>((p[c] * p[3] + options.fringe[c] * (255 - p[3]) + 127) / 255);
        return true;
    };
    Histogram histogram;
    bool transparent = false;
    for (const auto& frame : animation.frames)
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                Rgb colour;
                if (!kept(frame.at(x, y), colour)) { transparent = true; continue; }
                const auto k = key_of(colour[0], colour[1], colour[2]);
                ++histogram.count[k];
                for (int c = 0; c < 3; ++c) histogram.sum[k][c] += colour[c];
            }
    auto palette = median_cut(histogram, transparent ? 255 : 256);
    const auto colours = palette.size(); // the rest of the table is padding, never matched
    const auto transparent_index = static_cast<std::uint8_t>(colours);
    // Nearest palette colour of each bin, worked out when first needed.
    std::vector<std::int16_t> nearest(bins, -1);
    const auto map = [&](int r, int g, int b) {
        const auto k = key_of(r, g, b);
        if (nearest[k] < 0) {
            const int cr = (r & ~7) + 4, cg = (g & ~7) + 4, cb = (b & ~7) + 4;
            int best = 0, best_distance = INT32_MAX;
            for (std::size_t i = 0; i < colours; ++i) {
                const int dr = palette[i][0] - cr, dg = palette[i][1] - cg, db = palette[i][2] - cb;
                // Weighted roughly by how much the eye cares about each.
                const int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                if (d < best_distance) best_distance = d, best = static_cast<int>(i);
            }
            nearest[k] = static_cast<std::int16_t>(best);
        }
        return static_cast<std::uint8_t>(nearest[k]);
    };

    std::vector<std::uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
    put16(out, width);
    put16(out, height);
    out.push_back(0xf7); // global colour table of 256 entries
    out.push_back(0);
    out.push_back(0);
    palette.resize(256, Rgb{0, 0, 0});
    for (const auto& colour : palette) out.insert(out.end(), colour.begin(), colour.end());
    // Loop forever (NETSCAPE2.0).
    out.insert(out.end(), {0x21, 0xff, 0x0b, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00});

    const auto fps = std::clamp<std::uint32_t>(animation.fps, 1, 50);
    std::uint64_t elapsed = 0; // hundredths of a second written so far
    std::vector<std::uint8_t> indices(std::size_t{width} * height);
    for (std::size_t f = 0; f < animation.frames.size(); ++f) {
        const auto& frame = animation.frames[f];
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                Rgb colour;
                auto& index = indices[std::size_t{y} * width + x];
                if (!kept(frame.at(x, y), colour)) { index = transparent_index; continue; }
                const int offset = options.dither ? dither_offset(x, y) : 0;
                index = map(std::clamp(colour[0] + offset, 0, 255), std::clamp(colour[1] + offset, 0, 255),
                            std::clamp(colour[2] + offset, 0, 255));
            }
        const auto due = (f + 1) * 100 / fps;
        const auto delay = static_cast<std::uint32_t>(std::max<std::uint64_t>(2, due - elapsed));
        elapsed += delay;
        // Graphic control: clear to the background between frames when anything is see-through.
        out.insert(out.end(), {0x21, 0xf9, 0x04, static_cast<std::uint8_t>(transparent ? 0x09 : 0x04)});
        put16(out, delay);
        out.push_back(transparent ? transparent_index : 0);
        out.push_back(0);
        out.push_back(0x2c);
        put16(out, 0);
        put16(out, 0);
        put16(out, width);
        put16(out, height);
        out.push_back(0);
        out.push_back(8); // LZW minimum code size
        const auto data = lzw(indices);
        for (std::size_t i = 0; i < data.size(); i += 255) {
            const auto n = std::min<std::size_t>(255, data.size() - i);
            out.push_back(static_cast<std::uint8_t>(n));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(i + n));
        }
        out.push_back(0);
    }
    out.push_back(0x3b);
    return out;
}
} // namespace dingosdk::turnaround
