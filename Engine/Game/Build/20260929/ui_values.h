#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::ui_values {
// The asset loader publishes a symbolic alias separately from the typed model
// handle. The insertion routine holds no lock; callers hold ModelWriteLock.
inline constexpr std::uintptr_t alias_insert = 0x18d1320;
inline constexpr std::array<unsigned char, 32> alias_insert_prefix{
    0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x56,
    0x41,0x57,0x48,0x83,0xec,0x30,0x49,0x8b,0x39,0x48,0x8b,0xf2,0x44,0x8b,0x51,0x08};
inline constexpr std::uintptr_t models_offset = 0x140;
inline constexpr std::uintptr_t aliases_offset = 0x588;
inline constexpr std::uintptr_t alias_value_offset = 8;
}
