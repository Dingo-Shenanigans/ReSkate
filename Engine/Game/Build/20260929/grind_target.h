#pragma once
#include <array>
#include <cstdint>
namespace dingosdk::game::build::v20260929::grind_target {
// Ordinary airborne rail candidate selection. Native geometry and acceptance tests still run.
inline constexpr std::uintptr_t select = 0x47fc590;
inline constexpr std::array<std::uint8_t,16> select_prefix{0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0x81,0x98,0,0,0,0x48,0x8b,0xd9};
inline constexpr std::uintptr_t context = 0x98, request_gate = 0xaec, alignment = 0xb10;
inline constexpr std::uintptr_t requested = 0x17d0, request_flags = 0x13dc;
inline constexpr std::uint32_t requested_mask = 0x20000;
inline constexpr std::uintptr_t root = 0x3b8, target = 0x10, targeting = 0x1fb, direction = 0xc0;
}
