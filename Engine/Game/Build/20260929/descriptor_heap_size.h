// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// The size of the renderer's shader-visible CBV/SRV/UAV descriptor heaps: a persistent part
// (120,000) and, in each frame's heap, a part for that frame's descriptors: what is left of a
// total of 333,333 (a third of D3D12's 1,000,000), capped at 131,072.
#pragma once

#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::descriptor_heap_size {
// In the renderer's device setup (0x0410e3f0), where the total each heap may have (333,333, from
// the device or from the platform's own description, PQXD; both paths join here, at the first
// instruction) is capped at 1,000,000 unless the device's resource binding is tier 3:
//   cmp dword ptr [rsi + 0x7a4], 3
//   je +0x0a                           ; past the cap
//   mov edx, 1000000 / cmp ecx, edx / cmova ecx, edx   (left as they are)
inline constexpr std::uintptr_t total_cap_rva = 0x0410eaa0;
inline constexpr std::array<std::uint8_t, 9> total_cap{0x83, 0xbe, 0xa4, 0x07, 0x00, 0x00, 0x03, 0x74, 0x0a};
// In the renderer's device setup (0x0410e3f0), the per-frame part: what the device allows past
// the persistent part, divided among the heaps, then capped by the setting at desc+8
// (0xffffffff: no cap):
//   mov ecx, dword ptr [r14 + 8]       ; the cap
//   div r9d                            ; eax = room / heaps
//   mov edi, eax
//   mov eax, 0xffffffff
//   cmp ecx, eax / je / cmp edi, ecx / cmovb ecx, edi / mov edi, ecx   (left as they are)
// No branch lands inside these four instructions.
inline constexpr std::uintptr_t cap_load_rva = 0x0410eaee;
inline constexpr std::array<std::uint8_t, 14> cap_load{
    0x41, 0x8b, 0x4e, 0x08, 0x41, 0xf7, 0xf1, 0x8b, 0xf8, 0xb8, 0xff, 0xff, 0xff, 0xff};
// Layout choice: one heap with a region per frame (sil = 0) or a heap per frame (sil = 1, PC).
inline constexpr std::uintptr_t layout_rva = 0x0410eabb;
inline constexpr std::array<std::uint8_t, 8> layout{0x48, 0x85, 0xc0, 0x74, 0x1f, 0x0f, 0xb6, 0x40};
// Heap object constructor. Per-heap EASTL fixed vectors (4 inline): heaps +0x08, CPU starts +0x50,
// GPU starts +0x98. +0xf4: frame regions per heap.
inline constexpr std::uintptr_t heap_constructor_rva = 0x040e1f00;
inline constexpr std::array<std::uint8_t, 15> heap_constructor{
    0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08};
inline constexpr std::size_t heap_vectors[]{0x08, 0x50, 0x98};
inline constexpr std::size_t heap_regions = 0xf4;
// Heap object's begin-frame (vtable slot 6): sets the frame's start (+0x100) and cursor (+0x108),
// then moves to the next region. +0xf0: descriptors per frame region.
inline constexpr std::uintptr_t begin_frame_rva = 0x040e2520;
inline constexpr std::array<std::uint8_t, 14> begin_frame{
    0x44, 0x8b, 0x81, 0xf0, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xc9, 0x44, 0x0f, 0xaf, 0x81};
inline constexpr std::size_t heap_per_frame = 0xf0;
inline constexpr std::size_t heap_frame_start = 0x100;
inline constexpr std::size_t heap_cursor = 0x108;
} // namespace dingosdk::game::build::v20260929::descriptor_heap_size
