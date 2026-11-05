#pragma once
#include "developer_hoodie_material.h"

namespace dingosdk {
// Verified client update thread only. Remote identity comes from the Steam roster,
// generation from the current native actor; neither is a client cosmetic parameter.
// Colours what a player wears (developer_hoodie_material.h): `styles` is how they have
// each marked cosmetic, the skater's first.
void update_developer_hoodie(std::uintptr_t base, std::uintptr_t entity, std::uint64_t steam_id,
                             std::uint64_t generation, DeveloperHoodieState &state,
                             const multiplayer::MarkStyles &styles) noexcept;
void tick_local_developer_hoodie(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk
