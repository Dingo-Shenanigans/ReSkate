#pragma once
#include "style_pose.h"
#include <cstdint>

namespace dingosdk::style {
// Game thread.
void on_client_tick(std::uintptr_t base, std::uintptr_t client, bool multiplayer_ready) noexcept;
[[nodiscard]] StyleModel model();
} // namespace dingosdk::style
