#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstddef>
#include <cstdint>

// The skater graph's named game-state values ("Animation/Dingo/Float.Intent.JumpStrength"...). The gesture graph
// writes them through these setters, on job worker threads, as a trick starts. Found 2026-10-10 with a hardware
// write watch (ReSkate research notes, "Tricks by command: the gesture graph's intents").
namespace dingosdk::game::build::v20260929::game_state_values {
// (setter, instance, value): `setter` is the value's descriptor + descriptor_to_setter, `instance` the graph
// instance that owns the storage.
inline constexpr Fingerprint float_setter{0x11dd7d0, {0x48,0x83,0xec,0x38,0x48,0x8b,0xc2,0x41,0xb0,0x01,0x48,0x8d,0x51,0xa0,0x48,0x8b,
                                                      0xc8,0xc5,0xf8,0x29,0x74,0x24,0x20,0xc5,0xf8,0x28,0xf2,0xe8,0x00,0xf8,0xff,0xff}};
inline constexpr std::uintptr_t descriptor_to_setter = 0x60;
// The same for the graph's int and enum values: (setter, instance, value).
inline constexpr Fingerprint int_setter{0x1137350, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0xf7,0x41,0x20,0xfb,0xff,0xff,0xff,0x48,0x8b,
                                                    0xc2,0x48,0x8d,0x51,0xa0,0x41,0x8b,0xd8,0x74,0x0d,0x41,0xb0,0x01,0x48,0x8b,0xc8}};
// The descriptor's name: a char*.
inline constexpr std::size_t descriptor_name = 0x18;
// The pop's strength the gesture graph sets for a trick: 1 for a quick flick; the animation copies it to
// Float.Anim.JumpStrength. Forced to 0.5 the pop is 0.28 m, to 1.5 it is 0.48-0.50 m (standing, 1.0 gives 0.38 m).
inline constexpr char jump_strength_name[] = "Animation/Dingo/Float.Intent.JumpStrength";
// The flip's speed the gesture graph sets for a trick: a kickflip's quick flick 1.075, a 120 ms flick 0.552, an ollie
// about 2. Forced to 0.55 a kickflip turns 1015 deg/s, to 2.0 2746 deg/s (1438 at 1.075).
inline constexpr char flip_speed_name[] = "Animation/Dingo/Float.Intent.TrickFlipSpeed";
// The game's own grade of a pop, written once as the skater pops: 1 low, 2 medium, 3 high. Measured 2026-10-19 on a
// standing kickflip: strength forced to 0.2-0.4 of its own gave 1, 0.5-0.6 gave 2, 0.7 and up 3.
inline constexpr char pop_height_name[] = "Animation/Dingo/EnumGS.PopHeight";
} // namespace dingosdk::game::build::v20260929::game_state_values
