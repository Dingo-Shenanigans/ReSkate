#pragma once
#include <cstdint>

namespace dingosdk::game::build::v20260929::launches {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The local skater's launches into the air (Extension/Skater/launches.h), as measured with a
// logging-only recorder of physics steps, pad and launch calls on 2026-10-11: 48 pops out of
// grinds, rails and a ledge, a goofy skater riding normal; and the trainer's jump measurements
// (Extension/Trainer/trainer_jump.cpp).
//
// The physics states of a grind are the 400s: 401 the grind itself (all but a few steps of every
// grind), 403 at a rail's end and between grinds, 402 and 404 briefly. A pop out of a grind goes
// from 401 straight to 200, the same pop state an ollie enters from its crouch (103).
inline constexpr std::uint32_t grind_states_first = 400, grind_states_end = 500;
// 701 is a grind as well, one the 48 never reached (2026-10-21, grinds in a park): the selector
// pops out of it to 200 through the same call, and the game's own checks take "previous state 400
// or state 701" together in about a dozen places.
inline constexpr std::uint32_t other_grind_state = 701;
inline constexpr bool is_grind_state(std::uint32_t state) noexcept {
    return (state >= grind_states_first && state < grind_states_end) || state == other_grind_state;
}
inline constexpr std::uint32_t pop_state = 200;
//
// Every launch calls the jump-trajectory setter (Extension/Trainer/trainer_jump.cpp,
// set_trajectory 0x4752aa0), from a call site of its kind. Every one of the 48 pops out of a grind
// called it from grind_pop_call, about 1 ms before the selector left 401. It is the call no
// complies launch through as well (from the crouch, 103); flat-ground ollies (riding or out of a
// manual) launch from ollie_call, a boneless from boneless_call while the foot is planted (602).
// The trainer verifies their bytes before it hooks anything.
inline constexpr std::uintptr_t grind_pop_call = 0x482f646, ollie_call = 0x4824f7f, boneless_call = 0x47691f8;
//
// The launch velocity it passes (parameters + 0x80): out of a grind, along the rail about 0.93 x
// the grind speed; up a fixed 2.03; across the rail one of three values the game chose before the
// call: about +1.8 (toward the skater root's right axis), about -1.5 (the other way), or about 0
// (no lean). The side follows the left stick's x held up to the pop (let go 0.05 s or more before
// the pop: straight); a lean takes about 0.2 s to count in full.
//
// Measured 2026-10-11 (Stage 1): the lean exit starts in the grind's last steps, before the pop:
// the left stick's x held as little as 0.05 s before the pop already slides the board off the
// rail toward it, and the launch then carries about 1.4 m/s that way. Pushed only at the pop (or
// after), the stick still turns the body (FS/BS 180s of 130-155 degrees) without any lean exit.
//
// Which foot leads: context + 0x13c4, bit 5 (mask 0x20). A goofy skater: 0 riding normal or
// fakie, 1 riding switch; it changed exactly at each change of stance in a scan of the skater's
// state memory, never in a grind, a pop or a 180 (which lands fakie), and +0x13cf bit 2 and
// +0x1807 bit 7 follow it. Whether it means "switch" or "left foot leads" is the same thing for a
// goofy skater; a regular skater's scan would tell them apart. Plugins get it as "switch".
inline constexpr std::uintptr_t stance_flags_offset = 0x13c4; // context
inline constexpr std::uint8_t stance_switch_mask = 0x20;
}
