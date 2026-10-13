#pragma once
#include <cstdint>

namespace dingosdk::game::build::v20260929::grind_exit {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// Grind exits as measured with a logging-only recorder of physics steps, pad and launch calls
// (not part of this change) on 2026-10-11: 48 pops out of grinds, rails and a ledge, a goofy
// skater riding normal.
//
// The physics states of a grind are the 400s: 401 the grind itself (all but a few steps of every
// grind), 403 at a rail's end and between grinds, 402 and 404 briefly. A pop out of a grind goes
// from 401 straight to 200, the same pop state an ollie enters from its crouch (103).
inline constexpr std::uint32_t grind_states_first = 400, grind_states_end = 500;
inline constexpr std::uint32_t pop_state = 200;
//
// Every one of the 48 pops called the jump-trajectory setter (Extension/Trainer/trainer_jump.cpp,
// set_trajectory 0x4752aa0) from this call site, about 1 ms before the selector left 401. It is
// the call no complies launch through as well (from 103); flat-ground ollies launch from another
// (0x4824f7f). The trainer verifies its bytes (e8 55 34 f2 ff) before it hooks anything.
inline constexpr std::uintptr_t grind_pop_call = 0x482f646;
//
// The launch velocity it passes (parameters + 0x80): along the rail about 0.93 x the grind speed;
// up a fixed 2.03; across the rail one of three values the game chose before the call: about
// +1.8 (toward the skater root's right axis), about -1.5 (the other way), or about 0 (no lean).
// The side follows the left stick's x held up to the pop (let go 0.05 s or more before the pop:
// straight); a lean takes about 0.2 s to count in full.
inline constexpr float measured_side_speed = 1.8f;
//
// Measured 2026-10-11 (Stage 1): the lean exit starts in the grind's last steps, before the pop:
// the left stick's x held as little as 0.05 s before the pop already slides the board off the
// rail toward it, and the launch then carries about 1.4 m/s that way. Pushed only at the pop (or
// after), the stick still turns the body (FS/BS 180s of 130-155 degrees) without any lean exit.
//
// Which foot leads (for toe/heel): context + 0x13c4, bit 5 (mask 0x20). A goofy skater: 0 riding
// normal or fakie, 1 riding switch; it changed exactly at each change of stance in a scan of the
// skater's state memory, never in a grind, a pop or a 180 (which lands fakie), and
// +0x13cf bit 2 and +0x1807 bit 7 follow it. Whether it means "switch" or "left foot leads" is
// the same thing for a goofy skater; a regular skater's scan would tell them apart. The grind exit
// control takes it as "switch" and the player's stance as a setting.
inline constexpr std::uintptr_t stance_flags_offset = 0x13c4; // context
inline constexpr std::uint8_t stance_switch_mask = 0x20;
}
