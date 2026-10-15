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
} // namespace dingosdk::game::build::v20260929::descriptor_heap_size
