#pragma once
#include <cstdint>

// The most reported crash of 2.1.1 (Sentry, 2026-10-21: Skate.exe+0x3e897fb, a third of every
// report): a render job resolves a texture handle a material parameter still holds after the
// texture went, and the game reads through the null its redirect lookup gives back
// (Engine/Game/Build/<build>/texture_handles.h). The guard gives that lookup an empty holder
// instead, so the job takes the game's own default texture for the id, the way it does for an id
// whose texture is simply gone; what it draws may show the default texture where the game would
// have closed.
namespace dingosdk::texture_handle_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many times a handle had no texture and no redirect since the game started.
std::uint64_t stale() noexcept;
}
