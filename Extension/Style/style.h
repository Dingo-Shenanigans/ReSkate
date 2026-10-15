#pragma once
#include "style_pose.h"
#include <cstdint>

namespace dingosdk::style {
// Game thread.
void on_client_tick(std::uintptr_t base, std::uintptr_t client, bool multiplayer_ready) noexcept;
[[nodiscard]] StyleModel model();
// The style editor's entry points (style_editor_entry.cpp).
// Gives the overlay the editor's playhead, and the debug camera the editor screen.
void start() noexcept;
// Game thread, once a client tick, after the layer's tick.
void on_editor_tick(std::uintptr_t base, std::uintptr_t client, bool multiplayer_ready) noexcept;
void fill_editor(StyleModel &model);
} // namespace dingosdk::style
