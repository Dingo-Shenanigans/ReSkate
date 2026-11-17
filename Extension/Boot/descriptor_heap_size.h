#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// Each frame's share of the renderer's shader-visible descriptor heap is capped at 131,072
// descriptors, in a heap of 333,333. A dense level draws more than that in a frame (the Los Santos
// City sublevels with more than about 65,000 mesh sections), and the renderer then writes past
// the end of the heap: shaders read descriptors that are not there and fault or hang the GPU.
//
// GI shader records keep descriptor handles from the heap set when they were written, and are
// used with all three per-frame heaps. NVIDIA tolerates that only while the heaps share its 2^20
// pool; larger heaps split, and GI hangs the GPU after a level change. So use one heap with a
// region per frame: (1,000,000 - 120,000) / 3 = 293,333 a frame. The cap no longer binds there.
inline constexpr std::uint32_t heap_descriptor_total = 1'000'000;
inline constexpr std::uint32_t frame_descriptor_cap = 524'288;
// Fallback, three heaps of 320,000 that fit one pool.
inline constexpr std::uint32_t split_frame_descriptor_cap = 200'000;
// The game's own persistent part and frames in flight, for the log.
inline constexpr std::uint32_t persistent_descriptors = 120'000;
inline constexpr std::uint32_t frames_in_flight = 3;
extern bool single_heap_layout;

// Installs before executable entry, while the game is suspended.
bool start_descriptor_heap_size(std::uintptr_t base, std::string& error);
} // namespace dingosdk
