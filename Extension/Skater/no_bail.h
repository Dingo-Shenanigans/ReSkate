#pragma once
#include <cstddef>
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// Prepare the local skater's native teleport option after validating its owner.
// This only changes how the next teleport spawns; it does not grant No Bail.
bool prepare_on_board_teleport(std::uintptr_t client, std::uintptr_t entity) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    bool ragdoll_known{}, ragdoll{}; // native offboard substate, distinct from walking
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}, runouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
// The local skater as No Bail follows it (update_no_bail publishes it), for a feature that reads it
// (the plugins' physics steps, grind exits): any thread, its ownership chain resolved again now.
struct NoBailSkater {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
};
bool no_bail_skater(NoBailSkater& skater) noexcept;
// Every physics step of that skater, whether or not No Bail protects it: No Bail's hook on the
// skeleton response runs once per step, right after the step's body contacts, and hands it on after
// its own filtering, so a wipeout it filtered never happened. `seconds` is the step's simulated time.
// One observer, nullptr for none; it runs on the physics thread.
using NoBailStepObserver = void (*)(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept;
// Revert Boost: the local skater's turn over each flight, measured in the physics selector (every
// physics step) rather than the ~9 Hz client tick, so the start and the end of the turn are not lost.
// Publish the selector and the skater's transform address from the client tick; it expires if ticks stop.
struct RevertLanding {
    std::uint64_t sequence{};    // 0 = no landing yet; increases by one per finished flight
    float spin_degrees{};        // signed turn about the vertical axis from takeoff to landing
    float board_spin_degrees{};  // the same for the board's deck (if board_valid)
    float board_offset_degrees{}; // the deck's heading minus the skater's, at touchdown (if board_valid)
    float heading_degrees{};      // where the deck (the skater, without a deck pose) points at touchdown: 0 = +Z, as atan2(x, z) (trainer)
    bool board_valid{};
    std::uint32_t from{}, to{};  // last air state, and the state chosen on landing
    std::uint32_t steps{};       // physics steps in the air
    std::uint64_t air_ms{}, landed_at{}; // flight time, and GetTickCount64() at the landing
};
// transform: the skater's world matrix. board: the deck's physics body (its rotation rows start at +0x20).
void watch_revert_spin(std::uintptr_t selector, std::uintptr_t transform, std::uintptr_t board) noexcept;
RevertLanding last_revert_landing() noexcept;
// Fast probe: one row for every physics-state choice the game makes for the local skater (many per
// second, where the client tick sees about nine), with the state it was in, the state the game wanted,
// what was returned, the controller buttons and some of the skater's request words. Read-only. Rows are
// queued by the selector hook and drained by the client tick; a full queue drops rows.
struct FastProbeRow {
    std::int64_t ticks{}; // QueryPerformanceCounter
    std::uint32_t current{}, wanted{}, chosen{}, buttons{};
    std::uint32_t words[18]{}; // context+0x13c4..0x13e0, then +0x140c..0x1430
    float board_up{};          // the board's up axis, height part (+1 upright, -1 upside down; 9 = not readable)
    float axes[9]{};           // the board body's three unit axes (x,y,z each; the middle one is up; 9 = not readable)
    std::uint32_t dark_pop{};  // Dark Pop: 1 D-pad pressed this flight, 2 armed, 4 pop's flight, 8 after its touchdown
    // 1.1: context +0x4e0 (the footplant's plant point, 4 floats), +0x4f0 (its up axis, 4), +0x19c0/+0x19c4 (the two
    // stick values the old steering read), board-placement +0x140 (4; the old launch's start position); 9 = not readable.
    float extra[14]{};
    // 1.7 stage probe: the board's up axis as last sampled at the footplant tick's entry, after it returned, and after
    // the skater mover returned (9 = no sample since the last chooser call); the board body's +0x70 (linear) and
    // +0x80 (angular, unverified) velocities at the chooser.
    float stage_up[3]{};
    float board_vel[6]{};
    // 2.0: context +0x18c0 and +0x1838 (the 201 case's gate floats; 9 = not readable) and the word at +0x13f8.
    float gates[2]{};
    std::uint32_t word_13f8{};
    // 2.3: context +0x17b0 (the chooser's contact count, as found before any hold; 0xffffffff = unread) and the
    // physics block's bytes: bit 0 ground hit (+0xfb3), bits 1-4 wheels (+0xed0..+0xed3); 0xff = unread.
    std::uint32_t contacts{}, phys_flags{};
};
void fast_probe_enable(bool on) noexcept;
std::size_t fast_probe_drain(FastProbeRow* rows, std::size_t capacity) noexcept;
// Dark Pop: a flight with a D-pad press and the board upside down is armed; at its landing the pre-0.32.0 placement
// hold is restored and the chooser is sent to state 601, whose exit is the pop. `board` is the board body
// (NoclipBodies::parts[0]). Publish from the client tick.
void dark_pop_update(bool enabled, std::uintptr_t board, bool unpatch = true, bool hold = true, bool one_press = true,
    unsigned route_stay = 11, unsigned route_guard = 12, bool keep_speed = true, bool catch_dismount = true,
    bool feet = true, bool adaptive = true, bool catch_timeout = true, float pop_strength = 0.0f, bool hold_landing = true, bool dark_exit = true, bool full_flight = true, bool hold_flight = true, unsigned stay_physical = 6, bool early_physics = true, bool reaim = true, bool hide_dpad = true) noexcept;
// The skater's physics bodies (NoclipBodies::parts, board first), for the keep-speed write.
void dark_pop_publish_bodies(const std::uintptr_t* parts, std::size_t count) noexcept;
struct DarkPopLast {
    std::uint64_t landings{};     // armed landings since the process started
    std::uint32_t wanted{};       // what the game chose at the last one (601 = the route, 200 = its own pop, 504, 300, 1xx)
    float board_up{};             // the board's up axis (height part) on the decision tick of that landing
    std::uint64_t pop_landings{}; // landings of a pop's flight since the process started
    std::uint32_t pop_landed{};   // the state the last of them went to
    float pop_seconds{};          // how long that pop's flight lasted
    // Unpatch: the switch, whether the placement hook is installed, ticks the B bit was set, and what the placement
    // calls in the landing window saw.
    bool unpatch{}, unpatch_hooked{};
    unsigned unpatch_ticks{}, place_calls{}, place_latched_before{}, place_latched_after{}, place_flag{},
        place_arg_flag{}, place_side{}, place_forced{};
    // Hold: the switch, whether its hooks are installed, and what it did at the last landing.
    bool hold{}, hold_hooked{};
    unsigned hold_starts{}, hold_resets{}, hold_ticks{}, attach_requests{}, attach_bone{}, hold_ground{};
    std::uint64_t releases{};
    float hold_blend{}, pop_board_up{};
    // One press one pop and the 601 route: the switches, and at the last landing / stay / pop: ticks the route's
    // two bits were raised; exits from 601 so far; how long the last stay lasted, the stay length, ticks the package
    // was found raised and the stay bit cancelled, where it went; the guard and keep speed.
    bool one_press{}, route{};
    unsigned route_ticks{}, route_live_ticks{}, route_clears{}, route_stay{}, route_stay_clears{};
    std::uint32_t route_exit{};
    std::uint64_t route_exits{};
    bool route_let_go{};
    unsigned route_guard{}, guard_clears{};
    bool keep_speed{};
    float landing_speed{}, added_speed{};
    unsigned speed_bodies{}, speed_reasserts{};
    // 1.0: early dismounts caught: the switch, whether the last dark landing was one (the game wanted 504 straight from
    // the air, with no landing tick), and how many have been caught since the process started.
    bool catch_dismount{}, caught{};
    std::uint64_t catches{};
    // 1.9: timed-out landings caught: the switch, whether the last dark landing was one (the game wanted 201 -> 200 with
    // the board dark and resting, its landing stage never raised), and how many have been caught since the process started.
    bool catch_timeout{}, timed_out{};
    std::uint64_t timeout_catches{};
    // 1.10 pop strength: the floor (m/s, 0 = off); at the last sink: the context's vertical velocity found on the entry
    // tick, whether it was floored, the value read back on the first sink tick (9 = not read); writes so far.
    float pop_strength{}, strength_found{}, strength_seen{};
    bool strength_written{};
    std::uint64_t strength_writes{};
    // 2.2 launch: pop_strength is now the launch (m/s, 0 = off) written into the 200 state's launch record on the
    // pop's first 200 call. At the last exit: ticks since the animation's jump output and its vertical velocity
    // (context +0x17c4/+0x5f4; 0xffffffff / 9 = unread); on the first 200 call: whether the current state object was
    // the 200 one, its launch flag (9 = unread), the record's velocity as found (9 = unread), whether y was written.
    unsigned jump_ticks{};
    float jump_vy{9.0f};
    bool launch_state_ok{}, launch_written{};
    unsigned launch_flag{9};
    float launch_found[3]{9.0f, 9.0f, 9.0f};
    std::uint64_t launch_writes{};
    // 2.4 launch (bodies): at the last pop, the board's vertical velocity on the second air tick (9 = unread), the
    // lift added on the second and third ticks, bodies written, lift ticks, lifts so far.
    float lift_found{9.0f}, lift_added[2]{};
    unsigned lift_bodies{}, lift_ticks{};
    std::uint64_t lifts{};
    // 2.0 hold the landing: the switch, chooser calls the bit was raised on at the last landing, and landings the game
    // staged itself after a hold, since the process started.
    bool hold_landing{};
    unsigned hold_landing_ticks{};
    std::uint64_t hold_landing_staged{};
    // 2.1 dark exit: the switch, whether the board was put back at the last exit, its up axis before and after (9 = unread).
    bool dark_exit{}, exit_written{};
    float exit_up_before{}, exit_up_after{};
    // 2.1 full flight: the switch, and at the last pop: launch velocity, ballistic time, the countdown found at the 201
    // entry (9 = unread), calls floored, why it stopped (1 time up, 2 contact, 3 left 201, 4 launch too weak).
    bool full_flight{};
    float flight_v0{}, flight_total{}, flight_found{};
    // 2.3 hold the flight: the switch; at the last pop, the 201 calls whose contact inputs were cleared and the largest
    // contact count seen on them.
    bool hold_flight{};
    unsigned hold_blocks{}, hold_contacts_max{};
    // 2.5 stay physical: the tick count (0 = off) and, at the last pop, the 200 calls the jump-fired bit was hidden on.
    unsigned stay_physical{}, stay_clears{};
    // 2.6 physics flight: the switch, and whether the last pop's jump fired early (so the flight stayed physical).
    bool early_physics{}, early_jump{};
    // 2.7 re-aim: on, curve writes on the last pop, the tick of the first, what was written (start, velocity), the
    // body's and the context's vertical velocity it chose between, frames left written, the request's time offset,
    // the state the flight was handed to (201) or 0, and why nothing was written.
    bool reaim{};
    unsigned reaim_writes{}, reaim_tick{}, reaim_frames{}, reaim_from{};
    float reaim_pos[3]{}, reaim_vel[3]{}, reaim_body_vy{}, reaim_context_vy{}, reaim_time{};
    const char* reaim_skip{""};
    unsigned flight_floors{}, flight_stop{};
    // Feet-only hold (1.6): the switch, whether the footplant-tick hook is in, and at the last landing: the footplant
    // ticks whose attach requests were cleared and the two-tick attaches requested.
    bool feet{}, feet_ready{};
    unsigned feet_skips{}, feet_requests{};
    // 1.6 orientation hold: whether the mover hook is in, and in the last sink: first-tick captures (1) and ticks held.
    bool mover_ready{};
    unsigned mover_captures{}, mover_held{};
    // 1.7 adaptive sink: the switch, and the sink tick it ended the last sink on (0 = it did not fire).
    bool adaptive{};
    unsigned adaptive_tick{};
    // 1.8 board freeze, in the last sink: ticks the board's transform was written back, ticks the previous write was
    // still there at the next chooser call, ticks it had been overwritten.
    unsigned freeze_writes{}, freeze_held{}, freeze_lost{};
};
DarkPopLast dark_pop_last() noexcept;
// Further listeners on the same steps (skater_events.h, launches.h), independent of the observer
// above so none displaces another. Same thread and timing. Adding one already there, or removing one that is not, does nothing;
// false when all slots are taken.
inline constexpr std::size_t no_bail_step_listener_slots = 4;
bool add_no_bail_step_listener(NoBailStepObserver listener) noexcept;
// Any thread: the watched local skater bails at its next physics step, as from a slam of
// `magnitude` (Board Break: a board that snaps on a rail throws the rider). The cause goes in just
// before the game's selector picks the next state, the way the game's own contact tests record
// theirs, so the game makes the wipeout itself; while No Bail protects the skater it is dropped like
// any other. Expires unused after a quarter of a second.
void no_bail_request_wipeout(float magnitude) noexcept;
void remove_no_bail_step_listener(NoBailStepObserver listener) noexcept;
}
