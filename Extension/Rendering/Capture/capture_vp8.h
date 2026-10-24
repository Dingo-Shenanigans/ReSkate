#pragma once
#include <cstdint>
#include <vector>

// A small VP8 encoder (RFC 6386) for the captures' WebM files: every frame a key frame, 16x16
// intra prediction only, one token partition, no loop filter. Simple rather than small, which
// suits a few seconds of a turning board.
namespace dingosdk::capture::vp8 {
// 8-bit 4:2:0: `y` is width x height, `u` and `v` are half that, rounded up.
struct Planes {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> y, u, v;
};
// One key frame. `quality` is VP8's quantizer index, 0 (best) .. 127.
std::vector<std::uint8_t> encode_keyframe(const Planes& planes, int quality);
} // namespace dingosdk::capture::vp8
