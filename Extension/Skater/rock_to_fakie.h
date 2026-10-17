#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Rock to Fakie: the game has no button for a rock to fakie, but riding up to a coping, stalling
// on it and rolling back in fakie is how one is done, and in skate. that is a Board Stall or a
// Lip Stall left with B. While on:
//   - The trick ticker names those two stalls "Rock to Fakie" while the local skater holds them
//     (selector states 400 / 401). Only the label the ticker asks for on the one call route seen
//     for it (Engine/Game/Build/20260929/rock_to_fakie.h) is changed, in the ticker's own buffer
//     and only when it has room: the string database, other labels and other players' tickers are
//     the game's.
//   - Leaving the stall with the physical B button records the trick as the game's own fakie_rock
//     (id 187) instead of bs_board (9) or bs_lip (12): the recorder is given a changed copy of the
//     packet, never the game's. B must be pressed while the stall is held and the stall left within
//     a moment of it.
// Off by default and kept (profile preference "RockToFakie"). Requires the supported game build
// and No Bail started (it follows the skater's physics state through No Bail's state watch).
// Nothing is written to game memory but that label buffer and the packet copy.
namespace dingosdk::rock_to_fakie {
// Installs the two hooks when the game's code matches; they pass everything on unchanged while off.
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick, with the local client: keeps No Bail's state watch on the local
// skater and samples the physical B button.
void on_client_tick(std::uintptr_t client) noexcept;
// The console verb: rocktofakie on|off|status.
std::string command(const std::vector<std::string>& words);

// For the menus, any thread.
bool available() noexcept;
bool enabled() noexcept;
}
