#include "turnaround_encode.h"
#include <miniz.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

// PNG and APNG (https://www.w3.org/TR/png-3/#apng-frame-control) for board turnarounds.
namespace dingosdk::turnaround {
namespace {
std::uint32_t png_crc(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            auto c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const auto b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

struct Writer {
    std::vector<std::uint8_t> out;
    void u32(std::vector<std::uint8_t>& to, std::uint32_t v) {
        to.insert(to.end(), {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
                             static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
    }
    void u16(std::vector<std::uint8_t>& to, std::uint16_t v) {
        to.insert(to.end(), {static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
    }
    void chunk(std::string_view type, std::span<const std::uint8_t> data) {
        u32(out, static_cast<std::uint32_t>(data.size()));
        const auto start = out.size();
        out.insert(out.end(), type.begin(), type.end());
        out.insert(out.end(), data.begin(), data.end());
        u32(out, png_crc({out.data() + start, out.size() - start}));
    }
};

// The image's rows, each with the PNG filter that leaves the smallest sum of magnitudes (the usual
// heuristic), compressed with zlib.
std::vector<std::uint8_t> image_data(const Image& image) {
    const std::size_t stride = std::size_t{image.width} * 4;
    std::vector<std::uint8_t> filtered;
    filtered.reserve((stride + 1) * image.height);
    std::vector<std::uint8_t> zero(stride, 0), candidate(stride), best(stride);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto* row = image.rgba.data() + y * stride;
        const auto* up = y ? row - stride : zero.data();
        std::uint64_t best_cost = ~0ull;
        std::uint8_t best_filter = 0;
        for (std::uint8_t filter = 0; filter < 5; ++filter) {
            std::uint64_t cost = 0;
            for (std::size_t i = 0; i < stride; ++i) {
                const int a = i >= 4 ? row[i - 4] : 0, b = up[i], c = i >= 4 ? up[i - 4] : 0;
                int predicted = 0;
                switch (filter) {
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = (a + b) / 2; break;
                case 4: {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                    break;
                }
                default: break;
                }
                candidate[i] = static_cast<std::uint8_t>(row[i] - predicted);
                cost += static_cast<std::uint64_t>(std::abs(static_cast<std::int8_t>(candidate[i])));
            }
            if (cost < best_cost) best_cost = cost, best_filter = filter, best.swap(candidate);
        }
        filtered.push_back(best_filter);
        filtered.insert(filtered.end(), best.begin(), best.end());
    }
    std::size_t size = 0;
    void* packed = tdefl_compress_mem_to_heap(filtered.data(), filtered.size(), &size, TDEFL_WRITE_ZLIB_HEADER | 256);
    if (!packed) throw std::runtime_error("PNG compression failed.");
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(packed), static_cast<std::uint8_t*>(packed) + size);
    mz_free(packed);
    return out;
}

void header(Writer& w, const Image& image) {
    static constexpr std::uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    w.out.assign(std::begin(signature), std::end(signature));
    std::vector<std::uint8_t> ihdr;
    w.u32(ihdr, image.width);
    w.u32(ihdr, image.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, adaptive filters, not interlaced
    w.chunk("IHDR", ihdr);
}
} // namespace

std::vector<std::uint8_t> encode_png(const Image& image) {
    if (image.empty()) throw std::invalid_argument("The picture is empty.");
    Writer w;
    header(w, image);
    w.chunk("IDAT", image_data(image));
    w.chunk("IEND", {});
    return std::move(w.out);
}

std::vector<std::uint8_t> encode_apng(const Animation& animation) {
    if (animation.frames.empty() || animation.frames.front().empty()) throw std::invalid_argument("No frames to save.");
    const auto& first = animation.frames.front();
    for (const auto& frame : animation.frames)
        if (frame.width != first.width || frame.height != first.height)
            throw std::invalid_argument("The frames are not all the same size.");
    Writer w;
    header(w, first);
    std::vector<std::uint8_t> actl;
    w.u32(actl, static_cast<std::uint32_t>(animation.frames.size()));
    w.u32(actl, 0); // loop forever
    w.chunk("acTL", actl);
    std::uint32_t sequence = 0;
    const auto fps = static_cast<std::uint16_t>(std::clamp<std::uint32_t>(animation.fps, 1, 120));
    for (std::size_t i = 0; i < animation.frames.size(); ++i) {
        const auto& frame = animation.frames[i];
        std::vector<std::uint8_t> fctl;
        w.u32(fctl, sequence++);
        w.u32(fctl, frame.width);
        w.u32(fctl, frame.height);
        w.u32(fctl, 0);
        w.u32(fctl, 0);
        w.u16(fctl, 1);   // delay: 1 / fps seconds
        w.u16(fctl, fps);
        // Each frame is whole: clear what the last one left (dispose to background) and replace,
        // not blend, so see-through parts of the board stay see-through.
        fctl.insert(fctl.end(), {1, 0});
        w.chunk("fcTL", fctl);
        auto data = image_data(frame);
        if (!i) {
            w.chunk("IDAT", data);
            continue;
        }
        std::vector<std::uint8_t> fdat;
        fdat.reserve(data.size() + 4);
        w.u32(fdat, sequence++);
        fdat.insert(fdat.end(), data.begin(), data.end());
        w.chunk("fdAT", fdat);
    }
    w.chunk("IEND", {});
    return std::move(w.out);
}
} // namespace dingosdk::turnaround
