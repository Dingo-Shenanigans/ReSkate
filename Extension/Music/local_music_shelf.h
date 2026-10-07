#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

namespace dingosdk::profile_runtime {
inline constexpr game::build::Fingerprint music_model_construct_contract{0x1912670,
    {0x4c,0x89,0x44,0x24,0x18,0x55,0x56,0x41,0x55,0x41,0x56,0x48,0x8d,0x6c,0x24,0xd1,
     0x48,0x81,0xec,0xf8,0x00,0x00,0x00,0x4d,0x8b,0xf1,0x44,0x0f,0xb6,0xea,0x48,0x8b}};
using MusicModelConstruct = std::uint64_t (*)(std::uintptr_t, std::uint8_t, std::uint64_t,
    std::uintptr_t, bool, std::uintptr_t);
extern MusicModelConstruct music_model_construct_original;
std::uint64_t music_model_construct_hook(std::uintptr_t manager, std::uint8_t mode,
    std::uint64_t id, std::uintptr_t type, bool flag, std::uintptr_t record);
void update_music_shelf();
void music_shelf_before_level_transition(unsigned next) noexcept;
} // namespace dingosdk::profile_runtime
