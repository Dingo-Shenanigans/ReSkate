#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::game_states {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The skater's game states as the trainer reads and holds them (Extension/Trainer/trainer_gamestate.cpp).
// A game state is an asset object; its value is in one of the skater's state pages, which hang off
// its AntDrivenComponent (the same vtable as puppet_cost::ant_driven_vtable) in a block of pages
// known by its vtable.
inline constexpr std::uintptr_t ant_driven_vtable = 0x63aaee8, states_block_vtable = 0x61a8bd8;

// Hooked functions.
// The lookup of a state's value: (state object, context with the pages) -> where the value is, 0
// when the context has none. All of it is 0x79 bytes.
inline constexpr std::uintptr_t state_lookup = 0x11dcdd0;
inline constexpr std::array<unsigned char, 14> state_lookup_prefix{
    0x48,0x83,0xec,0x28,0x80,0x79,0x2b,0x00,0x4c,0x8b,0xd2,0x4c,0x8b,0xc9};
// The setter of a number state (a FloatAsset's virtual, `self` = object + 0x60, the value in the
// third argument): it finds the value with state_write_lookup and stores it.
inline constexpr std::uintptr_t float_setter = 0x11dd7d0;
inline constexpr std::array<unsigned char, 14> float_setter_prefix{
    0x48,0x83,0xec,0x38,0x48,0x8b,0xc2,0x41,0xb0,0x01,0x48,0x8d,0x51,0xa0};
// The lookup of a state's value for writing: (context, object, create) -> where to write.
inline constexpr std::uintptr_t state_write_lookup = 0x11dcff0;
inline constexpr std::array<unsigned char, 15> state_write_lookup_prefix{
    0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57};
// The copy of one state into another (destination object and context, source context, source
// object or 0 for the destination's own): it finds both values and has the destination's type
// copy them. 0x11d9a7b is its call of state_write_lookup.
inline constexpr std::uintptr_t state_copy = 0x11d9a40;
inline constexpr std::array<unsigned char, 15> state_copy_prefix{
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x28,0x4d,0x85,0xc9,0x48,0x8b,0xd9};
// A rate controller's rate for this tick (float (controller*)): its time scale clamped to the min
// and max it copied from its asset. The per-tick update of each controller (0x10de730) asks it.
inline constexpr std::uintptr_t rate_compute = 0x10dd2f0;
inline constexpr std::array<unsigned char, 13> rate_compute_prefix{
    0x40,0x53,0x48,0x83,0xec,0x30,0x83,0xb9,0xac,0x00,0x00,0x00,0x00};
}
