#pragma once
#include <cstdint>

// The most reported crash of 2.1.1 (Sentry, 2026-10-21: Skate.exe+0x3e897fb, a third of every
// report): a render job resolves a texture handle a material parameter still holds after the
// texture went, and the game reads through the null its redirect lookup gives back
// (Engine/Game/Build/<build>/texture_handles.h). The guard gives that lookup an empty holder
// instead, so resolve answers no texture, and has the texture view cache hand back the empty view
// it builds for no texture rather than ask the missing texture for its key (Skate.exe+0x3e6ddc1,
// where 2.1.2's first guard only moved the crash). The material draws without that texture where
// the game would have closed.
//
// What still crashed in 2.1.3 at the same sites was a holder that is not one: an id whose entry is
// free or never held a texture. Resolve is hooked too and checks that what it would read through is
// a texture (its vtable in the game's image) before the game calls it, and answers none otherwise.
// And a texture parameter value of a kind the game has no handler for (a broken shader preset's,
// Skate.exe+0x3f87028) is left empty, the type's own default, instead of crashing its copy.
namespace dingosdk::texture_handle_guard {
// Checks this build's functions and hooks them; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many times a handle had no texture and no redirect since the game started.
std::uint64_t stale() noexcept;
}
