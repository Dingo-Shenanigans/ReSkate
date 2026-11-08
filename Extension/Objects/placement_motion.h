#pragma once
#include "Engine/Game/Build/20260929/placement_physics.h"

// Which motion types reskate.objects.physics accepts on a placement's two copies, and what a
// released server copy goes back to (placement_physics.cpp). Kept apart so the rules are tested
// without the game.
//
// The client replica is the body a claim drives, so it must stand fixed: then its layout pose is the
// saved one and nothing else moves it. The server copy is only mirrored, keyframed, by server_tick.
// It used to be required to be fixed as well, read from the plugin's thread at claim time. In game
// (v2.1.4-2, offline) that refused every claim with "only fixed local placement bodies may be
// claimed", even on a freshly placed stock Build Kit prop. Its motion is now taken as server_tick finds
// it, on the thread that owns it: fixed or keyframed copies are mirrored and put back as they were;
// for a dynamic one, which the game simulates itself, the claim is let go (placement_physics.cpp).
// A copy without a body was let go too, which made every claim in solo play snap back after a second;
// it is now mirrored by transform (bodiless, below).
namespace dingosdk::placement_motion {
namespace abi = game::build::v20260929::placement_physics;
// Not seen yet: server_tick has not read the server copy, so nothing was written to it.
inline constexpr int unseen = 0;
// The server copy is a live placement but carries no physics body that can be reached: in solo play
// that is what server_tick finds for Build Kit props. It follows the client replica by its transform
// alone, as Dynamic QuickDrop 0.3 moved it, and goes back to its layout pose the same way.
inline constexpr int bodiless = -1;

// One of the game's motion types, which may be written to a body (never unseen or bodiless).
constexpr bool body_motion(int motion) noexcept {
    return motion == abi::motion_fixed || motion == abi::motion_keyframed || motion == abi::motion_dynamic;
}
constexpr bool client_claimable(int motion) noexcept { return motion == abi::motion_fixed; }
constexpr bool server_mirrorable(int motion) noexcept {
    return motion == abi::motion_fixed || motion == abi::motion_keyframed || motion == bodiless;
}
// The motion a released server copy is put back in, or `unseen` when it must not be written: it was
// never read (so never mirrored) or the game drives it itself.
constexpr int server_restore(int seen) noexcept { return server_mirrorable(seen) ? seen : unseen; }
// A claim made before the previous claim's restore ran finds the copy as that claim left it
// (keyframed), so it takes over the motion still owed to the copy instead of reading it again.
constexpr int inherit(int current, int owed) noexcept { return current != unseen ? current : owed; }
} // namespace dingosdk::placement_motion
