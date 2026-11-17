#pragma once
#include "Engine/Game/Build/fingerprint.h"

namespace dingosdk::game::build::v20260929::offboard_flight {
// Supported executable. Offboard owns these embedded substates
// (constructor RVA 480fd40), selected through +48 and updated through vtable+18.
// Ground/Slide add context+910 to velocity XYZ at +10; Falling adds gravity*dt
// to the same velocity. The motion target override alone cannot stop that sum.
// Constructor caller: RDX=core+3c0 (context), R8=core+430 (board wrapper),
// R9=core+438 (rig wrapper). Constructor stores R8 at parent+10, R9 at +18.
inline constexpr std::array<Fingerprint, 2> offboard_owner_contracts{{
    {0x47bf916, {
        0x4c,0x8b,0x8b,0x38,0x04,0x00,0x00,0x48,0x8d,0x4c,0x24,0x50,0x4c,0x8b,0x83,0x30,
        0x04,0x00,0x00,0x48,0x8b,0x93,0xc0,0x03,0x00,0x00,0x48,0x89,0x4c,0x24,0x20,0x48}},
    {0x48108e9, {
        0x4c,0x89,0x41,0x10,0x4c,0x89,0x49,0x18,0x48,0x8b,0x42,0x08,0x48,0x89,0x41,0x20,
        0x48,0x8b,0x02,0x48,0x89,0x41,0x28,0x48,0x8b,0x42,0x10,0x48,0x89,0x41,0x30,0x48}},
}};
struct OffboardFlightState {
    std::uintptr_t offset, vtable_rva, update_rva;
    Fingerprint velocity_write;
};
inline constexpr std::array<OffboardFlightState, 3> offboard_flight_states{{
    // FollowGround
    {0x60, 0x65e8c20, 0x48318a0, {0x4832134, {
        0x48,0x8b,0x43,0x08,0xc5,0xf8,0x10,0x80,0x10,0x09,0x00,0x00,0xc5,0xf8,0x58,0x4f,
        0x10,0xc5,0xf8,0x11,0x4f,0x10,0x48,0x89,0xb3,0x48,0x0d,0x00,0x00,0xc7,0x83,0x50}}},
    // FollowGroundSlide
    {0x1c0, 0x65e8c80, 0x4832530, {0x4832a6b, {
        0x43,0x08,0xc5,0xf8,0x10,0x80,0x10,0x09,0x00,0x00,0xc5,0xf8,0x58,0x4f,0x10,0xc5,
        0xf8,0x11,0x4f,0x10,0x48,0x89,0xb3,0x48,0x0d,0x00,0x00,0xc7,0x83,0x50,0x0d,0x00}}},
    // Falling
    {0x5b0, 0x65e8da0, 0x4820860, {0x4820c6c, {
        0xc5,0xf0,0x59,0xd0,0xc5,0xfa,0x10,0x0e,0xc4,0xe3,0x71,0x21,0x4f,0x14,0x10,0xc4,
        0xe3,0x71,0x21,0x4f,0x18,0x20,0xc5,0xe8,0x58,0xc1,0xc5,0xf8,0x11,0x06,0x4c,0x8b}}},
}};
inline constexpr std::uintptr_t offboard_flight_vtable = 0x65e8f40;
// FollowRagdoll / FollowAnimatedRagdoll / FollowSimulatedRagdoll also keep
// world velocity at +10. Their shared update derives it from displacement
// (RVA 48380DE), including Y at +14. Boosts must update this cached velocity
// as well as the skeleton bodies when diving or gliding.
inline constexpr std::array<std::array<std::uintptr_t, 3>, 3> offboard_ragdoll_states{{
    {0x720, 0x65e8df8, 0x4832c80},
    {0x8e0, 0x65e8e60, 0x4830a70},
    {0xaa0, 0x65e8ed0, 0x48332f0},
}};
inline constexpr Fingerprint ragdoll_velocity_write{0x48380de, {
    0xc4,0xe2,0x79,0x18,0x80,0xec,0x17,0x00,0x00,0xc5,0xe8,0x5e,0xc8,0xc5,0xf8,0x11,
    0x4f,0x10,0xc5,0xf8,0x10,0x43,0x30,0xc5,0xf8,0x11,0x87,0x60,0x01,0x00,0x00,0x4c}};
// How Offboard comes to its ragdoll states (found 2026-11-16 in the decompile, looking for why a
// wipeout asked for on foot did nothing). Offboard's update (vtable slot 2, 4822b80) first takes
// up the state asked for at +50 when it is not the one at +48, then runs the state's own update.
// FollowGround's update (48318a0) reads the context's word at +9c0 every step: bit 13 clear
// means the body is not on its feet, and it calls 481eb50, which asks for FollowRagdoll or, by
// bits 28 and 19 of the same word, one of its two kin. That word is copied into the context
// each step from the animation's side (0x10 of its input), so a ragdoll on foot begins with the
// animation being told to bail (no_bail.h: the requests at context +13c4 bit 15 and +13d4 bit 27),
// not with Offboard: asked for FollowRagdoll directly while bit 13 is still set, Offboard is back
// in FollowGround a step later (tried in game: the skater stood).
inline constexpr std::uintptr_t core_offboard_offset = 0x3b0; // core: the state object, Offboard while off the board
inline constexpr std::uintptr_t core_context_offset = 0x3c0;  // core: its context, which Offboard keeps at +8
inline constexpr std::uintptr_t core_selector_offset = 0x440; // core: its physics state selector
inline constexpr std::uintptr_t state_now_offset = 0x48;      // Offboard: the state it is in, a pointer into itself
inline constexpr std::uintptr_t mode_flags_offset = 0x9c0;    // context: the word FollowGround reads (bit 13: on its feet)
// What the animation was last told (the publisher, 47da450, writes both each step): the block at
// core+3b8 -> +38 has the explicit bail at +87 (context +13c4 bit 15) and the independent one at +8d.
inline constexpr std::uintptr_t core_animation_offset = 0x3b8;
// The skydive (2026-11-16: falls on foot in game, the context's request words and the skater's
// named animation values logged every step). The game calls its air tricks on foot flumps
// (EnableFlumping, DingoTrickCategory_FlumpAirPose). A skater that walks off a roof is in Falling
// (+5b0) and stays in its plain fall until the player makes one. When one begins, the animation's
// flags (context +9c0) come back with bit 13 clear and bits 28 and 19 set, on which Falling's
// update (4820860) asks for FollowSimulatedRagdoll (+aa0): the dive. With it the animation gives
// the game Bool.Physics.Offboard.ForceFollowRagdoll true and Enum.GS.Current.FlumpTrickType 7 (1 in
// a plain fall); both stay so until the body touches the ground, where the game makes its own
// wipeout. Those two are results: written from outside they do not begin a dive (no_bail.cpp).
inline constexpr std::uintptr_t falling_state_offset = 0x5b0; // Offboard: Falling, the first state that is not on its feet
inline constexpr std::uintptr_t animation_block_offset = 0x38;
inline constexpr std::uintptr_t told_impact_offset = 0x87;
inline constexpr std::uintptr_t told_bail_offset = 0x8d;
// Seen in game (2026-11-16, 22:20): told both of those, the animation of a skater on foot does
// not bail. The request it has on foot is the player's own bail (the dive): context +13d8 bit 9,
// which 1005200 copies each step from the player's action, the selector answers with Wipeout and
// the publisher hands to the animation at +a0.
inline constexpr std::uintptr_t manual_bail_offset = 0x13d8;
inline constexpr long manual_bail_mask = 0x200;
inline constexpr std::uintptr_t told_manual_offset = 0xa0;
// Still standing with all three told (2026-11-16, 22:35). What the player's own ragdoll (LB + RB +
// L3 + R3, on foot too) does is in 1005200, which copies the player's actions into the context
// each step: the action sets context +13c4 bit 15, and with that bit set the same function clears
// bit 13 of +9c0, the on-its-feet flag every Offboard state reads. It is that flag, not what the
// animation is told, that takes Offboard to its ragdoll states (481eb50) and keeps it there; and
// the bit raised from outside comes too late for that test, the action having overwritten it a
// few lines before. So the flag is cleared where it is read: at the head of Offboard's own update
// (its vtable's slot 2, 4822b80: it takes up the state asked for, then runs the state's update).
inline constexpr std::uintptr_t on_feet_mask = 0x2000;
// Seen with the flag cleared for half a second at an ask (2026-11-16, 23:06, Zee: "it starts like
// a fall sequence but gets stuck in it"): Offboard did leave its feet, and the game counted the
// body down for five seconds after each ask. Which ragdoll state 481eb50 asks for is in the same
// word: bit 28 clear, FollowRagdoll; set (it is, on foot: 0x1100e600), FollowAnimatedRagdoll, a
// fall the animation plays; set with bit 19, FollowSimulatedRagdoll, the limp one. A skater on its
// feet has 28 without 19, so the fall was the animated one, and half a second on it was back on
// its feet's state with the fall half played. So bit 19 is raised with bit 13 cleared, and both
// are held for as long as the asks keep coming, the bail itself told only as the hold begins.
inline constexpr std::uintptr_t simulated_ragdoll_mask = 0x80000;
inline constexpr Fingerprint offboard_update_contract{0x4822b80, {
    0x40,0x57,0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x8b,0x05,0x30,0x18,0x9a,0x02,
    0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0xc0,0x00,0x00,0x00,0x48,0x8b,0xf9,0x48,0x8b}};
}
