#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// Cull IDs per spatial transaction queue: one for every entity slot the entity pages hold
// (entity_pages.h), as each object a queue tracks is an entity. 12 bytes an ID per queue.
inline constexpr std::uint32_t spatial_cull_id_target = 262'144;

// Install after Detours hook service init, before any level loads. Raises the game's
// InitialCullIdListSize to the target as each queue makes its list; a larger configured
// value is kept.
bool start_spatial_cull_ids(std::uintptr_t base, std::string& error);
} // namespace dingosdk
