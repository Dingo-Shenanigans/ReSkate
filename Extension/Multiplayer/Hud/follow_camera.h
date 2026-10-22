#pragma once
#include "Engine/Game/UI/game_view.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>

// The spectate camera (party Spectate and throwdown turns). It frames another skater the way
// the game's own camera frames the local one: while the player skates, the game camera's
// distance behind the skater, height, pitch and FOV are measured and averaged, and a
// spectated skater gets the same, behind their direction of travel.
namespace dingosdk::multiplayer {
// Client thread, every tick: where the local skater stands.
void note_local_skater(const Transform &root) noexcept;
// Client thread, once per tick after the tick's camera was read (Engine/Game/UI/game_view.h).
// That read holds the camera the game placed after the previous tick, so it is measured
// against the local skater of the previous tick. Only the game's own camera is measured.
void observe_gameplay_camera(std::uintptr_t base, const GameView &view) noexcept;
// The camera for spectating `id` standing at `root` this frame (world matrix rows: right, up,
// back, position), and the FOV to give it.
std::array<float, 16> follow_camera(std::uint64_t id, const Transform &root, float &fov) noexcept;
// The local skater as note_local_skater last saw them, if that was within `max_age_ms`.
bool latest_local_skater(Transform &root, unsigned max_age_ms = 250) noexcept;
} // namespace dingosdk::multiplayer
