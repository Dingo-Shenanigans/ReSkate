#pragma once
#include "Extension/Multiplayer/Net/effects.h"
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
// Skater effects on other players' skaters (native_vfx.cpp). Client thread unless said.
// Installs the hooks once; safe to call again.
void prepare_effects(std::uintptr_t base) noexcept;
// The local game's contacts since the last call, oldest first: the newest max_impacts.
std::vector<Impact> drain_impacts();
// For the current PeerScope slot's skater: the game's effect for one of that player's contacts.
void play_impact(const Impact &impact) noexcept;
// For the current PeerScope slot's skater, after its outfit changed: the effects its costume
// and skateboard come with (trails, fire) are built again, as the game built them before the
// outfit was on (tick_remote_effects asks for it, every frame the player is shown).
void note_remote_outfit(std::uint64_t now) noexcept;
void tick_remote_effects(std::uint64_t now) noexcept;
void reset_effects() noexcept;
// One line for the console: what has been captured and played, and what is in the way.
std::string native_effects_status();
} // namespace dingosdk::multiplayer
