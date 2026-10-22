#pragma once
#include <cstdint>

// Server-side player spawners.
namespace dingosdk::game::build::v20260929::server_spawn {
inline constexpr std::uintptr_t spawner_vtable = 0x63ba798;
// Spawner list heads, 16 bytes each, indexed by partition + 13 (or + 8 for flagged spawners).
inline constexpr std::uintptr_t spawner_registry = 0x75803b0;
// ServerSpawner::onEvent(self, event), vtable+28. No incoming R8/R9/stack
// arguments; the native dispatcher treats this as void.
inline constexpr std::uintptr_t spawner_event_handler = 0x2ccb6b0;
// ServerPlayer ctor RVA2ea7507 owns this state manager at +290 (+298 aliases
// it). Its ctor RVA2f35cf0 sets owner+3f0 and owns the typed state at +460.
inline constexpr std::uintptr_t player_state_manager_vtable = 0x63f1220;
inline constexpr std::uintptr_t player_state_vtable = 0x63f0fe0;
}
