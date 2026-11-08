// The motion rules of reskate.objects.physics on a placement's two copies (placement_motion.h):
// which copies may be claimed or mirrored, and what a released server copy is put back in.
#include "Extension/Objects/placement_motion.h"
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    using namespace dingosdk::placement_motion;
    constexpr int fixed = abi::motion_fixed, keyframed = abi::motion_keyframed, dynamic = abi::motion_dynamic;

    check(client_claimable(fixed), "a fixed client replica may be claimed");
    check(!client_claimable(keyframed), "a keyframed client replica is moved by something else");
    check(!client_claimable(dynamic), "a dynamic client replica is simulated already");
    check(!client_claimable(unseen), "an unread client replica is not claimable");

    // The server copy no longer decides whether a claim is made.
    check(server_mirrorable(fixed), "a fixed server copy is mirrored");
    check(server_mirrorable(keyframed), "a keyframed server copy is mirrored");
    check(!server_mirrorable(dynamic), "a dynamic server copy is left to the game");
    check(!server_mirrorable(unseen), "an unread server copy is not written");

    check(server_restore(fixed) == fixed, "a fixed copy goes back fixed");
    check(server_restore(keyframed) == keyframed, "a keyframed copy goes back keyframed, not fixed");
    check(server_restore(dynamic) == unseen, "a copy the claim never wrote is not restored");
    check(server_restore(unseen) == unseen, "a copy never read is not restored");
    check(server_restore(7) == unseen, "an unknown motion is never written back");

    // Claimed again before the old restore ran: the new claim owes the copy what the old one did.
    check(inherit(unseen, fixed) == fixed, "a new claim takes over the motion still owed");
    check(inherit(unseen, unseen) == unseen, "nothing owed leaves the new claim to read the copy");
    check(inherit(keyframed, fixed) == keyframed, "a claim that already read the copy keeps what it read");

    // A server copy without a body (solo play) follows by transform instead of ending the claim.
    check(server_mirrorable(bodiless), "a bodiless server copy is mirrored by its transform");
    check(server_restore(bodiless) == bodiless, "a bodiless copy goes back to its layout pose by transform");
    check(inherit(unseen, bodiless) == bodiless, "a new claim takes over a bodiless copy still owed its restore");
    check(!client_claimable(bodiless), "a client replica without a body is never claimed");
    check(bodiless != unseen, "bodiless is not mistaken for unread");
    check(body_motion(fixed) && body_motion(keyframed) && body_motion(dynamic), "the game's motions may be written to a body");
    check(!body_motion(bodiless) && !body_motion(unseen) && !body_motion(7), "only real motions are written to a body");

    if (failures) return 1;
    std::cout << "placement motion: all checks passed\n";
    return 0;
}
