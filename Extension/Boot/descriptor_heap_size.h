#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// Each frame's share of the renderer's shader-visible descriptor heap is capped at 131,072
// descriptors, in a heap of 333,333. A dense level draws more than that in a frame (the Los Santos
// City sublevels with more than about 65,000 mesh sections), and the renderer then writes past
// the end of the heap: shaders read descriptors that are not there and fault or hang the GPU.
// Each heap is allowed D3D12's 1,000,000 instead, and a frame's share is capped at this, which
// makes each frame's heap 644,288 descriptors.
inline constexpr std::uint32_t heap_descriptor_total = 1'000'000;
inline constexpr std::uint32_t frame_descriptor_cap = 524'288;

// Installs before executable entry, while the game is suspended.
bool start_descriptor_heap_size(std::uintptr_t base, std::string& error);
} // namespace dingosdk
