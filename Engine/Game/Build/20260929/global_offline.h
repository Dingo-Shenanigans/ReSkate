#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::global_offline {
// Shared OnlineEnabled leaf hooked by the offline boot path (Extension/Boot/offline_boot.cpp).
inline constexpr std::uintptr_t online_enabled = 0x3b8830;
// MOV AL,1; RET followed by the inspected INT3 padding.
inline constexpr std::array<unsigned char, 16> online_enabled_prefix{
    0xb0,0x01,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
// Return addresses of the direct OnlineEnabled calls the offline boot path answers.
inline constexpr std::uintptr_t backend_predicate_return = 0x2830c4;
inline constexpr std::uintptr_t async_prepare_character_return = 0x7fe2f2;
inline constexpr std::uintptr_t character_readiness_return = 0x8070d5;
// Return address inside the reflected Flow getIsOnlineEnabled invocation adapter.
inline constexpr std::uintptr_t flow_adapter_return = 0x58e2abe;
// Post-spawn progression guard.
inline constexpr std::uintptr_t post_spawn_progression_return = 0x82ab99;
}
