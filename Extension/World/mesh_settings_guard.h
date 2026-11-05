#pragma once
#include <cstdint>

// Leaving a level crashed the game in its allocator (Skate.exe+0x3d9ed9d, +0x3d9ee43, +0x3d9a856)
// as it freed a string of each unloading mesh (Engine/Game/Build/<build>/mesh_settings.h). The
// string belongs to the settings object the game builds inside the mesh; a mesh whose asset has no
// settings object is marked as having one anyway, and its string is whatever that memory held
// before (often a pointer freed long ago). The guard gives such a mesh the game's empty string
// first, so its unload frees nothing.
namespace dingosdk::mesh_settings_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
}
