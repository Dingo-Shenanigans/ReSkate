#pragma once
#include <cstdint>
namespace dingosdk::game::build::v20260929::animation_values {
inline constexpr std::uintptr_t core_position = 0x100, core_forward = 0x150, core_backwards = 0x5e4;
inline constexpr std::uintptr_t frame_source = 0x3c8, frame_flags = 0x10;
inline constexpr std::uintptr_t frame_context = 0x13dc, basis_context = 0x13c4;
inline constexpr std::uint32_t native_mask = 1u << 11, basis_mask = 1u << 13;
inline constexpr std::uintptr_t state_root = 0x3b8, export_state = 0x60, exported_flags = 0xa3;
// Occupied per-rider animation graph properties, build 20260929. The descriptor carries
// its inline name and an encoded byte offset, not an array index. No defaults are used.
inline constexpr std::uintptr_t provider = 0x630, model = 0x610;
inline constexpr std::uintptr_t input = 0x28, output = 0x38, graph_bytes = 0x500;
inline constexpr std::uintptr_t type = 0x20, secondary_type = 0x60, key = 0x28;
inline constexpr std::uintptr_t bool_type = 0x61ab030, bool_secondary = 0x61ab080;
inline constexpr std::uintptr_t enum_type = 0x61926d8, enum_secondary = 0x6192728;
inline constexpr std::uintptr_t float_type = 0x61ab550;
inline constexpr std::uintptr_t bool_name = 0x88, float_name = 0x78;
inline constexpr std::uintptr_t pages = 0x60, occupied = 0x71c, last_value = 0x718;
}
