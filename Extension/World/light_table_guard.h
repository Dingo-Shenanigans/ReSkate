#pragma once
#include <cstdint>

// The game keeps each kind of light (PbrSpotLight, PbrSphereLight, ...) in a table of 8,192 and
// crashes when a level makes more (Engine/Game/Build/<build>/light_tables.h). Once a kind's table is
// nearly full, the guard has the game make no more lights of that kind: they are not drawn, where
// the game would have crashed.
namespace dingosdk::light_table_guard {
// Checks this build's functions and hooks the six light factories; once. False when the build
// differs (no guard).
bool install(std::uintptr_t base) noexcept;
}
