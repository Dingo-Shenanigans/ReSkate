#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include <cstdint>

namespace dingosdk::scoring_hud {
using Style = overlay::ScoringHudStyle;

// Starts the read-only scoring model observer. The saved style defaults to the game HUD.
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick: snapshots the live scoring model and owns/restores the stock HUD.
void on_client_tick() noexcept;
// Any thread. set_style applies on the next client tick and persists with the local profile.
Style style() noexcept;
void set_style(Style value) noexcept;
// Presentation thread feed: a detached copy, never game memory.
overlay::ScoringHud view();
}
