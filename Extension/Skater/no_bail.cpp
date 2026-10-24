#include "no_bail.h"
#include "Extension/Skater/skater_events.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <intrin.h>

namespace dingosdk {
namespace {
using namespace addr::no_bail;
// Native cause recorder: RCX=collector, EDX=reason, XMM2=magnitude.
using RecordCause = void (*)(std::uintptr_t, std::int32_t, float);
using ChooseState = std::uint32_t (*)(std::uintptr_t, std::uint32_t);
using SkeletonResponse = void (*)(std::uintptr_t, float, bool);
using PublishAnimation = void (*)(std::uintptr_t);
using ResetCauses = void (*)(std::uintptr_t);
constexpr std::uintptr_t highest = memory::highest_user_address;
// The cause most real slams record (Board Break's logs: cause 6 at forces of 9.6 to 10.7): a wipeout
// asked for goes in as one of those.
constexpr std::int32_t wanted_wipeout_reason = 6;
// The hooks re-resolve the local skater's ownership (some 35 fields) on every
// protected physics step: guarded same-process copies, not a system call each.
template<class T> bool read(std::uintptr_t address, T& value) noexcept { return memory::peek(address, value); }
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !read(object + offset, value) ||
        value < 0x10000 || value > highest - 0x1000100) return 0;
    return value;
}
struct Owner {
    std::uintptr_t client{}, entity{}, player{}, handle{}, component{}, core{}, context{}, selector{}, causes{}, rig{};
    bool operator==(const Owner&) const = default;
};
struct Lease {
    Owner owner;
    std::uint64_t manual_until{}, flight_until{};
    bool active(std::uint64_t now) const noexcept {
        return now < manual_until || now < flight_until;
    }
};
struct BoardLock {
    Owner owner;
    std::uint64_t until{};
};
struct Protection {
    std::uintptr_t base{};
    bool mount_test{}; // the selector's mount request test is the known one
    BoardLock board;
    RecordCause cause_original{};
    ChooseState choose_original{};
    SkeletonResponse skeleton_original{};
    PublishAnimation publish_original{};
    ResetCauses reset_causes{};
    std::atomic<bool> ready{};
    SRWLOCK lock = SRWLOCK_INIT;
    Lease lease;
    std::atomic<NoBailStepObserver> step_observer{};
    std::array<std::atomic<NoBailStepObserver>, no_bail_step_listener_slots> listeners{};
};
Protection& protection() { static auto* value = new Protection; return *value; }
struct StateWatch {
    std::atomic<std::uintptr_t> selector{}, causes{}; // the watched skater's selector and cause collector
    std::atomic<std::uintptr_t> core{};
    std::atomic<std::uint64_t> until{}, changes{}, wipeouts{};
    std::atomic<std::uint64_t> runouts{};
    std::atomic<bool> runout{};
    std::atomic<std::uint32_t> state{}, previous{};
    std::atomic<std::int64_t> since{}, previous_ticks{}; // performance-counter ticks
};
StateWatch& state_watch() { static auto* value = new StateWatch; return *value; }
// Revert Boost spin tracking. The published fields are atomics; the rest is only touched by the
// physics selector of the watched skater.
struct SpinWatch {
    std::atomic<std::uintptr_t> selector{}, transform{}, board{};
    std::atomic<std::uint64_t> until{};
    bool airborne{}, have_yaw{}, board_ok{};
    float yaw{}, spin{}, board_yaw{}, board_spin{};
    std::uint32_t steps{}, last_air{};
    std::uint64_t took_off{};
    SRWLOCK lock = SRWLOCK_INIT;
    RevertLanding last;
};
SpinWatch& spin_watch() { static auto* value = new SpinWatch; return *value; }
constexpr float spin_pi = 3.14159265f;
void track_spin(std::uintptr_t selector, std::uint32_t chosen) noexcept {
    auto& s = spin_watch();
    const auto now = GetTickCount64();
    if (selector != s.selector.load(std::memory_order_acquire) || now >= s.until.load(std::memory_order_acquire)) {
        if (selector == s.selector.load(std::memory_order_relaxed)) s.airborne = s.have_yaw = false;
        return;
    }
    std::array<float, 16> m{};
    const auto transform = s.transform.load(std::memory_order_acquire);
    if (!transform || !read(transform, m) || !std::isfinite(m[8]) || !std::isfinite(m[10]) ||
        std::abs(m[8]) + std::abs(m[10]) < 1e-3f) {
        s.airborne = s.have_yaw = false;
        return;
    }
    const float yaw = std::atan2(m[8], m[10]);
    // The deck's rotation, read the same way.
    // Rows assumed at +0x20 / +0x30 / +0x40 (position at +0x50, whose height +0x54 ReSkate already reads).
    std::array<float, 12> b{};
    float board_yaw{};
    bool board_ok = false;
    if (const auto board = s.board.load(std::memory_order_acquire); board && read(board + 0x20, b)) {
        const auto length = [&](int r) { return b[r * 4] * b[r * 4] + b[r * 4 + 1] * b[r * 4 + 1] + b[r * 4 + 2] * b[r * 4 + 2]; };
        board_ok = std::isfinite(length(0) + length(1) + length(2)) && std::abs(length(0) - 1) < 0.05f &&
            std::abs(length(1) - 1) < 0.05f && std::abs(length(2) - 1) < 0.05f && std::abs(b[8]) + std::abs(b[10]) > 1e-3f;
        if (board_ok) board_yaw = std::atan2(b[8], b[10]);
    }
    const auto wrap = [](float d) {
        while (d > spin_pi) d -= 2 * spin_pi;
        while (d < -spin_pi) d += 2 * spin_pi;
        return d;
    };
    const bool air = chosen >= 200 && chosen <= 299;
    if (!s.airborne) {
        if (air && s.have_yaw) {
            // Count from the last ground pose, so the turn begun on the takeoff step is kept.
            s.airborne = true;
            s.spin = wrap(yaw - s.yaw);
            s.board_spin = board_ok && s.board_ok ? wrap(board_yaw - s.board_yaw) : 0.0f;
            s.board_ok = board_ok && s.board_ok;
            s.steps = 1;
            s.took_off = now;
            s.last_air = chosen;
        }
    } else {
        s.spin += wrap(yaw - s.yaw);
        if (s.board_ok && board_ok) s.board_spin += wrap(board_yaw - s.board_yaw);
        else s.board_ok = false;
        if (air) {
            ++s.steps;
            s.last_air = chosen;
        } else {
            // The pose read here is the one the flight ended with, before the game's revert turns it.
            RevertLanding landing;
            landing.spin_degrees = s.spin * 180.0f / spin_pi;
            landing.board_spin_degrees = s.board_spin * 180.0f / spin_pi;
            landing.board_valid = s.board_ok;
            landing.board_offset_degrees = wrap(board_yaw - yaw) * 180.0f / spin_pi;
            landing.heading_degrees = (s.board_ok ? board_yaw : yaw) * 180.0f / spin_pi;
            landing.from = s.last_air;
            landing.to = chosen;
            landing.steps = s.steps;
            landing.air_ms = now - s.took_off;
            landing.landed_at = now;
            AcquireSRWLockExclusive(&s.lock);
            landing.sequence = s.last.sequence + 1;
            s.last = landing;
            ReleaseSRWLockExclusive(&s.lock);
            s.airborne = false;
        }
    }
    s.yaw = yaw;
    s.have_yaw = true;
    if (!s.airborne) s.board_ok = board_ok; // on the ground: whether the next takeoff has a board pose
    s.board_yaw = board_yaw;
}
// A wipeout asked for (no_bail_request_wipeout): the cause goes in at the watched selector's next pick.
struct WantedWipeout {
    std::atomic<float> magnitude{};
    std::atomic<std::uint64_t> until{};
};
WantedWipeout& wanted_wipeout() { static auto* value = new WantedWipeout; return *value; }

// Recheck live local ownership at use time. A retained physics address alone
// must never protect another skater after a respawn or level change.
bool resolve(std::uintptr_t client, std::uintptr_t entity, Owner& o) noexcept {
    const auto base = protection().base;
    unsigned state{}, manager_offset{};
    if (pointer(client) != base + addr::engine::client_vtable || !read(client + 0xc4, state) ||
        (state != 13 && state != 21) || pointer(entity) != base + addr::engine::skater_entity_vtable ||
        !read(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000) return false;
    const auto context = pointer(client, 8);
    const auto manager = pointer(context, manager_offset);
    const auto begin = pointer(manager, 0x4c8), end = pointer(manager, 0x4d0);
    if (!context || pointer(entity, 0x20) != context || pointer(manager) != base + addr::engine::local_player_manager_vtable ||
        !begin || end != begin + 8) return false;
    o.player = pointer(begin);
    std::uint8_t local{}, remote{}, teleport{};
    if (pointer(o.player) != base + addr::engine::local_player_vtable || pointer(o.player, 0x78) != context ||
        !read(o.player + 0x45, local) || local != 1 || !read(o.player + 0x44, remote) || remote ||
        pointer(o.player, 0xb8) != entity || pointer(entity, 0xf8) != o.player ||
        !read(entity + 0x7e0, teleport) || teleport) return false;
    o.handle = pointer(o.player, 0xb0);
    const auto collection = pointer(entity, 0x70);
    o.component = pointer(entity, 0x628);
    o.core = pointer(o.component, 0x70);
    o.context = pointer(o.core, 0x3c0);
    o.selector = pointer(o.core, 0x440);
    o.causes = pointer(o.core, 0x428);
    o.rig = pointer(o.core, 0x438);
    if (pointer(o.handle) != entity + 8 || pointer(collection) != entity ||
        pointer(o.component) != base + addr::engine::skater_component_vtable || pointer(o.component, 0x18) != collection ||
        pointer(o.core) != base + bail_core_vtable || !o.context || !o.selector ||
        pointer(o.selector, 8) != o.context || pointer(o.causes, 0x20) != o.context || pointer(o.rig) != o.context || pointer(o.rig, 0x4630) != o.core)
        return false;
    o.client = client;
    o.entity = entity;
    return true;
}
// Dark Pop borrows this protection around armed landings and the pops they make (defined with Dark Pop below).
bool dark_pop_protecting(std::uint64_t now) noexcept;
bool protected_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    // The lock only copies SDK data, and is never held across native code or
    // memory reads. Hooks do not contend for the debug/camera action lock.
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    const auto now = GetTickCount64();
    if (!(lease.active(now) || dark_pop_protecting(now)) || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
}
// The local skater this process published (the lease keeps its owner even with No Bail off), checked
// against the live game objects. Says nothing about whether protection is on.
bool local_owner(std::uintptr_t object, std::uintptr_t Owner::* member, Owner* owner = nullptr) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto lease = p.lease;
    ReleaseSRWLockShared(&p.lock);
    if (!lease.owner.entity || !object || object != lease.owner.*member) return false;
    Owner current;
    if (!resolve(lease.owner.client, lease.owner.entity, current) || current != lease.owner) return false;
    if (owner) *owner = current;
    return true;
}
// The published owner, its chain resolved again now; when `rig` is given, only if it is that owner's.
bool published_skater(std::uintptr_t rig, NoBailSkater& skater) noexcept {
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&p.lock);
    const auto published = p.lease.owner;
    ReleaseSRWLockShared(&p.lock);
    Owner current;
    if (!published.entity || (rig && rig != published.rig) || !resolve(published.client, published.entity, current) ||
        current != published) return false;
    skater = {p.base, current.entity, current.core, current.context, current.rig};
    return true;
}
bool cancel_request(std::uintptr_t context, std::uintptr_t offset, LONG mask) noexcept {
    const auto address = context + offset;
    if (context < 0x10000 || context > highest - offset - sizeof(LONG) ||
        (address & (alignof(LONG) - 1)) != 0) return false;
    // Consume only the identified request bit. Preserve unrelated native flags
    // even when another producer updates the same word.
    __try {
        _InterlockedAnd(reinterpret_cast<volatile LONG*>(address), ~mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool cancel_impact_request(std::uintptr_t context) noexcept {
    return cancel_request(context, impact_request_offset, impact_request_mask);
}
bool cancel_wipeout_requests(std::uintptr_t context) noexcept {
    return cancel_impact_request(context) &&
        cancel_request(context, animation_request_offset, animation_request_mask);
}
bool reset_pending_causes(std::uintptr_t causes) noexcept {
    const auto reset = protection().reset_causes;
    if (!reset) return false;
    __try {
        reset(causes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool filter_requests(std::uintptr_t object, std::uintptr_t Owner::* member) noexcept {
    LastError error;
    Owner owner;
    // Native collision/landing checks also write the collector inline. Reset
    // it at each consumer, after those writes, before either animation's bail
    // or recover/runout query. Filtering record_cause alone misses this path.
    return protected_owner(object, member, &owner) && cancel_wipeout_requests(owner.context) &&
        reset_pending_causes(owner.causes);
}
bool suppress_cause(std::uintptr_t causes, std::int32_t reason, std::uintptr_t caller) noexcept {
    LastError error;
    Owner owner;
    if (!protected_owner(causes, &Owner::causes, &owner)) return false;
    for (const auto& impact : impact_bail_calls) {
        if (caller == protection().base + impact.return_rva && reason == impact.reason)
            return cancel_impact_request(owner.context);
    }
    return true;
}
void record_cause(std::uintptr_t causes, std::int32_t reason, float magnitude) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (causes == state_watch().causes.load(std::memory_order_acquire)) {
        skater_events::note_cause(reason, magnitude);
    }
    const bool protect = suppress_cause(causes, reason, caller);
    // Do not force the native recovery/stumble predicate (recovery_predicate). Its
    // result is also exported to animation at +0x9e, even without a collision.
    // Stop new causes before they reach either the wipeout or runout decision;
    // the native per-step reset still owns clearing the collector's history.
    if (!protect) protection().cause_original(causes, reason, magnitude);
}
// The local skater waiting on foot for its S.K.A.T.E. turn: its mount request goes.
void hold_off_board(std::uintptr_t selector, std::uint32_t current) noexcept {
    auto& p = protection();
    if (current != offboard_physics_state || !p.mount_test || !p.ready.load(std::memory_order_acquire)) return;
    AcquireSRWLockShared(&p.lock);
    const auto board = p.board;
    ReleaseSRWLockShared(&p.lock);
    if (GetTickCount64() >= board.until || selector != board.owner.selector) return;
    Owner current_owner;
    if (!resolve(board.owner.client, board.owner.entity, current_owner) || current_owner != board.owner) return;
    (void)cancel_request(current_owner.context, animation_request_offset, mount_request_mask);
}
// ---- Dark Pop ---------------------------------------------------------------------------
// Before skate. 0.32.0, landing a dark catch (the board upside down) with a D-pad direction held popped the skater
// back into the air: the body sank into the ground and was thrown back up, like landing on a trampoline. 0.32.0
// removed one animation signal ("X"), which (1) gated the board placement's old hold (the foot-attach that glued the
// board through the landing) and (2) let the physics-state chooser leave the landing for state 601, the air-to-board
// state whose exit to the air is the pop. This mod puts both back:
// - A flight is armed once a D-pad direction (Left, Down or Right) has been pressed in it and the board is upside
//   down (the board body's up axis points down); it disarms if the board is turned back over.
// - Unpatch + hold: in the landing window the placement is entered the pre-0.32.0 way (B signal + latched) and the
//   old hold is started with foot-attach mode 2, so the board stays glued.
// - 601 route: on the landing tick the chooser's two 601 inputs (+0x13d0 bit 31, +0x13d4 bit 2; never produced by
//   the current game) are raised for that one decision. In 601 the bail package, the wipeout requests and the
//   animation's bail request are cleared before every decision, No Bail's protection is borrowed, and after `stay`
//   ticks the game's own stay signal (+0x13d4 bit 1) is cancelled so the chooser falls through to 200: the pop. The
//   pop's first `guard` ticks get the same protection, and the landing's forward speed is put back (601's flatten
//   plants the feet).
// The chooser, both exes, 200/201 cases: (bit31 && bit2) -> 601 first; the 601 case: +0x13c4 bit 15 -> 300, else
// +0x13d4 bit 1 -> stay, else +0x13c4 bit 27 -> 201, else 200.
extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput* input, bool allow_menu);
extern "C" void DingoSDKOverlayDarkPopAir() noexcept; // input_capture.cpp: the D-pad hidden from the game while this is renewed
constexpr unsigned dark_pop_dpad_bits = 0x4 | 0x2 | 0x8;  // D-pad Left, Down, Right (XInput bits; PlayStation pads too)
constexpr float dark_pop_dark_below = -0.3f;              // board up axis, height part: below this the board is upside down
constexpr float dark_pop_upright_above = 0.3f;            // and above this it has been turned back over
constexpr float dark_pop_adaptive_above = -0.75f;         // 1.7: in the sink, above this the board is rolling over: pop now
constexpr float dark_pop_rest_speed = 2.0f;               // 1.9: |board vertical velocity| below this = the board is resting on the ground
constexpr unsigned dark_pop_rest_ticks = 3;
// 1.10: the skater context's velocity (+0x4f0, the probe's pup_* columns; x/z match the board's). Its vertical part on
// the sink-entry tick predicts the pop (8 Oct, sink 0: staged landings enter at -5..-7 m/s and fly 1.0-2.1 s, timed-out
// ones mostly at -2 after resting and fly 0.4-0.8 s; the pop's bv_y two ticks after the exit is ~1.1x it). The pop is a
// bounce of this number; Pop strength floors it on the entry tick.
constexpr std::uintptr_t context_velocity_offset = 0x4f0;
// 2.2 launch (read from the exe, 9 Oct): the state transition 0x47d3fa0 keeps the state objects on the core
// (+0x2f8 = state 200, +0x3b0 = the current one). State 200's enter 0x482be90, entered from 601, builds a launch
// record on its object (+0x910 position, +0x920 velocity, +0x96d flag) and its step 0x48347b0 moves the rig along
// that record every tick (v += g dt, p += v dt): the pop's flight is scripted from the record, which is why the
// second air tick shows the velocity and the bodies never took a write. The record's velocity comes from the
// animation's jump output when one fired within 12 ticks (context +0x17c4 = ticks since it, +0x5f0 = its velocity,
// both stamped each tick by 0x47dda10 from [[core+0x3b8]+0x10]+0x1fa/+0x90), else from context +0x4f0.
// 2.3 hold the flight (read from the exe, 9 Oct): the chooser's 201 case lands on contact once the state is 0.04 s
// old, whatever the countdown says: its contact flag is context +0x17b0 > 0 || +0x13c4 & 0x90000. +0x17b0 is copied
// every tick from the physics-query block [[core+0x3b8]+0x30]+0 (that block's +0xfb3 is a ground-hit byte, +0xed0..
// +0xed3 wheel bytes, +0xfb1/+0xfb2/+0xfb7 feed +0x13c4 bits 17/18/23). 2.2 test: every 180 hop at the drop spot
// left 201 that way with 0.3-0.75 s still on the countdown.
constexpr std::uintptr_t context_contacts_offset = 0x17b0;
constexpr std::uint32_t context_contact_bits = 0x90000u; // in +0x13c4
constexpr std::uintptr_t core_interface_offset = 0x3b8, interface_physics_offset = 0x30;
constexpr std::uintptr_t physics_ground_byte_offset = 0xfb3, physics_wheel_bytes_offset = 0xed0;
constexpr float hold_flight_countdown_min = 0.05f;      // the 201 case's own landing threshold on +0x18c0
// 2.5 stay physical (read from the exe, 9 Oct): the chooser's 200 case goes to 201 (the animation-driven air state)
// when context +0x13c4 bit 27 is set (0x47dda10 sets it each tick from the launch-request processor 0x4755c70: a jump
// fired). On a clean pop that is 8 ticks after the exit, after physics has carried the body up; after a 180 landing
// it is the first tick, the animation's pose is still on the ground, the bodies are pulled back, contact kills the
// lift (2.4 test: 0.18-0.20 s hops). So for the first ticks of a pop the bit is hidden from the chooser's decision.
constexpr std::uint32_t context_jump_fired_bit = 0x8000000u; // in +0x13c4
constexpr unsigned physics_flight_cap_ticks = 180;           // 3 s: a physics flight never holds 200 past this
constexpr std::uintptr_t core_current_state_offset = 0x3b0;
constexpr std::uintptr_t core_air_state_offset = 0x2f8;
constexpr std::uintptr_t air_state_launch_velocity_offset = 0x920; // 4 floats; +4 = vertical
constexpr std::uintptr_t air_state_launch_flag_offset = 0x96d;     // byte: 1 = the step integrates the record
constexpr std::uintptr_t context_jump_ticks_offset = 0x17c4;      // u32
constexpr std::uintptr_t context_jump_velocity_offset = 0x5f0;    // 4 floats; +4 = vertical
// 2.7 re-aim the handover (read from the exe, 9 Oct). The 201 state (core+0x308; vtable 0x65e8a18, step 0x4822230,
// enter 0x4817f30) does not integrate anything: its step places the rig (0x4775ce0) at state+0x3a0, which 0x48256b0
// sets every tick to a sample of the launch block's curve, p0 + v0 t + g t^2 / 2 at t = frame * dt (sampler 0x474ea20;
// the block B = core+0x3e8, kept at +0x30 of every state object; p0 B+0x1880, v0 B+0x1890, g B+0x18a0, dt B+0x18b4),
// plus an offset found on 201's first tick (0x474e8c0: the nearest of frames 0-99 to context +0x4e0, the leftover
// faded out over the next ticks). The curve is copied by 0x4747b20 from the current 0x110-byte jump request
// (R = [B+0x1100]: p0 +0xb0, v0 +0xc0, g +0xd0, frames +0x80, a time offset +0x30 that 201's step also evaluates the
// request at) when the launch processor 0x4755c70 takes a request, i.e. when the jump fires. After a 180 landing that is
// the pop's first tick, from the ground at the plant's own speed: 201 then finds the nearest point of a small hop and
// pulls the body onto it (the 2.5 rubber band). So after the Stay physical ticks, on every 200 call until the handover,
// the curve and the request are rewritten from the context's position and the body's velocity.
constexpr std::uintptr_t core_jump_block_offset = 0x3e8;
constexpr std::uintptr_t core_anim_air_state_offset = 0x308;
constexpr std::uintptr_t state_jump_block_offset = 0x30;
constexpr std::uintptr_t jump_block_curve_start_offset = 0x1880, jump_block_curve_velocity_offset = 0x1890;
constexpr std::uintptr_t jump_block_curve_gravity_offset = 0x18a0, jump_block_curve_dt_offset = 0x18b4;
constexpr std::uintptr_t jump_block_request_offset = 0x1100;
constexpr std::uintptr_t request_time_offset = 0x30, request_frames_offset = 0x80, request_start_offset = 0xb0, request_velocity_offset = 0xc0;
constexpr std::uintptr_t context_position_offset = 0x4e0;      // 4 floats (the probe's plant_* columns: world position)
constexpr float reaim_min_vy = 0.5f;                            // m/s: below this the body is not rising, nothing is written
constexpr unsigned reaim_min_frames = 6, reaim_max_frames = 240;
// 2.0 hold the landing. The chooser's 201 case (0x47460b9): with context +0x13cc bit 20 set it returns 201 (0x4746a84 ->
// 0x4746a8a); with it clear and +0x18c0 below -0.2 it returns 200, which is the drop that cut the landing sequence
// (+0x13c4: 0x300000, then 0x880000, then the landing stage in +0x13c8) short on the nollie 360 flip. On an armed
// flight in 201 with the board dark and resting and 0x880000 up but no stage yet, the bit is raised before the chooser
// and taken down after, for at most this many calls, so the game can stage the landing itself.
constexpr std::uintptr_t context_landing_hold_offset = 0x13cc;
constexpr LONG context_landing_hold_mask = 0x100000;
constexpr unsigned dark_pop_landing_sequence_mask = 0x880000; // +0x13c4
constexpr unsigned dark_pop_landing_hold_limit = 10;
// Probe columns for the gate: context +0x18c0 and +0x1838 (floats the 201 case compares with -0.2, 0.05, 0.04, 0.08).
constexpr std::uintptr_t context_gate_a_offset = 0x18c0, context_gate_b_offset = 0x1838, context_word_13f8_offset = 0x13f8;
// 2.1 full flight. +0x18c0 is a countdown to the landing: copied every tick from the animation graph's output block
// (0x47d8f22: [[core+0x3b8]+0x10]+0xd8), set at the known-air (201) entry (2.0 test: 0.32-0.41 s on flat-ground pops that
// physics would have carried 1.0-1.6 s; the game put the skater in a ground state at 0.5 s with the board still rising
// at +1..+4 m/s) and decremented by dt. On a pop's flight the countdown is floored at the ballistic time left from the
// launch velocity (2 v / g - elapsed, flat ground), never lowered, until the board's vertical velocity reads well above
// free fall (contact) or the flight ends.
constexpr float dark_pop_gravity = 9.81f;
constexpr float dark_pop_full_flight_min_v = 1.0f;        // m/s: no floor below this launch velocity
constexpr float dark_pop_full_flight_cap = 3.0f;          // s: never floor past this
               // 1.9: resting this many chooser calls in a row before a timed-out landing is caught
// 1.8 board freeze: the board body's transform (three axis rows +0x20/+0x30/+0x40 and the position row +0x50; the up
// axis is +0x34) is taken on the first sink tick and written back, with the velocity zeroed, on every later one.
constexpr std::uintptr_t body_transform_offset = 0x20;
constexpr std::size_t body_transform_floats = 16;
constexpr float freeze_held_tolerance = 0.05f;
constexpr std::uintptr_t dark_pop_board_up_offset = 0x34; // board body (NoclipBodies::parts[0]), float 13
struct DarkPopFlag { std::uintptr_t offset; unsigned mask; };
// The bail package: context+0x13c4 bit 0x8000, +0x13c8 bits 0x2c000, +0x13cc bit 0x400 (landing contact),
// +0x13d4 bit 0x100 (the dismount request). The landing stage is +0x13c8 bit 0x4000000.
constexpr DarkPopFlag dark_pop_bail_flags[] = {{0x13c4, 0x8000}, {0x13c8, 0x2c000}, {0x13cc, 0x400}, {0x13d4, 0x100}};
constexpr unsigned dark_pop_landing_stage = 0x4000000;
constexpr std::uint64_t dark_pop_pop_ms = 2500;      // the longest a pop's flight is protected if its landing is missed
constexpr std::uintptr_t context_signals_offset = 0x13d0; // context word holding the animation signals A (bit 29) and B (bit 30)
// Unpatch (0.10.0). By field identity, 0.32.0 removed a third graph output ("X", old 0x13d0 bit 29) and
// renumbered the other two: old bit 30 = new bit 29 ("A"), old bit 31 = new bit 30 ("B"). The old
// board-placement entry test was (B || (A && !X)) && (X || latched || !ground) && (arg+0x54 || flag); the new one is
// the same with X false. X also drove a timed hold state machine that 0.32.0 deleted. This experiment emulates the
// old `B && X` entry for every placement call in the landing window: B is set in the context and the placement
// object's latched byte (+0x294) is forced, which is what X used to skip. The landing-stage bit is also on during the
// catch, so a landing is the stage bit coming back after it has been clear for a few ticks since arming.
constexpr LONG dark_pop_unpatch_mask = 0x40000000;      // context+0x13d0 bit 30 ("B")
constexpr std::uintptr_t placement_context_offset = 0x1c0; // board-placement object -> physics context
constexpr std::uintptr_t placement_latched_offset = 0x294; // byte: the special path's latched flag
constexpr std::uintptr_t placement_flag_offset = 0x295;    // byte: the third gate's alternative to arg+0x54
constexpr std::uintptr_t placement_arg_flag_offset = 0x54; // arg byte: third gate
constexpr std::uintptr_t placement_arg_side_offset = 0x55; // arg byte: bone choice (0xf / 0x13)
constexpr unsigned dark_pop_stage_clear_ticks = 5;         // stage bit clear this long since arming = the catch is over
// Hold port (0.11.0). The old board-placement update (0x442c5f0) ran a hold state machine keyed on X: with X on and the
// hold idle it started the hold when the entry had latched this tick and the blend (old +0x270, now +0x220) was under
// 0.3, else reset the stored placement; while active it added dt to the timer and ran the apply function (0x441e740),
// whose X path placed the board at the foot bone and requested foot-attach mode 2 on the bone the entry chose (old
// +0x28c, now +0x23c) while the blend was under 0.2. 0.32.0 kept the state machine (active +0x29a, applied +0x29b,
// timer +0x234) but starts it only from B plus the arg's +0x56 flag, and its apply (0x47ecd30) only ever requests mode
// 1 (a blended offset). The mode setters are still in the exe (0x4769020 regular, 0x4768fb0 switch). The port starts
// the hold the old way before the game's update runs and requests mode 2 after it, in the same window as the unpatch.
constexpr std::uintptr_t placement_skater_offset = 0x1c8;      // board-placement object -> skater
constexpr std::uintptr_t placement_active_offset = 0x29a;      // byte: the hold is active (old 0x2ea)
constexpr std::uintptr_t placement_applied_offset = 0x29b;     // byte: the apply has run for the hold (old 0x2eb)
constexpr std::uintptr_t placement_blend_offset = 0x220;       // float (old 0x270)
constexpr std::uintptr_t placement_timer_offset = 0x234;       // float, seconds the hold has run (old 0x280)
constexpr std::uintptr_t placement_bone_offset = 0x23c;        // u32: 0x13 regular, 0xf switch (old 0x28c)
constexpr std::uintptr_t placement_stored_offset = 0x130;      // 16 bytes: the stored placement (old 0x120)
constexpr std::uintptr_t placement_stored_flag_offset = 0x290; // u32 (old 0x2e0)
constexpr std::uintptr_t context_dt_offset = 0x17ec;           // float: the physics step
constexpr std::uintptr_t context_ground_offset = 0x13d4;       // context: old bit 3, now bit 2 = on the ground
constexpr unsigned context_ground_mask = 0x4;
constexpr float hold_start_limit = 0.3f;                       // blend under this: the hold may start (old 0x5d2b198)
constexpr float hold_attach_limit = 0.2f;                      // blend under this: mode 2 is requested (old 0x5d26270)
constexpr unsigned foot_attach_hold_mode = 2;                  // the old X path's mode; the new apply uses 1
constexpr unsigned foot_attach_release_mode = 1;
constexpr std::uint32_t placement_bone_regular = 0x13;
// 601 route (0.13.0). Reading the physics-state chooser (old 0x43777b0, now 0x4745a70) in both exes: it is the same
// function apart from the X removal, which shifted every later bit of the packed animation->physics words down by
// one (old +0x13d4 bit 0 is now +0x13d0 bit 31, old +0x13d4 bit n is now bit n-1, and so on through +0x13e0). From
// the air (200) and on the landing tick (201) its first test, before any contact or bail test, is "+0x13d0 bit 31 and
// +0x13d4 bit 2 (old bits 0 and 3 of +0x13d4) -> state 601", the air-to-board placement state (its siblings 600-605
// are the board interaction states; 604 is the walk-on mount). Neither signal was set on any tick of two recorded
// sessions (riding, flips, dark catches, an underflip, walking), so 601 is dormant today. This experiment raises both
// on the dark landing ticks, before the chooser, and clears them after it. 0.13.0 play test: the chooser took 601 on
// all 12 routed landings and the game itself raised the "stay" signal (+0x13d4 bit 1, old bit 2) on the first 601
// tick, but 601 lasted 1-2 ticks and went to 300 every time. The 601 case of the chooser (identical in both exes)
// is: +0x13c4 bit 15 (the impact-bail request) -> 300; else +0x13d4 bit 1 -> stay; else +0x13c4 bit 27 -> 201;
// else 200, the pop. So 0.13.1 clears the bail package, the wipeout requests and the pending causes on every tick
// the game spends in 601 after a routed landing, before the chooser, and leaves the stay bit to the game.
constexpr std::uintptr_t context_route_a_offset = 0x13d0;
constexpr LONG context_route_a_mask = static_cast<LONG>(0x80000000u); // bit 31 (old +0x13d4 bit 0)
constexpr std::uintptr_t context_route_b_offset = 0x13d4;
constexpr LONG context_route_b_mask = 0x4;                            // bit 2 (old bit 3)
constexpr std::uint32_t route_physics_state = 601;
// 0.13.1 play test: with the package cleared, 601 lasted 4-13 s and never ended through the chooser. The game raises
// the stay bit (+0x13d4 bit 1) on the first 601 tick and never drops it (the old animation graph dropped it when
// the X hold ran out); the body flattened into the ground (601's own animation) and the board rolled back upright
// within 0.3 s. 0.13.2: after `route_stay` ticks in 601 the stay bit is cancelled on every tick before the chooser
// (the game re-raises it), so the chooser falls through to its next line: 200, the pop.
constexpr std::uintptr_t context_route_stay_offset = 0x13d4;
constexpr LONG context_route_stay_mask = 0x2;           // bit 1 (old bit 2): "stay in 601"
// 1.0: the sink is one tick (stay 0). 0.15.0 play tests, 98 routed landings: the pop's size went with the board's tilt
// at the 601 exit, not the speed or the landing; the sink animation tips the board onto its end a little more each
// tick (tilt 0.3 -> 0.5 -> 0.75 over three ticks), and a tilted board at the exit gave a hop. With one tick the pops
// were 15 big / 3 mid / 0 hops in 21 (three ticks: 5 / 1 / 1 in 7; two: 5 / 8 / 1 in 14; four or five: all hops).
constexpr unsigned route_stay_default = 0;               // ticks of 601 before the stay bit is cancelled
// 0.13.2 play test: the exit to 200 came on schedule every time, but on the pop's first tick the game saw the
// contact package still up and chose 300; with No Bail on (which swaps 300 back after the game has decided it) the
// pops were real (1-2 s flights, rises up to 6 m) but the forward speed collapsed on every one, probably the
// wipeout's handling landing before the swap. 0.13.3: for the pop's first `route_guard` ticks the bail package,
// wipeout requests and causes are cleared before the chooser, the 601 treatment carried into the flight.
constexpr unsigned route_guard_default = 12;             // ticks of the pop's flight guarded (0.2 s)
// Early dismounts (1.0). 0.15.0 play tests: of 98 dark landings, 6 were never routed because the game went from the
// air (200) straight to the dismount (504) with no landing tick (201) in between, so the landing window never opened.
// All six came off a shorter flight (0.55-0.63 s against 0.72-0.80 s for every routed flip; the player's 180s). With
// the catch on, an armed flight with the board still dark whose decision is 504 straight from the air is sent to 601
// instead, as if the landing tick had routed it; the chooser's 200 case would have returned 601 itself had the two
// route bits been up, and No Bail already returns a state other than the chooser's here (300 -> the current one).
// 0.13.3 play test: the guard kept 300 away, but every guarded pop ended in 504 the tick the guard stopped, and
// with No Bail on (which hooks the game's two bail-animation consumers) the pops were 1-2 s. So the animation goes
// into its bail on its own path and asks for the dismount afterwards. 0.13.4: in 601 and in the guard the
// animation's independent bail request (+0x13d4 bit 27, No Bail's animation_request_mask) is cancelled too.
// And the forward speed collapsed on every pop, No Bail or not (about 9 m/s before the landing, 0.6-6.8 after):
// 601's own flatten plants the feet. 0.13.4 "keep speed": the board body's horizontal velocity on the landing tick
// is saved, and at the 601 exit the missing part is added back to every physics body, the way the forward boost
// does (velocity +0x70, flag +0x60 | 8).
constexpr LONG context_bail_animation_mask = 0x08000000; // +0x13d4 bit 27
constexpr std::uintptr_t body_velocity_offset = 0x70;
constexpr std::uintptr_t body_flags_offset = 0x60;
constexpr std::size_t body_parts = 32;
constexpr unsigned route_live_limit = 180;               // ticks of 601 the window stays open at most (3 s), then it is let go and logged
bool dark_pop_air(std::uint32_t state) noexcept { return state >= 200 && state < 300; }
struct DarkPop {
    std::atomic<bool> hide_dpad{true}; // 3.1: the D-pad hidden from the game in the air
    std::atomic<bool> enabled{}, pressed{}, armed{};
    std::atomic<std::uintptr_t> board{};              // board body, published by the client tick
    std::atomic<std::uint64_t> pop_until{};
    std::atomic<unsigned> clears{};
    std::atomic<std::int64_t> pop_start{};
    // For the log: armed landings and pop landings since the process started.
    std::atomic<std::uint64_t> landings{}, pop_landings{};
    std::atomic<std::uint32_t> wanted{}, pop_landed{};
    std::atomic<float> board_up{}, pop_seconds{};
    std::atomic<unsigned> pop_ticks{};
    // Unpatch: the switch, the B bit's bookkeeping, the landing window (x_want) the placement hook reads,
    // the armed skater's context, and what the placement calls in the window saw (for the log).
    std::atomic<bool> unpatch{true}, unpatch_on{}, x_want{}, stage_was_clear{};
    std::atomic<std::uintptr_t> context{};
    std::atomic<unsigned> stage_clear_run{}, unpatch_ticks{}, last_unpatch_ticks{};
    std::atomic<unsigned> place_calls{}, place_latched_before{}, place_latched_after{}, place_flag{}, place_arg_flag{},
        place_side{}, place_forced{};
    std::atomic<unsigned> last_place_calls{}, last_place_latched_before{}, last_place_latched_after{}, last_place_flag{},
        last_place_arg_flag{}, last_place_side{}, last_place_forced{};
    std::atomic<bool> hooked{};
    // Hold port (0.11.0): the switches, whether the update hook and the setters are installed, the window the update
    // hook reads (the landing window, or the whole armed catch with "hold in the air"), the skater and bone of the
    // last request (for the release), and what the hold did at the last landing.
    std::atomic<bool> hold{true}, hold_want{}, hold_hooked{};
    // 0.13.0: one press, one pop (the D-pad must be let go before the next arming); the 601 route and its bookkeeping:
    // the bits are up this tick, the game is in 601 after an armed landing, ticks counted, what it left 601 to.
    std::atomic<bool> one_press{true}, need_release{}, route{true}, route_on{}, route_live{};
    std::atomic<unsigned> route_ticks{}, last_route_ticks{}, route_live_ticks{}, last_route_live_ticks{};
    std::atomic<unsigned> route_clears{}, last_route_clears{}; // 0.13.1: ticks in 601 the package was found raised
    std::atomic<unsigned> route_stay{route_stay_default};       // 0.13.2: 601 ticks before the stay bit is cancelled
    std::atomic<unsigned> route_stay_clears{}, last_route_stay_clears{};
    std::atomic<bool> route_let_go{};                           // the last 601 stay hit route_live_limit
    std::atomic<unsigned> route_guard{route_guard_default};     // 0.13.3: pop ticks guarded after a 601 exit
    std::atomic<unsigned> guard_left{}, guard_clears{}, last_guard_clears{};
    // 0.13.4 keep speed: the skater's physics bodies (published by the client tick), the landing-tick velocity,
    // and what was added back at the last exit (for the log).
    std::atomic<bool> keep_speed{true};
    std::atomic<std::uintptr_t> parts[body_parts]{};
    std::atomic<unsigned> part_count{};
    std::atomic<float> landing_vx{}, landing_vz{}, added_speed{}, last_added_speed{}, landing_speed{}, last_landing_speed{};
    std::atomic<unsigned> speed_bodies{}, last_speed_bodies{}, speed_reasserts{}, last_speed_reasserts{};
    // 1.0 early dismounts: the switch, whether the last dark landing was one, and how many have been caught.
    std::atomic<bool> catch_dismount{true}, last_caught{};
    std::atomic<std::uint64_t> catches{};
    // 1.9 timed-out landings: the switch, how many chooser calls in a row the board has been resting (|vertical velocity|
    // small) on the armed flight, whether the last dark landing was one, and how many have been caught.
    std::atomic<bool> catch_timeout{true}, last_timed_out{};
    // 2.0 hold the landing: the switch, whether the bit is up for this chooser call, calls it was raised on this flight,
    // the same at the last landing, and how many landings were staged by the game after a hold.
    std::atomic<bool> hold_landing{true}, hold_landing_up{};
    // 2.1 dark exit: the switch, the board transform taken at the 601 entry decision (and whether), and at the last exit:
    // whether it was written back, the board's up axis before and after.
    std::atomic<bool> dark_exit{true}, exit_have{}, exit_written{}, last_exit_written{};
    std::atomic<float> exit_snap[body_transform_floats]{};
    std::atomic<float> exit_up_before{9.0f}, exit_up_after{9.0f}, last_exit_up_before{9.0f}, last_exit_up_after{9.0f};
    // 2.1 full flight: the switch, whether a pop flight is being floored, its launch velocity and total time, the
    // countdown the game had at the 201 entry, calls floored, why it stopped; the same at the last pop landing.
    std::atomic<bool> full_flight{true}, flight_live{};
    std::atomic<float> flight_v0{}, flight_total{}, flight_found{9.0f}, last_flight_v0{}, last_flight_total{}, last_flight_found{9.0f};
    std::atomic<float> flight_launch{}; // 2.1.1: the pop's vertical velocity, the largest seen on its 200 ticks (the second air tick carries it)
    std::atomic<unsigned> flight_floors{}, flight_stop{}, last_flight_floors{}, last_flight_stop{}; // stop: 0 running, 1 time, 3 left 201, 4 cap (2 contact: dropped in 2.1.1)
    std::atomic<unsigned> hold_landing_ticks{}, last_hold_landing_ticks{};
    std::atomic<std::uint64_t> hold_landing_staged{};
    // 1.10 pop strength: the floor (m/s, 0 = off), and at the last landing: the vertical velocity found on the entry tick,
    // whether it was written, and the value read back at the next chooser call (9 = not read).
    std::atomic<float> pop_strength{0.0f}, strength_found{}, strength_seen{9.0f}, last_strength_found{}, last_strength_seen{9.0f};
    std::atomic<bool> strength_written{}, last_strength_written{};
    // 2.2 launch: pending from the 601 exit to the pop's first 200 chooser call; what was found and written there.
    std::atomic<bool> launch_pending{}, launch_state_ok{}, launch_written{};
    std::atomic<unsigned> launch_flag{}, jump_ticks{};
    std::atomic<float> jump_vy{9.0f}, launch_found[3]{};
    std::atomic<std::uint64_t> launch_writes{};
    // 2.4 launch (bodies): at the last pop, the board's vertical velocity found on the second air tick, the lift added
    // on each of the two lift ticks, bodies written, and lifts so far.
    std::atomic<float> lift_found{9.0f}, lift_added[2]{};
    std::atomic<unsigned> lift_bodies{}, lift_ticks{};
    std::atomic<std::uint64_t> lifts{};
    // 2.3 hold the flight: the switch; a restore pending from before the chooser to after it; what was found there.
    std::atomic<bool> hold_flight{true}, hold_restore{};
    std::atomic<std::uintptr_t> hold_context{};
    std::atomic<std::uint32_t> hold_saved_contacts{}, hold_saved_word{};
    std::atomic<unsigned> hold_blocks{}, last_hold_blocks{}, hold_contacts_max{}, last_hold_contacts_max{};
    std::atomic<std::uint32_t> hold_mask{};
    // 2.5 stay physical: ticks after the exit the jump-fired bit is hidden (0 = off); calls it was cleared on.
    std::atomic<unsigned> stay_physical{6}, stay_clears{}, last_stay_clears{};
    // 2.6 physics flight: when the jump-fired bit is already up on the pop's first ticks (2.5 test: every 180 landing;
    // the animation's air clip then never plays and 201 drags the body back to the ground, the "rubber band"), the
    // whole flight stays in 200 and lands by contact. The switch, whether the last pop was one, its length cap.
    std::atomic<bool> early_physics{true}, early_jump{}, last_early_jump{};
    // 2.7 re-aim: the switch; at the last pop, curve writes, the tick of the first, frames written, the state handed
    // to (201) or 0, what was written and what the vertical choice was between, the request's time offset, and why
    // nothing was written.
    std::atomic<bool> reaim{true};
    std::atomic<unsigned> reaim_writes{}, last_reaim_writes{}, reaim_tick{}, last_reaim_tick{}, reaim_frames{}, last_reaim_frames{}, reaim_from{}, last_reaim_from{};
    std::atomic<float> reaim_pos[3]{}, reaim_vel[3]{}, last_reaim_pos[3]{}, last_reaim_vel[3]{};
    std::atomic<float> reaim_body_vy{9.0f}, reaim_context_vy{9.0f}, reaim_time{}, last_reaim_body_vy{9.0f}, last_reaim_context_vy{9.0f}, last_reaim_time{};
    std::atomic<const char*> reaim_skip{""}, last_reaim_skip{""};
    std::atomic<std::uint64_t> strength_writes{};
    std::atomic<unsigned> rest_ticks{};
    std::atomic<std::uint64_t> timeout_catches{};
    // Feet-only hold (1.6): the switch, the footplant-tick hook, the local skater's board-placement object (seen by the
    // tick hook and the entry hook), and at the last landing: ticks whose attach requests were cleared, two-tick attaches requested.
    std::atomic<bool> feet{}, feet_hooked{};
    std::atomic<std::uintptr_t> placement_self{};
    std::atomic<unsigned> feet_skips{}, feet_requests{}, last_feet_skips{}, last_feet_requests{};
    // 1.6 orientation hold: the mover hook, the first sink tick's two direction vectors, and the counts for the log.
    std::atomic<bool> mover_hooked{}, mover_have{};
    std::atomic<unsigned> mover_captures{}, mover_held{}, last_mover_captures{}, last_mover_held{};
    // 1.7 stage probe samples (9 = none since the last chooser call) and the adaptive sink.
    std::atomic<float> stage_up[3]{};
    std::atomic<bool> adaptive{true};
    std::atomic<unsigned> adaptive_tick{}, last_adaptive_tick{};
    // 1.8 board freeze: the snapshot, whether it is taken, and for the log: ticks written, ticks the previous write was
    // found held at the next chooser call (axes within 0.05), ticks it was found overwritten.
    std::atomic<bool> freeze_have{};
    std::atomic<float> freeze[body_transform_floats]{};
    std::atomic<unsigned> freeze_writes{}, freeze_held{}, freeze_lost{}, last_freeze_writes{}, last_freeze_held{}, last_freeze_lost{};
    std::atomic<std::uint32_t> route_exit{};
    std::atomic<std::uint64_t> route_exits{};
    std::atomic<float> last_board_up{};    // the board's up axis on the decision tick of the last dark landing
    std::atomic<std::uintptr_t> attach_skater{};
    std::atomic<unsigned> hold_starts{}, hold_resets{}, hold_ticks{}, attach_requests{}, attach_bone{}, hold_ground{}, releases{};
    std::atomic<float> hold_blend{}, pop_board_up{};
    std::atomic<unsigned> last_hold_starts{}, last_hold_resets{}, last_hold_ticks{}, last_attach_requests{}, last_attach_bone{},
        last_hold_ground{};
    std::atomic<float> last_hold_blend{};
};
DarkPop& dark_pop() { static auto* value = new DarkPop; return *value; }
// The board's up axis, height part: about +1 riding normally, about -1 upside down. False if it cannot be read.
bool dark_pop_board_up(float& up) noexcept {
    const auto board = dark_pop().board.load(std::memory_order_relaxed);
    return board && read(board + dark_pop_board_up_offset, up) && std::isfinite(up) && up >= -1.5f && up <= 1.5f;
}
float dark_pop_seconds(std::int64_t ticks) noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? static_cast<float>(static_cast<double>(ticks) / static_cast<double>(f.QuadPart)) : 0.0f;
}
bool dark_pop_protecting(std::uint64_t now) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed)) return false;
    // 0.13.5: the 601 route borrows No Bail's protection (its hooks on the cause recorder, the impact producers and
    // the bail-animation consumers, through protected_owner) while the game is in 601 and through the pop's flight.
    // 0.13.4 play test: with No Bail off every guarded pop still ended in 504 the tick the guard stopped, with No
    // Bail on the same pops were 1-2 s; cancelling the request bits before the chooser was not enough.
    return d.route.load(std::memory_order_relaxed) &&
        (d.route_live.load(std::memory_order_relaxed) || now < d.pop_until.load(std::memory_order_relaxed));
}
void dark_pop_clear_package(std::uintptr_t context) noexcept {
    auto& d = dark_pop();
    bool any = false;
    for (const auto& flag : dark_pop_bail_flags) {
        unsigned word{};
        if (read(context + flag.offset, word) && (word & flag.mask)) {
            (void)cancel_request(context, flag.offset, static_cast<LONG>(flag.mask));
            any = true;
        }
    }
    if (any) d.clears.fetch_add(1, std::memory_order_relaxed);
}
bool dark_pop_set_flag(std::uintptr_t context, std::uintptr_t offset, LONG mask) noexcept {
    const auto address = context + offset;
    if (context < 0x10000 || context > highest - offset - sizeof(LONG) || (address & 3) != 0) return false;
    __try {
        _InterlockedOr(reinterpret_cast<volatile LONG*>(address), mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Sets the B bit (unpatch) while wanted; clears it once when it no longer is.
void dark_pop_unpatch_bit(std::uintptr_t context, bool want) noexcept {
    auto& d = dark_pop();
    if (want && d.unpatch.load(std::memory_order_relaxed)) {
        if (dark_pop_set_flag(context, context_signals_offset, dark_pop_unpatch_mask)) {
            d.unpatch_on.store(true, std::memory_order_relaxed);
            d.unpatch_ticks.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (d.unpatch_on.exchange(false, std::memory_order_relaxed)) {
        (void)cancel_request(context, context_signals_offset, dark_pop_unpatch_mask);
    }
}
void dark_pop_hold_window(bool want) noexcept;
// Closes the placement window: the hold let go, and the B bit cleared if it is up.
void dark_pop_window_off(std::uintptr_t selector) noexcept {
    auto& d = dark_pop();
    d.x_want.store(false, std::memory_order_relaxed);
    dark_pop_hold_window(false);
    if (!d.unpatch_on.load(std::memory_order_relaxed)) return;
    Owner owner;
    if (local_owner(selector, &Owner::selector, &owner)) dark_pop_unpatch_bit(owner.context, false);
}
bool dark_pop_read_byte(std::uintptr_t address, unsigned char& value) noexcept {
    return address >= 0x10000 && address < highest && read(address, value);
}
bool dark_pop_write_byte(std::uintptr_t address, unsigned char value) noexcept {
    if (address < 0x10000 || address >= highest) return false;
    __try {
        _InterlockedExchange8(reinterpret_cast<volatile char*>(address), static_cast<char>(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The board-placement entry hook. The original is called unchanged; in the landing window of an armed flight the
// local skater's placement object gets its latched byte forced first (the old X shortcut), and what the call saw is
// counted for the log. Four register arguments are passed through in case the function takes more than two.
using PlacementEntry = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::atomic<PlacementEntry> placement_original{};
std::uint64_t __fastcall placement_entry(std::uintptr_t self, std::uintptr_t arg, std::uintptr_t r8, std::uintptr_t r9) noexcept {
    auto& d = dark_pop();
    const auto original = placement_original.load(std::memory_order_acquire);
    bool mine = false;
    if (d.enabled.load(std::memory_order_relaxed)) {
        std::uintptr_t context{};
        const auto want = d.context.load(std::memory_order_relaxed);
        if (want && read(self + placement_context_offset, context) && context == want) d.placement_self.store(self, std::memory_order_relaxed);
    }
    if (d.enabled.load(std::memory_order_relaxed) && d.unpatch.load(std::memory_order_relaxed) &&
        d.x_want.load(std::memory_order_relaxed)) {
        std::uintptr_t context{};
        const auto want = d.context.load(std::memory_order_relaxed);
        if (want && read(self + placement_context_offset, context) && context == want) {
            mine = true;
            unsigned char latched{}, flag{}, arg_flag{}, side{};
            if (dark_pop_read_byte(self + placement_latched_offset, latched) && latched) d.place_latched_before.fetch_add(1, std::memory_order_relaxed);
            if (dark_pop_read_byte(self + placement_flag_offset, flag) && flag) d.place_flag.fetch_add(1, std::memory_order_relaxed);
            if (arg && dark_pop_read_byte(arg + placement_arg_flag_offset, arg_flag) && arg_flag) d.place_arg_flag.fetch_add(1, std::memory_order_relaxed);
            if (arg && dark_pop_read_byte(arg + placement_arg_side_offset, side) && side) d.place_side.fetch_add(1, std::memory_order_relaxed);
            if (dark_pop_write_byte(self + placement_latched_offset, 1)) d.place_forced.fetch_add(1, std::memory_order_relaxed);
            d.place_calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto result = original ? original(self, arg, r8, r9) : 0;
    if (mine) {
        unsigned char latched{};
        if (dark_pop_read_byte(self + placement_latched_offset, latched) && latched) d.place_latched_after.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
bool start_dark_pop_unpatch(std::uintptr_t base) noexcept {
    auto& d = dark_pop();
    if (d.hooked.load(std::memory_order_acquire)) return true;
    std::array<unsigned char, 32> actual{};
    if (!read(base + board_placement_entry_contract.rva, actual) || actual != board_placement_entry_contract.bytes) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Dark Pop unpatch is unavailable: the board-placement entry did not match its fingerprint.");
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + board_placement_entry_contract.rva);
    void* original{};
    auto status = hook_prepare(target, reinterpret_cast<void*>(&placement_entry), &original);
    if (status == HookOk && original) {
        placement_original.store(reinterpret_cast<PlacementEntry>(original), std::memory_order_release);
        status = hook_enable(target);
        if (status == HookOk) {
            d.hooked.store(true, std::memory_order_release);
            logging::write(logging::Level::info, logging::Channel::skater,
                "Dark Pop unpatch ready: board-placement entry hooked (B + latched in the dark landing window).");
            return true;
        }
    }
    (void)hook_remove(target);
    logging::log(logging::Level::warning, logging::Channel::skater,
        "Dark Pop unpatch hook setup failed (status {}); the experiment is unavailable.", static_cast<LONG>(status));
    return false;
}
bool dark_pop_read_float(std::uintptr_t address, float& value) noexcept {
    return address >= 0x10000 && address < highest - sizeof(float) && read(address, value) && std::isfinite(value);
}
bool dark_pop_write_u32(std::uintptr_t address, std::uint32_t value) noexcept {
    if (address < 0x10000 || address >= highest - sizeof(LONG) || (address & 3) != 0) return false;
    __try {
        _InterlockedExchange(reinterpret_cast<volatile LONG*>(address), static_cast<LONG>(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool dark_pop_write_float(std::uintptr_t address, float value) noexcept {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof bits);
    return dark_pop_write_u32(address, bits);
}
// At the 601 exit: the horizontal velocity the board body had on the landing tick, minus what it has now, added to
// every published physics body (the forward boost's write: velocity +0x70, flag +0x60 bit 3). Nothing is written
// unless every body reads back sane.
void dark_pop_keep_speed() noexcept {
    auto& d = dark_pop();
    const auto n = d.part_count.load(std::memory_order_relaxed);
    const auto board = d.parts[0].load(std::memory_order_relaxed);
    std::array<float, 3> now{};
    if (!n || !board || !read(board + body_velocity_offset, now) || !std::isfinite(now[0]) || !std::isfinite(now[2])) return;
    const float dx = d.landing_vx.load(std::memory_order_relaxed) - now[0];
    const float dz = d.landing_vz.load(std::memory_order_relaxed) - now[2];
    const float missing = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(missing) || missing < 0.5f || missing > 40.0f) return;
    std::array<std::array<float, 3>, body_parts> velocities{};
    std::array<std::uint32_t, body_parts> flags{};
    for (unsigned i = 0; i < n; ++i) {
        const auto body = d.parts[i].load(std::memory_order_relaxed);
        if (!body || !read(body + body_velocity_offset, velocities[i]) || !read(body + body_flags_offset, flags[i])) return;
        velocities[i][0] += dx;
        velocities[i][2] += dz;
        for (float component : velocities[i])
            if (!std::isfinite(component) || std::abs(component) > 1000.0f) return;
    }
    unsigned written = 0;
    for (unsigned i = 0; i < n; ++i) {
        const auto body = d.parts[i].load(std::memory_order_relaxed);
        if (dark_pop_write_float(body + body_velocity_offset, velocities[i][0]) &&
            dark_pop_write_float(body + body_velocity_offset + 4, velocities[i][1]) &&
            dark_pop_write_float(body + body_velocity_offset + 8, velocities[i][2]) &&
            dark_pop_write_u32(body + body_flags_offset, flags[i] | 8u))
            ++written;
    }
    d.added_speed.store(missing, std::memory_order_relaxed);
    d.speed_bodies.store(written, std::memory_order_relaxed);
}
// 2.4 launch (bodies): the keep-speed write for the vertical component. Adds `dy` to every physics body's vertical
// velocity at once; returns how many bodies took it.
// 2.7: rewrite the launch block's curve and the current jump request from where the body is and how fast it is going.
// Returns true when the curve was written; otherwise the reason is in reaim_skip.
bool dark_pop_reaim_curve(const Owner& owner, unsigned tick) noexcept {
    auto& d = dark_pop();
    const auto skip = [&](const char* why) { d.reaim_skip.store(why, std::memory_order_relaxed); return false; };
    std::uintptr_t block{}, state{}, state_block{}, request{};
    if (!read(owner.core + core_jump_block_offset, block) || !block) return skip("the core's launch block was not readable");
    if (!read(owner.core + core_anim_air_state_offset, state) || !state || !read(state + state_jump_block_offset, state_block) || state_block != block)
        return skip("the 201 state's launch block is not the core's");
    float dt{};
    if (!dark_pop_read_float(block + jump_block_curve_dt_offset, dt) || !std::isfinite(dt) || dt < 0.001f || dt > 0.1f) return skip("the curve's frame time was not readable");
    std::array<float, 4> pos{}, cvel{}, gravity{};
    if (!read(owner.context + context_position_offset, pos) || !read(owner.context + context_velocity_offset, cvel) || !read(block + jump_block_curve_gravity_offset, gravity))
        return skip("the position, velocity or gravity was not readable");
    for (std::size_t i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || !std::isfinite(cvel[i]) || !std::isfinite(gravity[i]) || std::abs(pos[i]) > 100000.0f || std::abs(cvel[i]) > 1000.0f || std::abs(gravity[i]) > 100.0f)
            return skip("the position, velocity or gravity read wrong");
    // Vertical: the board body's, when it reads as rising (the lift wrote the bodies; the context's own estimate spikes
    // for a few ticks after the lift); else the context's.
    float body_vy = 9.0f;
    std::array<float, 3> bv{};
    const auto board = d.parts[0].load(std::memory_order_relaxed);
    if (board && read(board + body_velocity_offset, bv) && std::isfinite(bv[1]) && std::abs(bv[1]) < 1000.0f) body_vy = bv[1];
    d.reaim_body_vy.store(body_vy, std::memory_order_relaxed);
    d.reaim_context_vy.store(cvel[1], std::memory_order_relaxed);
    const float vy = body_vy < 8.5f && body_vy > reaim_min_vy ? body_vy : cvel[1];
    if (!(vy > reaim_min_vy)) return skip("the body was not rising");
    const std::array<float, 4> vel{cvel[0], vy, cvel[2], 0.0f};
    // Frames left: the pop's own ballistic time (full flight's launch) minus the time since the pop, else from vy.
    LARGE_INTEGER clock{};
    QueryPerformanceCounter(&clock);
    const float elapsed = dark_pop_seconds(clock.QuadPart - d.pop_start.load(std::memory_order_relaxed));
    const float v0 = d.flight_launch.load(std::memory_order_relaxed);
    float left = v0 >= dark_pop_full_flight_min_v ? 2.0f * v0 / dark_pop_gravity - elapsed : 2.0f * vy / dark_pop_gravity;
    if (!std::isfinite(left) || left < 2.0f * vy / dark_pop_gravity) left = 2.0f * vy / dark_pop_gravity;
    const unsigned frames = std::clamp(static_cast<unsigned>(std::lround(left / dt)), reaim_min_frames, reaim_max_frames);
    // The curve: start where the context is, at the body's velocity.
    for (std::size_t i = 0; i < 4; ++i)
        if (!dark_pop_write_float(block + jump_block_curve_start_offset + 4 * i, pos[i]) || !dark_pop_write_float(block + jump_block_curve_velocity_offset + 4 * i, vel[i]))
            return skip("a curve write failed");
    // The request: keep its time offset t, and choose its start and velocity so that at t it is at pos with vel.
    float t{};
    if (read(block + jump_block_request_offset, request) && request && dark_pop_read_float(request + request_time_offset, t) && std::isfinite(t) && t >= 0.0f && t <= 3.0f) {
        std::array<float, 4> p0{}, v0r{};
        for (std::size_t i = 0; i < 3; ++i) {
            v0r[i] = vel[i] - gravity[i] * t;
            p0[i] = pos[i] - vel[i] * t + 0.5f * gravity[i] * t * t;
        }
        p0[3] = pos[3];
        bool ok = dark_pop_write_u32(request + request_frames_offset, frames);
        for (std::size_t i = 0; i < 4; ++i)
            ok = ok && dark_pop_write_float(request + request_start_offset + 4 * i, p0[i]) && dark_pop_write_float(request + request_velocity_offset + 4 * i, v0r[i]);
        d.reaim_time.store(ok ? t : -1.0f, std::memory_order_relaxed);
    } else {
        d.reaim_time.store(-1.0f, std::memory_order_relaxed);
    }
    if (d.reaim_writes.fetch_add(1, std::memory_order_relaxed) == 0) d.reaim_tick.store(tick, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) {
        d.reaim_pos[i].store(pos[i], std::memory_order_relaxed);
        d.reaim_vel[i].store(vel[i], std::memory_order_relaxed);
    }
    d.reaim_frames.store(frames, std::memory_order_relaxed);
    d.reaim_skip.store("", std::memory_order_relaxed);
    return true;
}
unsigned dark_pop_lift(float dy) noexcept {
    auto& d = dark_pop();
    const auto n = d.part_count.load(std::memory_order_relaxed);
    if (!n || !std::isfinite(dy) || dy <= 0.0f || dy > 40.0f) return 0;
    std::array<std::array<float, 3>, body_parts> velocities{};
    std::array<std::uint32_t, body_parts> flags{};
    for (unsigned i = 0; i < n; ++i) {
        const auto body = d.parts[i].load(std::memory_order_relaxed);
        if (!body || !read(body + body_velocity_offset, velocities[i]) || !read(body + body_flags_offset, flags[i])) return 0;
        velocities[i][1] += dy;
        for (float component : velocities[i])
            if (!std::isfinite(component) || std::abs(component) > 1000.0f) return 0;
    }
    unsigned written = 0;
    for (unsigned i = 0; i < n; ++i) {
        const auto body = d.parts[i].load(std::memory_order_relaxed);
        if (dark_pop_write_float(body + body_velocity_offset + 4, velocities[i][1]) && dark_pop_write_u32(body + body_flags_offset, flags[i] | 8u)) ++written;
    }
    return written;
}
// The game's foot-attach setters: `this` = skater, edx = mode (see foot_attach_*_contract); and its placement apply.
using FootAttach = void(__fastcall*)(std::uintptr_t, unsigned);
using PlacementApply = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
std::atomic<PlacementEntry> placement_update_original{};
std::atomic<FootAttach> foot_attach_regular{}, foot_attach_switch{};
std::atomic<PlacementApply> placement_apply{};
bool dark_pop_call_apply(std::uintptr_t self, std::uintptr_t arg) noexcept {
    const auto apply = placement_apply.load(std::memory_order_acquire);
    if (!apply) return false;
    __try {
        apply(self, arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool dark_pop_request_attach(std::uint32_t bone, std::uintptr_t skater, unsigned mode) noexcept {
    const auto setter = bone == placement_bone_regular ? foot_attach_regular.load(std::memory_order_acquire)
                                                       : foot_attach_switch.load(std::memory_order_acquire);
    if (!setter || skater < 0x10000 || skater >= highest) return false;
    __try {
        setter(skater, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Opens or closes the window the update hook works in. When it closes after mode 2 was requested, mode 1 (what the
// new apply requests on every hold tick) is requested once on the same bone, in case the request outlives the tick.
void dark_pop_hold_window(bool want) noexcept {
    auto& d = dark_pop();
    if (d.hold_want.exchange(want, std::memory_order_relaxed) == want || want) return;
    const auto skater = d.attach_skater.exchange(0, std::memory_order_relaxed);
    if (skater && d.attach_requests.load(std::memory_order_relaxed) &&
        dark_pop_request_attach(d.attach_bone.load(std::memory_order_relaxed), skater, foot_attach_release_mode))
        d.releases.fetch_add(1, std::memory_order_relaxed);
}
// ---- Feet-only hold: the footplant tick with its orientation held (1.6) --------------------------------------------------------------------------------
// Read from both exes (1.3): state 601's per-tick controller (old 0x4435e40, now 0x4803ea0) is the same function
// except for one fork the old one had right after `timer += dt`: if the removed signal X (old context+0x13d0 bit 29)
// was up, the body's position came from a cubic Bezier (the planned jump of 1.1), the output was handed to the step's
// mover, and the tick jumped straight to its epilogue, past the tuning reads, the footplant solver (old 0x44516e0,
// now 0x481f630) and the two foot-attach calls (a type-1 attach with an offset every tick, old 0x43a28d0 / now
// 0x4770c50, and a blended one when the solver says so). With X down, and in the current game always, the whole
// footplant runs: the solver plants the body where it stands and the attaches pull the feet to the deck and turn the
// board over to meet them. Two play-test sessions (8 Oct, 79 routed landings) tied the pop to exactly that: the board
// started rolling upright on sink tick 1-2 in half the landings, every landing where it had rolled by the exit gave a
// hop (0 of 40 big), every landing where it was still dark gave the 1-2 s pop (36 of 46 big); and the body planted
// for the sink where the original kept sliding. 1.2's hook on the placement apply (0x47ecd30) saw none of this: that
// function is not on 601's path (0 calls on every landing).
// 1.3 (play-tested, 42 landings): the hook on the footplant tick replaced the whole tick in a routed sink. The board
// then never moved (exit board-up -0.79 on every landing, flat through every sink) and there was no pop at all
// (flights 0.03-0.65 s, the board taking the exit alone, the skater staying down). With the tick running again the
// 1-2 s pops and the roll-over hops both came back. So the tick's two halves are two different things: the solver's
// plant is what loads the pop, and the per-tick attaches are what roll the board.
// 1.4: the original tick runs unchanged, and right after it returns, while routed, the request slots its attach
// setters filled on the skater are cleared: regular bone +0x59c0..+0x59fc (0x4770c20: +0x59c0, +0x59d0, +0x59f8,
// +0x59fc; 0x4770c50: +0x59e0, +0x59f0, +0x59f4) and switch bone +0x5a00..+0x5a3c (0x4770d10 / 0x4770d40, the same
// layout 0x40 on). Then the two-tick attach is requested so the feet ride the board where it lies. Nothing else in the
// tick is touched, so the plant, and with it the pop, stay.
// Observed in the original (frame by frame): feet on the trucks of the upside-down board, body sinking until the hips
// are near the ground, sliding on with the flip's speed, rising with the board still dark, the board rolling over
// only near the top.
// Probe columns (1.1): the footplant's plant point and up axis, the two values the old steering read, the placement's +0x140.
constexpr std::uintptr_t context_plant_offset = 0x4e0, context_plant_up_offset = 0x4f0, context_stick_offset = 0x19c0;
constexpr std::uintptr_t placement_start_offset = 0x140;
// The skater's foot-attach request slots the footplant tick's setters write (regular bone, then switch 0x40 on):
// three 16-byte vectors, a u32 type, two floats and a flag byte.
constexpr std::uintptr_t attach_slots_regular_offset = 0x59c0, attach_slots_switch_offset = 0x5a00;
constexpr std::uintptr_t attach_slots_dwords = 0x3c / 4; // +0x00..+0x3b, then the flag byte at +0x3c
using FootplantTick = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
std::atomic<FootplantTick> footplant_tick_original{};
// True while the feet-only hold owns 601 for this context: a routed sink (the tick only runs in 601).
bool feet_window(std::uintptr_t self) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed) || !d.feet.load(std::memory_order_relaxed)) return false;
    if (!(d.x_want.load(std::memory_order_relaxed) || d.route_live.load(std::memory_order_relaxed))) return false;
    std::uintptr_t context{};
    const auto want = d.context.load(std::memory_order_relaxed);
    return want && read(self + placement_context_offset, context) && context == want;
}
constexpr unsigned feet_attach_ticks = 2; // what the old apply requested every tick
void feet_request(std::uintptr_t self) noexcept {
    auto& d = dark_pop();
    std::uint32_t bone{};
    if (!read(self + placement_bone_offset, bone) || (bone != placement_bone_regular && bone != 0xf)) bone = placement_bone_regular;
    const auto skater = pointer(self, placement_skater_offset);
    if (skater && dark_pop_request_attach(bone, skater, feet_attach_ticks)) {
        d.feet_requests.fetch_add(1, std::memory_order_relaxed);
        d.attach_requests.fetch_add(1, std::memory_order_relaxed);
        d.attach_bone.store(bone, std::memory_order_relaxed);
        d.attach_skater.store(skater, std::memory_order_relaxed);
    }
}
// 1.4 (play-tested, 34 landings): with the tick running and its slots cleared, the pops were back (9 big, up to 2.7 s)
// and so was the roll (22 of 34 sinks, the same tick-1-2 climb). So neither the setters' slots, the solver's own
// writes (+0x270 plant point, +0x280 found, +0x284; read in full but for its tail) nor the epilogue (a real early-out
// with the jump flag down; 0x47e8cf8 is a `ret`) roll the board. What fits all three tests is the plant acting
// through any foot attach at all: the plant pulls the feet to the ground point under the board, the attach pins the
// feet to the board, and the board follows them over. 1.3 (attach, no plant): no roll. 1.4 and 1.0 (plant + an
// attach): roll. 1.5 requested no attach at all: the board still rolled (18 of 27), so the attaches were never it.
// Read since (the solver's tail and the mover, 0x47752a0): the solver builds a full pose, position AND an orientation
// from the plant normal and the foot direction, blended toward its target at two tuning rates (the steady tick-1-2
// climb), and hands it to the mover with the flag set; the mover writes it into the rig root (+0x49e0..+0x4a10) and
// poses the rig and its bodies, the board among them. The roll is the plant's orientation half.
// 1.6: a hook on the mover. On a routed sink tick for the local skater, the first call's two direction vectors
// (input +0x10, +0x20) are kept and written over every later call's, so the rig keeps the orientation the landing
// left it with while the position half of the plant, the spring (1.3), still runs. The two-tick attach of 1.4 is back.
// Clears both bones' attach request slots on the placement's skater. False if any write failed.
bool feet_clear_attach(std::uintptr_t self) noexcept {
    const auto skater = pointer(self, placement_skater_offset);
    if (!skater || skater >= highest - attach_slots_switch_offset - 0x40) return false;
    bool ok = true;
    for (const auto block : {attach_slots_regular_offset, attach_slots_switch_offset}) {
        for (std::uintptr_t i = 0; i < attach_slots_dwords; ++i) ok = dark_pop_write_u32(skater + block + 4 * i, 0) && ok;
        ok = dark_pop_write_byte(skater + block + 0x3c, 0) && ok;
    }
    return ok;
}
void __fastcall footplant_tick_hook(std::uintptr_t self, std::uintptr_t out) noexcept {
    auto& d = dark_pop();
    const auto original = footplant_tick_original.load(std::memory_order_acquire);
    if (d.enabled.load(std::memory_order_relaxed)) {
        std::uintptr_t context{};
        const auto want = d.context.load(std::memory_order_relaxed);
        if (want && read(self + placement_context_offset, context) && context == want) d.placement_self.store(self, std::memory_order_relaxed);
    }
    const bool mine = d.enabled.load(std::memory_order_relaxed) && d.route_live.load(std::memory_order_relaxed) &&
        d.placement_self.load(std::memory_order_relaxed) == self;
    float up{};
    if (mine) d.stage_up[0].store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
    if (original) original(self, out);
    if (mine) d.stage_up[1].store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
    if (feet_window(self) && feet_clear_attach(self)) {
        d.feet_skips.fetch_add(1, std::memory_order_relaxed);
        feet_request(self);
    }
}
// ---- The mover hook (1.6; a stage probe since 1.7) ----------------------------------------------------------------------------------------
using SkaterMover = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t);
std::atomic<SkaterMover> skater_mover_original{};
void __fastcall skater_mover_hook(std::uintptr_t skater, std::uintptr_t context, std::uintptr_t input, std::uint8_t flag) noexcept {
    auto& d = dark_pop();
    const auto original = skater_mover_original.load(std::memory_order_acquire);
    const bool mine = d.enabled.load(std::memory_order_relaxed) && d.route_live.load(std::memory_order_relaxed) &&
        context && context == d.context.load(std::memory_order_relaxed);
    // 1.6 (play-tested, 16 landings): holding the input's two direction vectors at the first sink tick's changed nothing
    // (12 of 16 rolled, same climb), so the mover's orientation input is not the roll either. 1.7: the hook only samples.
    if (mine && !d.mover_have.load(std::memory_order_relaxed)) {
        d.mover_have.store(true, std::memory_order_relaxed);
        d.mover_captures.fetch_add(1, std::memory_order_relaxed);
    }
    if (original) original(skater, context, input, flag);
    if (mine) {
        float up{};
        d.stage_up[2].store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
        d.mover_held.fetch_add(1, std::memory_order_relaxed);
    }
}
bool start_dark_pop_mover(std::uintptr_t base) noexcept {
    auto& d = dark_pop();
    if (d.mover_hooked.load(std::memory_order_acquire)) return true;
    std::array<unsigned char, 32> actual{};
    if (!read(base + skater_mover_contract.rva, actual) || actual != skater_mover_contract.bytes) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Dark Pop stage probe is unavailable: the skater mover did not match its fingerprint.");
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + skater_mover_contract.rva);
    void* original{};
    auto status = hook_prepare(target, reinterpret_cast<void*>(&skater_mover_hook), &original);
    if (status == HookOk && original) {
        skater_mover_original.store(reinterpret_cast<SkaterMover>(original), std::memory_order_release);
        status = hook_enable(target);
        if (status == HookOk) {
            d.mover_hooked.store(true, std::memory_order_release);
            logging::write(logging::Level::info, logging::Channel::skater,
                "Dark Pop stage probe ready: skater mover hooked (sampling the board's up axis after each move in a routed sink).");
            return true;
        }
    }
    (void)hook_remove(target);
    logging::log(logging::Level::warning, logging::Channel::skater,
        "Dark Pop stage probe hook setup failed (status {}); it is unavailable.", static_cast<LONG>(status));
    return false;
}
bool start_dark_pop_feet(std::uintptr_t base) noexcept {
    auto& d = dark_pop();
    if (d.feet_hooked.load(std::memory_order_acquire)) return true;
    std::array<unsigned char, 32> actual{};
    if (!read(base + footplant_tick_contract.rva, actual) || actual != footplant_tick_contract.bytes) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Dark Pop feet-only hold is unavailable: the footplant tick did not match its fingerprint.");
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + footplant_tick_contract.rva);
    void* original{};
    auto status = hook_prepare(target, reinterpret_cast<void*>(&footplant_tick_hook), &original);
    if (status == HookOk && original) {
        footplant_tick_original.store(reinterpret_cast<FootplantTick>(original), std::memory_order_release);
        status = hook_enable(target);
        if (status == HookOk) {
            d.feet_hooked.store(true, std::memory_order_release);
            logging::write(logging::Level::info, logging::Channel::skater,
                "Dark Pop feet-only hold ready: footplant tick hooked (in a routed sink its foot-attach requests are cleared after it runs and a two-tick attach requested; the plant stays).");
            return true;
        }
    }
    (void)hook_remove(target);
    logging::log(logging::Level::warning, logging::Channel::skater,
        "Dark Pop feet-only hold hook setup failed (status {}); it is unavailable.", static_cast<LONG>(status));
    return false;
}
// The board-placement update hook (Dark Pop hold). The game's update runs first: its prologue (stored placement,
// blend, continuation flag), then, only if the hold is already active or B plus the arg's +0x56 flag start it, the
// hold tail (applied = 1, timer += dt, apply). If the hold was neither active nor started, the old X block runs where
// the old code ran it, after the prologue: start the hold when the entry latched this tick and the blend is under
// 0.3 (active = 1, timer = 0) and run the same tail the game would have (applied = 1, timer += dt, apply), or else the
// old idle reset. Then, with the hold active, applied, latched and the blend under 0.2, foot-attach mode 2 is
// requested on the bone the entry chose: what the old apply's X path did and the new one no longer does. One
// deliberate difference: the old code skipped the apply on the hold's first tick unless context+0x13d4's "ground"
// output was set, and in today's recordings that output is never set at a dark landing, where the game decides the
// bail on the very next tick; so mode 2 is requested from the first active tick. The output's state is logged.
std::uint64_t __fastcall placement_update(std::uintptr_t self, std::uintptr_t arg, std::uintptr_t r8, std::uintptr_t r9) noexcept {
    auto& d = dark_pop();
    const auto original = placement_update_original.load(std::memory_order_acquire);
    bool mine = false, active_before = false;
    std::uintptr_t context{};
    if (d.enabled.load(std::memory_order_relaxed) && d.hold.load(std::memory_order_relaxed) &&
        d.hold_want.load(std::memory_order_relaxed)) {
        const auto want = d.context.load(std::memory_order_relaxed);
        if (want && read(self + placement_context_offset, context) && context == want) {
            mine = true;
            unsigned char active{};
            active_before = dark_pop_read_byte(self + placement_active_offset, active) && active;
        }
    }
    const auto result = original ? original(self, arg, r8, r9) : 0;
    if (!mine) return result;
    unsigned char active{}, applied{}, latched{};
    unsigned flags{};
    float blend{}, dt{}, timer{};
    const bool ground = read(context + context_ground_offset, flags) && (flags & context_ground_mask);
    const bool active_after = dark_pop_read_byte(self + placement_active_offset, active) && active;
    if (active_before) {
        d.hold_ticks.fetch_add(1, std::memory_order_relaxed);
    } else if (active_after) {
        d.hold_starts.fetch_add(1, std::memory_order_relaxed); // the game's own start (B and the arg's +0x56 flag)
        d.hold_ground.store(ground ? 1u : 0u, std::memory_order_relaxed);
        if (dark_pop_read_float(self + placement_blend_offset, blend)) d.hold_blend.store(blend, std::memory_order_relaxed);
    } else if (dark_pop_read_byte(self + placement_latched_offset, latched) && latched &&
               dark_pop_read_float(self + placement_blend_offset, blend) && blend < hold_start_limit) {
        // The old X start, then the tail the game skipped.
        if (dark_pop_write_byte(self + placement_active_offset, 1) && dark_pop_write_byte(self + placement_applied_offset, 1)) {
            (void)dark_pop_read_float(context + context_dt_offset, dt);
            (void)dark_pop_write_float(self + placement_timer_offset, dt);
            d.hold_blend.store(blend, std::memory_order_relaxed);
            d.hold_ground.store(ground ? 1u : 0u, std::memory_order_relaxed);
            d.hold_starts.fetch_add(1, std::memory_order_relaxed);
            (void)dark_pop_call_apply(self, arg);
        }
    } else {
        // The old idle reset: the stored placement, the blend, its flag and the continuation flag.
        for (std::uintptr_t part = 0; part < 16; part += 4) (void)dark_pop_write_float(self + placement_stored_offset + part, 0.0f);
        (void)dark_pop_write_float(self + placement_blend_offset, 0.0f);
        (void)dark_pop_write_u32(self + placement_stored_flag_offset, 0);
        (void)dark_pop_write_byte(self + placement_flag_offset, 0);
        d.hold_resets.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint32_t bone{};
    if (dark_pop_read_byte(self + placement_active_offset, active) && active &&
        dark_pop_read_byte(self + placement_applied_offset, applied) && applied &&
        dark_pop_read_byte(self + placement_latched_offset, latched) && latched &&
        dark_pop_read_float(self + placement_blend_offset, blend) && blend < hold_attach_limit &&
        dark_pop_read_float(self + placement_timer_offset, timer) && timer >= 0.0f &&
        read(self + placement_bone_offset, bone)) {
        const auto skater = pointer(self, placement_skater_offset);
        if (skater && dark_pop_request_attach(bone, skater, foot_attach_hold_mode)) {
            d.attach_requests.fetch_add(1, std::memory_order_relaxed);
            d.attach_bone.store(bone, std::memory_order_relaxed);
            d.attach_skater.store(skater, std::memory_order_relaxed);
        }
    }
    return result;
}
bool start_dark_pop_hold(std::uintptr_t base) noexcept {
    auto& d = dark_pop();
    if (d.hold_hooked.load(std::memory_order_acquire)) return true;
    struct Check { const game::build::Fingerprint& contract; const char* name; };
    const Check checks[] = {{board_placement_update_contract, "board-placement update"}, {board_placement_apply_contract, "board-placement apply"},
        {foot_attach_regular_contract, "regular foot-attach setter"}, {foot_attach_switch_contract, "switch foot-attach setter"}};
    for (const auto& check : checks) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + check.contract.rva, actual) || actual != check.contract.bytes) {
            logging::log(logging::Level::warning, logging::Channel::skater,
                "Dark Pop hold is unavailable: the {} did not match its fingerprint.", check.name);
            return false;
        }
    }
    foot_attach_regular.store(reinterpret_cast<FootAttach>(base + foot_attach_regular_contract.rva), std::memory_order_release);
    foot_attach_switch.store(reinterpret_cast<FootAttach>(base + foot_attach_switch_contract.rva), std::memory_order_release);
    placement_apply.store(reinterpret_cast<PlacementApply>(base + board_placement_apply_contract.rva), std::memory_order_release);
    auto* target = reinterpret_cast<void*>(base + board_placement_update_contract.rva);
    void* original{};
    auto status = hook_prepare(target, reinterpret_cast<void*>(&placement_update), &original);
    if (status == HookOk && original) {
        placement_update_original.store(reinterpret_cast<PlacementEntry>(original), std::memory_order_release);
        status = hook_enable(target);
        if (status == HookOk) {
            d.hold_hooked.store(true, std::memory_order_release);
            logging::write(logging::Level::info, logging::Channel::skater,
                "Dark Pop hold ready: board-placement update hooked, apply and foot-attach setters found (old start test and mode 2 in the dark landing window).");
            return true;
        }
    }
    (void)hook_remove(target);
    logging::log(logging::Level::warning, logging::Channel::skater,
        "Dark Pop hold hook setup failed (status {}); the hold is unavailable.", static_cast<LONG>(status));
    return false;
}
// Before the original selector: arming, the 601 route and its stay, the pop guard, and the placement window.
void dark_pop_before(std::uintptr_t selector, std::uint32_t current) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed)) { dark_pop_window_off(selector); return; }
    // One press, one pop: after a landing the D-pad has to be seen let go before a press counts again. Read on every
    // tick, so the release is seen on the ground too.
    const bool one_press = d.one_press.load(std::memory_order_relaxed);
    bool dpad = false;
    {
        dingosdk::ControllerInput pad;
        DingoSDKOverlayReadControllerInput(&pad, false);
        dpad = pad.available && (pad.buttons & dark_pop_dpad_bits);
    }
    if (!one_press) d.need_release.store(false, std::memory_order_relaxed);
    else if (!dpad) d.need_release.store(false, std::memory_order_relaxed);
    // 601 route: while the game sits in 601 after an armed landing, keep the placement window open (the unpatch's
    // B bit, the latched byte and the hold) so what 601 does is seen with the hold in place, not released on its
    // first tick. The window is let go after route_live_limit ticks or when the game leaves 601 (dark_pop_after).
    if (d.route_live.load(std::memory_order_relaxed)) {
        const auto ticks = d.route_live_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (current == route_physics_state && ticks <= route_live_limit) {
            Owner live;
            if (local_owner(selector, &Owner::selector, &live)) {
                if (ticks == 1) { // 1.10: did the pop-strength write survive to the first sink tick?
                    float vy{};
                    d.strength_seen.store(dark_pop_read_float(live.context + context_velocity_offset + 4, vy) && std::isfinite(vy) ? vy : 9.0f, std::memory_order_relaxed);
                }
                // 0.13.1: nothing of the landing impact reaches the chooser while the game is in 601.
                const auto before = d.clears.load(std::memory_order_relaxed);
                dark_pop_clear_package(live.context);
                if (d.clears.load(std::memory_order_relaxed) != before) d.route_clears.fetch_add(1, std::memory_order_relaxed);
                (void)cancel_wipeout_requests(live.context);
                (void)reset_pending_causes(live.causes);
                (void)cancel_request(live.context, animation_request_offset, context_bail_animation_mask);
                // 1.7: the footplant tick hook clears the tick's attach requests and requests the two-tick one; the mover hook only samples.
                // 1.8 board freeze (1.7 stage probe: the board's up axis never changes inside the footplant tick, the solver or
                // the mover; it changes between chooser calls, in the physics step, and its vertical velocity goes positive in
                // the sinks that roll: the plant poses the rig into the upside-down board and the physics step kicks it). With
                // Feet-only hold on, the board body's transform is taken on the first sink tick and written back, velocity
                // zeroed, on every later one; whether the write survives the step is counted at the next call.
                if (d.feet.load(std::memory_order_relaxed)) {
                    const auto board = d.parts[0].load(std::memory_order_relaxed);
                    std::array<float, body_transform_floats> now{};
                    if (board && read(board + body_transform_offset, now)) {
                        bool finite = true;
                        for (float v : now) finite = finite && std::isfinite(v);
                        if (!d.freeze_have.load(std::memory_order_relaxed)) {
                            if (finite) {
                                for (std::size_t i = 0; i < now.size(); ++i) d.freeze[i].store(now[i], std::memory_order_relaxed);
                                d.freeze_have.store(true, std::memory_order_relaxed);
                            }
                        } else {
                            if (d.freeze_writes.load(std::memory_order_relaxed)) {
                                float worst = 0.0f;
                                // 1.9: the three axis rows' x/y/z only; each row's 4th float is not part of the axis and 1.8 counted
                                // its drift as "overwritten" while the probe showed the axes identical every sink tick.
                                for (std::size_t i = 0; i < 12; ++i)
                                    if (i % 4 != 3) worst = std::max(worst, std::abs(now[i] - d.freeze[i].load(std::memory_order_relaxed)));
                                (worst <= freeze_held_tolerance ? d.freeze_held : d.freeze_lost).fetch_add(1, std::memory_order_relaxed);
                            }
                            bool ok = true;
                            for (std::size_t i = 0; i < now.size(); ++i)
                                ok = dark_pop_write_float(board + body_transform_offset + 4 * i, d.freeze[i].load(std::memory_order_relaxed)) && ok;
                            std::uint32_t flags{};
                            ok = dark_pop_write_float(board + body_velocity_offset, 0.0f) && dark_pop_write_float(board + body_velocity_offset + 4, 0.0f) &&
                                 dark_pop_write_float(board + body_velocity_offset + 8, 0.0f) && read(board + body_flags_offset, flags) &&
                                 dark_pop_write_u32(board + body_flags_offset, flags | 8u) && ok;
                            if (ok) d.freeze_writes.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                // 1.7 adaptive sink: every sink the board rolled over in gave a hop, every one it stayed dark in gave the pop,
                // and the roll starts on tick 1-2 and climbs. When the board's up axis has risen past -0.75 the sink is
                // ended on that tick, whatever the sink length: the pop fires while the board is still dark.
                float live_up{};
                const bool rising = d.adaptive.load(std::memory_order_relaxed) && dark_pop_board_up(live_up) && live_up > dark_pop_adaptive_above;
                if (rising && !d.adaptive_tick.load(std::memory_order_relaxed)) d.adaptive_tick.store(ticks, std::memory_order_relaxed);
                // 0.13.2: once 601 has had its stay, the stay bit comes down so the chooser falls through to 200.
                if (ticks > d.route_stay.load(std::memory_order_relaxed) || rising) {
                    unsigned word{};
                    if (read(live.context + context_route_stay_offset, word) && (word & context_route_stay_mask) &&
                        cancel_request(live.context, context_route_stay_offset, context_route_stay_mask))
                        d.route_stay_clears.fetch_add(1, std::memory_order_relaxed);
                }
                dark_pop_unpatch_bit(live.context, true);
                d.x_want.store(true, std::memory_order_relaxed);
                dark_pop_hold_window(true);
                return;
            }
        }
        // The game is still in 601 past the limit (or left it without a chooser call): let the window go and say so.
        d.route_live.store(false, std::memory_order_relaxed);
        d.mover_have.store(false, std::memory_order_relaxed);
        d.freeze_have.store(false, std::memory_order_relaxed);
        d.route_exit.store(current, std::memory_order_relaxed);
        d.route_let_go.store(true, std::memory_order_relaxed);
        d.last_route_live_ticks.store(ticks - 1, std::memory_order_relaxed);
        d.last_route_clears.store(d.route_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_route_stay_clears.store(d.route_stay_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.route_exits.fetch_add(1, std::memory_order_acq_rel);
    }
    // 2.2 launch: on the pop's first chooser call in 200 (the enter and one step have run), read the launch record
    // off the 200 state object and set its vertical velocity. The step integrates the record every tick after this.
    if (d.launch_pending.exchange(false, std::memory_order_relaxed)) {
        Owner popping;
        if (current == 200 && GetTickCount64() < d.pop_until.load(std::memory_order_relaxed) && local_owner(selector, &Owner::selector, &popping)) {
            std::uintptr_t state{}, air{};
            std::uint8_t flag{};
            std::array<float, 3> v{};
            if (read(popping.core + core_current_state_offset, state) && read(popping.core + core_air_state_offset, air) && state && state == air &&
                read(state + air_state_launch_flag_offset, flag) && read(state + air_state_launch_velocity_offset, v) &&
                std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])) {
                d.launch_state_ok.store(true, std::memory_order_relaxed);
                d.launch_flag.store(flag, std::memory_order_relaxed);
                for (std::size_t i = 0; i < 3; ++i) d.launch_found[i].store(v[i], std::memory_order_relaxed);
                const float launch = d.pop_strength.load(std::memory_order_relaxed);
                if (launch > 0.0f && flag == 1 && dark_pop_write_float(state + air_state_launch_velocity_offset + 4, launch)) {
                    d.launch_written.store(true, std::memory_order_relaxed);
                    d.launch_writes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    // 2.3 hold the flight: on a pop's 201 calls with the countdown still running, the chooser's contact inputs are
    // cleared for this call (put back in dark_pop_after), so only the countdown can end the flight.
    if ((current == 201 || current == 200) && GetTickCount64() < d.pop_until.load(std::memory_order_relaxed) && d.guard_left.load(std::memory_order_relaxed) <= d.route_guard.load(std::memory_order_relaxed)) {
        Owner held;
        float countdown{};
        std::uint32_t contacts{}, word{};
        const unsigned tick = d.route_guard.load(std::memory_order_relaxed) - d.guard_left.load(std::memory_order_relaxed);
        // 2.5: the pop's first ticks stay in 200 whatever the launch processor says. 2.6: when the jump fired on those
        // first ticks (the animation jumped before physics left the ground), the whole flight stays physical.
        const unsigned stay = d.stay_physical.load(std::memory_order_relaxed);
        const bool early = d.early_jump.load(std::memory_order_relaxed);
        // 2.7: with re-aim on, an early-jump pop is handed over after the Stay physical ticks like a clean one.
        const bool reaim = d.reaim.load(std::memory_order_relaxed);
        const bool physical = current == 200 && (tick < stay || (early && !reaim && d.early_physics.load(std::memory_order_relaxed) && tick < physics_flight_cap_ticks));
        // 2.3: in 201 the contact inputs are hidden while the countdown runs; 2.5: in 200 too while staying physical.
        if (local_owner(selector, &Owner::selector, &held) && read(held.context + context_contacts_offset, contacts) && read(held.context + 0x13c4, word)) {
            const bool in_countdown = current == 201 && dark_pop_read_float(held.context + context_gate_a_offset, countdown) && std::isfinite(countdown) && countdown > hold_flight_countdown_min;
            const bool hide_contacts = d.hold_flight.load(std::memory_order_relaxed) && ((physical && tick < stay) || in_countdown) && (contacts != 0 || (word & context_contact_bits) != 0);
            const bool hide_jump = physical && (word & context_jump_fired_bit) != 0;
            if (hide_jump && tick <= 1 && stay) d.early_jump.store(true, std::memory_order_relaxed);
            if (hide_contacts || hide_jump) {
                std::uint32_t mask = (hide_contacts ? context_contact_bits : 0u) | (hide_jump ? context_jump_fired_bit : 0u);
                if (hide_contacts && contacts > d.hold_contacts_max.load(std::memory_order_relaxed)) d.hold_contacts_max.store(contacts, std::memory_order_relaxed);
                if ((!hide_contacts || dark_pop_write_u32(held.context + context_contacts_offset, 0u)) && dark_pop_write_u32(held.context + 0x13c4, word & ~mask)) {
                    d.hold_context.store(held.context, std::memory_order_relaxed);
                    d.hold_saved_contacts.store(hide_contacts ? contacts : 0u, std::memory_order_relaxed);
                    d.hold_saved_word.store(word, std::memory_order_relaxed);
                    d.hold_mask.store(mask, std::memory_order_relaxed);
                    d.hold_restore.store(true, std::memory_order_relaxed);
                    if (hide_contacts) d.hold_blocks.fetch_add(1, std::memory_order_relaxed);
                    if (hide_jump) d.stay_clears.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // 2.7 re-aim: from the first 200 call after the Stay physical ticks until the chooser takes 201, the
            // animation's flight curve is rewritten from where the body is (the writes are idempotent; the curve is
            // not read in 200). The jump-fired bit stays as the game set it, so the chooser can hand over.
            if (reaim && early && stay && current == 200 && tick >= stay) (void)dark_pop_reaim_curve(held, tick);
            if (reaim && early && current == 201 && d.reaim_writes.load(std::memory_order_relaxed) && !d.reaim_from.load(std::memory_order_relaxed))
                d.reaim_from.store(201, std::memory_order_relaxed);
        }
    }
    // 2.1 full flight: floor the landing countdown on the pop's known-air ticks.
    if (d.full_flight.load(std::memory_order_relaxed) && GetTickCount64() < d.pop_until.load(std::memory_order_relaxed) &&
        d.flight_stop.load(std::memory_order_relaxed) == 0) {
        if (current == 200 && !d.flight_live.load(std::memory_order_relaxed)) {
            // 2.1.1: the launch velocity is the largest vertical velocity the board shows on the pop's 200 ticks (the
            // second air tick carries the pop, 1.8). The 201 entry's own sample dips for one tick and the board is not
            // ballistic in 201 (2.1 test: every flight stopped on "contact" within 0.12 s), so neither is used.
            const auto board = d.parts[0].load(std::memory_order_relaxed);
            std::array<float, 3> v{};
            if (board && read(board + body_velocity_offset, v) && std::isfinite(v[1]) && v[1] > d.flight_launch.load(std::memory_order_relaxed))
                d.flight_launch.store(v[1], std::memory_order_relaxed);
        } else if (current == 201) {
            Owner flying;
            if (local_owner(selector, &Owner::selector, &flying)) {
                LARGE_INTEGER clock{};
                QueryPerformanceCounter(&clock);
                const float elapsed = dark_pop_seconds(clock.QuadPart - d.pop_start.load(std::memory_order_relaxed));
                if (!d.flight_live.load(std::memory_order_relaxed)) {
                    const float v0 = d.flight_launch.load(std::memory_order_relaxed);
                    float found{};
                    d.flight_found.store(dark_pop_read_float(flying.context + context_gate_a_offset, found) ? found : 9.0f, std::memory_order_relaxed);
                    d.flight_v0.store(v0, std::memory_order_relaxed);
                    d.flight_total.store(std::min(dark_pop_full_flight_cap, 2.0f * v0 / dark_pop_gravity), std::memory_order_relaxed);
                    d.flight_live.store(true, std::memory_order_relaxed);
                    if (v0 < dark_pop_full_flight_min_v) d.flight_stop.store(4, std::memory_order_relaxed);
                }
                if (d.flight_stop.load(std::memory_order_relaxed) == 0) {
                    const float left = d.flight_total.load(std::memory_order_relaxed) - elapsed;
                    if (left <= 0.0f) d.flight_stop.store(1, std::memory_order_relaxed);
                    else {
                        float have{};
                        if (dark_pop_read_float(flying.context + context_gate_a_offset, have) && std::isfinite(have) && have < left &&
                            dark_pop_write_float(flying.context + context_gate_a_offset, left))
                            d.flight_floors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        } else if (d.flight_live.load(std::memory_order_relaxed)) {
            d.flight_stop.store(3, std::memory_order_relaxed);
        }
    }
    // 0.13.3 pop guard: the pop's first ticks get the 601 treatment, so the game cannot bail it on the way up.
    if (d.guard_left.load(std::memory_order_relaxed) > 0) {
        if (dark_pop_air(current)) {
            Owner guarded;
            if (local_owner(selector, &Owner::selector, &guarded)) {
                const auto before = d.clears.load(std::memory_order_relaxed);
                dark_pop_clear_package(guarded.context);
                if (d.clears.load(std::memory_order_relaxed) != before) d.guard_clears.fetch_add(1, std::memory_order_relaxed);
                (void)cancel_wipeout_requests(guarded.context);
                (void)reset_pending_causes(guarded.causes);
                (void)cancel_request(guarded.context, animation_request_offset, context_bail_animation_mask);
            }
            // 0.13.5: the 200 entry re-seeds the body velocities after the exit write (0.13.4: 8 m/s added, 0.9 m/s a
            // tick later), so the landing speed is re-asserted on every guard tick it has fallen under 70% of.
            if (d.keep_speed.load(std::memory_order_relaxed)) {
                std::array<float, 3> v{};
                const auto board = d.parts[0].load(std::memory_order_relaxed);
                const float want = d.landing_speed.load(std::memory_order_relaxed);
                if (want > 1.0f && board && read(board + body_velocity_offset, v) && std::isfinite(v[0]) && std::isfinite(v[2]) &&
                    std::sqrt(v[0] * v[0] + v[2] * v[2]) < 0.7f * want) {
                    dark_pop_keep_speed();
                    if (d.speed_bodies.load(std::memory_order_relaxed)) d.speed_reasserts.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // 2.4 launch (bodies): the handover seeds the bodies' velocity on the pop's first step (it shows on the second
            // chooser call, 1.8); on that call and the next, lift every body's vertical velocity up to the launch.
            {
                const unsigned tick = d.route_guard.load(std::memory_order_relaxed) - d.guard_left.load(std::memory_order_relaxed);
                const float launch = d.pop_strength.load(std::memory_order_relaxed);
                if (launch > 0.0f && (tick == 1 || tick == 2)) {
                    std::array<float, 3> v{};
                    const auto board = d.parts[0].load(std::memory_order_relaxed);
                    if (board && read(board + body_velocity_offset, v) && std::isfinite(v[1])) {
                        if (tick == 1) d.lift_found.store(v[1], std::memory_order_relaxed);
                        const float want = tick == 1 ? launch : launch - dark_pop_gravity * 0.0167f;
                        const float dy = want - v[1];
                        if (dy > 0.25f) {
                            const unsigned bodies = dark_pop_lift(dy);
                            if (bodies) {
                                d.lift_added[tick - 1].store(dy, std::memory_order_relaxed);
                                d.lift_bodies.store(bodies, std::memory_order_relaxed);
                                d.lift_ticks.fetch_add(1, std::memory_order_relaxed);
                                d.lifts.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
            d.guard_left.fetch_sub(1, std::memory_order_relaxed);
        } else {
            d.guard_left.store(0, std::memory_order_relaxed);
        }
    }
    if (!dark_pop_air(current)) {
        dark_pop_window_off(selector);
        // On the ground every flight starts over.
        if (d.pressed.load(std::memory_order_relaxed) || d.armed.load(std::memory_order_relaxed)) {
            d.pressed.store(false, std::memory_order_relaxed);
            d.armed.store(false, std::memory_order_relaxed);
        }
        return;
    }
    Owner owner;
    if (!local_owner(selector, &Owner::selector, &owner)) return;
    // 3.1: in the air the game's own polls see the D-pad let go (input_capture.cpp), so no Quick Drop radial opens.
    if (d.hide_dpad.load(std::memory_order_relaxed)) DingoSDKOverlayDarkPopAir();
    if (!d.pressed.load(std::memory_order_relaxed) && dpad && !d.need_release.load(std::memory_order_relaxed))
        d.pressed.store(true, std::memory_order_relaxed);
    float up{};
    const bool board_known = dark_pop_board_up(up);
    if (board_known) d.board_up.store(up, std::memory_order_relaxed);
    if (d.armed.load(std::memory_order_relaxed)) {
        if (board_known && up > dark_pop_upright_above) d.armed.store(false, std::memory_order_relaxed); // turned back over
    } else if (d.pressed.load(std::memory_order_relaxed) && board_known && up < dark_pop_dark_below) {
        d.armed.store(true, std::memory_order_relaxed);
        d.clears.store(0, std::memory_order_relaxed);
        d.unpatch_ticks.store(0, std::memory_order_relaxed);
        d.stage_clear_run.store(0, std::memory_order_relaxed);
        d.stage_was_clear.store(false, std::memory_order_relaxed);
        d.context.store(owner.context, std::memory_order_relaxed);
        for (auto* counter : {&d.place_calls, &d.place_latched_before, &d.place_latched_after, &d.place_flag,
                &d.place_arg_flag, &d.place_side, &d.place_forced, &d.hold_starts, &d.hold_resets, &d.hold_ticks,
                &d.attach_requests, &d.attach_bone, &d.hold_ground, &d.feet_skips, &d.feet_requests, &d.mover_captures, &d.mover_held})
            counter->store(0, std::memory_order_relaxed);
        d.hold_blend.store(0.0f, std::memory_order_relaxed);
        d.attach_skater.store(0, std::memory_order_relaxed);
        d.mover_have.store(false, std::memory_order_relaxed);
        for (auto& sample : d.stage_up) sample.store(9.0f, std::memory_order_relaxed);
        d.route_ticks.store(0, std::memory_order_relaxed);
        d.hold_landing_ticks.store(0, std::memory_order_relaxed);
    }
    // 1.9: how long the board has been resting on the armed flight (for the timed-out landing catch in dark_pop_after).
    if (d.armed.load(std::memory_order_relaxed)) {
        std::array<float, 3> v{};
        const auto board = d.parts[0].load(std::memory_order_relaxed);
        if (board && read(board + body_velocity_offset, v) && std::isfinite(v[1]) && std::abs(v[1]) < dark_pop_rest_speed)
            d.rest_ticks.fetch_add(1, std::memory_order_relaxed);
        else
            d.rest_ticks.store(0, std::memory_order_relaxed);
    } else {
        d.rest_ticks.store(0, std::memory_order_relaxed);
    }
    bool landing = false; // the dark landing ticks: the placement window
    if (d.armed.load(std::memory_order_relaxed)) {
        unsigned stage{};
        const bool staged = read(owner.context + 0x13c8, stage) && (stage & dark_pop_landing_stage);
        // The stage bit is also on during the catch; the landing is its return after a clear run.
        if (!staged) {
            if (d.stage_clear_run.fetch_add(1, std::memory_order_relaxed) + 1 >= dark_pop_stage_clear_ticks)
                d.stage_was_clear.store(true, std::memory_order_relaxed);
        }
        if (current == 201 && !staged && d.hold_landing.load(std::memory_order_relaxed) &&
            d.hold_landing_ticks.load(std::memory_order_relaxed) < dark_pop_landing_hold_limit &&
            d.rest_ticks.load(std::memory_order_relaxed) >= 1) {
            unsigned sequence{};
            float hold_up{};
            if (read(owner.context + 0x13c4, sequence) && (sequence & dark_pop_landing_sequence_mask) == dark_pop_landing_sequence_mask &&
                dark_pop_board_up(hold_up) && hold_up < dark_pop_dark_below &&
                dark_pop_set_flag(owner.context, context_landing_hold_offset, context_landing_hold_mask)) {
                d.hold_landing_up.store(true, std::memory_order_relaxed);
                d.hold_landing_ticks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (current == 201 && staged && d.stage_was_clear.load(std::memory_order_relaxed)) {
            landing = true;
            if (d.hold_landing_ticks.load(std::memory_order_relaxed)) d.hold_landing_staged.fetch_add(1, std::memory_order_relaxed);
            if (d.route.load(std::memory_order_relaxed)) {
                // 601 route: both chooser inputs up for this decision; dark_pop_after takes them down again.
                const bool a = dark_pop_set_flag(owner.context, context_route_a_offset, context_route_a_mask);
                const bool b = dark_pop_set_flag(owner.context, context_route_b_offset, context_route_b_mask);
                if (a || b) {
                    d.route_on.store(true, std::memory_order_relaxed);
                    d.route_ticks.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    dark_pop_unpatch_bit(owner.context, landing);
    d.x_want.store(landing, std::memory_order_relaxed);
    dark_pop_hold_window(landing);
}
// After the original selector (and No Bail's wipeout swap); returns the state to use.
std::uint32_t dark_pop_after(std::uintptr_t selector, std::uint32_t current, std::uint32_t chosen) noexcept {
    auto& d = dark_pop();
    if (!d.enabled.load(std::memory_order_relaxed)) {
        d.pop_until.store(0, std::memory_order_relaxed);
        return chosen;
    }
    const auto now = GetTickCount64();
    const bool armed = d.armed.load(std::memory_order_relaxed);
    const bool popping = now < d.pop_until.load(std::memory_order_relaxed);
    // 2.3 hold the flight: the contact inputs cleared before this decision go back as they were.
    if (d.hold_restore.exchange(false, std::memory_order_relaxed)) {
        const auto context = d.hold_context.load(std::memory_order_relaxed);
        std::uint32_t word{};
        if (context) {
            const auto mask = d.hold_mask.load(std::memory_order_relaxed);
            if (mask & context_contact_bits) (void)dark_pop_write_u32(context + context_contacts_offset, d.hold_saved_contacts.load(std::memory_order_relaxed));
            if (read(context + 0x13c4, word)) (void)dark_pop_write_u32(context + 0x13c4, word | (d.hold_saved_word.load(std::memory_order_relaxed) & mask));
        }
    }
    // 601 route: the bits raised before this decision come down whatever it was.
    if (d.route_on.exchange(false, std::memory_order_relaxed)) {
        Owner raised;
        if (local_owner(selector, &Owner::selector, &raised)) {
            (void)cancel_request(raised.context, context_route_a_offset, context_route_a_mask);
            (void)cancel_request(raised.context, context_route_b_offset, context_route_b_mask);
        }
    }
    if (d.hold_landing_up.exchange(false, std::memory_order_relaxed)) {
        Owner held;
        if (local_owner(selector, &Owner::selector, &held)) (void)cancel_request(held.context, context_landing_hold_offset, context_landing_hold_mask);
    }
    // Leaving 601: note where to, and let the window go.
    if (d.route_live.load(std::memory_order_relaxed) && current == route_physics_state && chosen != route_physics_state) {
        d.route_live.store(false, std::memory_order_relaxed);
        d.mover_have.store(false, std::memory_order_relaxed);
        d.last_mover_captures.store(d.mover_captures.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_mover_held.store(d.mover_held.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_adaptive_tick.store(d.adaptive_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_freeze_writes.store(d.freeze_writes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_freeze_held.store(d.freeze_held.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_freeze_lost.store(d.freeze_lost.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_strength_found.store(d.strength_found.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_strength_written.store(d.strength_written.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_strength_seen.store(d.strength_seen.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_exit_written.store(d.exit_written.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_exit_up_before.store(d.exit_up_before.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_exit_up_after.store(d.exit_up_after.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.freeze_have.store(false, std::memory_order_relaxed);
        d.route_exit.store(chosen, std::memory_order_relaxed);
        d.last_route_live_ticks.store(d.route_live_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_route_clears.store(d.route_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_route_stay_clears.store(d.route_stay_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.route_let_go.store(false, std::memory_order_relaxed);
        if (chosen == 200) {
            // 2.1 dark exit: the board put back as it landed (flat dark, still) before the 200 entry takes over.
            {
                float up{};
                d.exit_up_before.store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
                bool written = false;
                const auto board = d.parts[0].load(std::memory_order_relaxed);
                if (d.dark_exit.load(std::memory_order_relaxed) && d.exit_have.load(std::memory_order_relaxed) && board) {
                    bool ok = true;
                    for (std::size_t i = 0; i < body_transform_floats; ++i)
                        ok = dark_pop_write_float(board + body_transform_offset + 4 * i, d.exit_snap[i].load(std::memory_order_relaxed)) && ok;
                    std::uint32_t flags{};
                    ok = dark_pop_write_float(board + body_velocity_offset, 0.0f) && dark_pop_write_float(board + body_velocity_offset + 4, 0.0f) &&
                         dark_pop_write_float(board + body_velocity_offset + 8, 0.0f) && read(board + body_flags_offset, flags) &&
                         dark_pop_write_u32(board + body_flags_offset, flags | 8u) && ok;
                    written = ok;
                }
                d.exit_written.store(written, std::memory_order_relaxed);
                d.exit_up_after.store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
                // 2.1.1: the sink line's copies above were taken before this write (2.1 logged "nothing to write" on
                // every landing); take them again now.
                d.last_exit_written.store(written, std::memory_order_relaxed);
                d.last_exit_up_before.store(d.exit_up_before.load(std::memory_order_relaxed), std::memory_order_relaxed);
                d.last_exit_up_after.store(d.exit_up_after.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            // 2.2 launch: what the 200 enter is about to read, and arm the record write for the pop's first 200 call.
            {
                Owner leaving;
                std::uint32_t ticks{}; float jvy{};
                const bool have = local_owner(selector, &Owner::selector, &leaving);
                d.jump_ticks.store(have && read(leaving.context + context_jump_ticks_offset, ticks) ? ticks : 0xffffffffu, std::memory_order_relaxed);
                d.jump_vy.store(have && dark_pop_read_float(leaving.context + context_jump_velocity_offset + 4, jvy) && std::isfinite(jvy) ? jvy : 9.0f, std::memory_order_relaxed);
                d.launch_state_ok.store(false, std::memory_order_relaxed);
                d.launch_written.store(false, std::memory_order_relaxed);
                d.launch_flag.store(9, std::memory_order_relaxed);
                for (auto& v : d.launch_found) v.store(9.0f, std::memory_order_relaxed);
                d.launch_pending.store(true, std::memory_order_relaxed);
                d.hold_blocks.store(0, std::memory_order_relaxed);
                d.hold_contacts_max.store(0, std::memory_order_relaxed);
                d.stay_clears.store(0, std::memory_order_relaxed);
                d.early_jump.store(false, std::memory_order_relaxed);
                d.reaim_writes.store(0, std::memory_order_relaxed);
                d.reaim_tick.store(0, std::memory_order_relaxed);
                d.reaim_frames.store(0, std::memory_order_relaxed);
                d.reaim_from.store(0, std::memory_order_relaxed);
                d.reaim_time.store(-1.0f, std::memory_order_relaxed);
                d.reaim_body_vy.store(9.0f, std::memory_order_relaxed);
                d.reaim_context_vy.store(9.0f, std::memory_order_relaxed);
                d.reaim_skip.store("the handover never came", std::memory_order_relaxed);
                d.lift_found.store(9.0f, std::memory_order_relaxed);
                d.lift_added[0].store(0.0f, std::memory_order_relaxed);
                d.lift_added[1].store(0.0f, std::memory_order_relaxed);
                d.lift_bodies.store(0, std::memory_order_relaxed);
                d.lift_ticks.store(0, std::memory_order_relaxed);
            }
            // 2.1 full flight: armed for this pop; the launch velocity is read at the 201 entry (dark_pop_before).
            d.flight_live.store(false, std::memory_order_relaxed);
            d.flight_v0.store(0.0f, std::memory_order_relaxed);
            d.flight_total.store(0.0f, std::memory_order_relaxed);
            d.flight_found.store(9.0f, std::memory_order_relaxed);
            d.flight_launch.store(0.0f, std::memory_order_relaxed);
            d.flight_floors.store(0, std::memory_order_relaxed);
            d.flight_stop.store(0, std::memory_order_relaxed);
            // Keep speed: add back what 601 took from the horizontal velocity, to every physics body at once.
            d.added_speed.store(0.0f, std::memory_order_relaxed);
            d.speed_bodies.store(0, std::memory_order_relaxed);
            d.speed_reasserts.store(0, std::memory_order_relaxed);
            if (d.keep_speed.load(std::memory_order_relaxed)) dark_pop_keep_speed();
            d.last_added_speed.store(d.added_speed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.last_landing_speed.store(d.landing_speed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.last_speed_bodies.store(d.speed_bodies.load(std::memory_order_relaxed), std::memory_order_relaxed);
            // 601's exit to the air is the pop: track its flight like a pop the landing tick chose, and guard it.
            d.guard_left.store(d.route_guard.load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.guard_clears.store(0, std::memory_order_relaxed);
            d.pop_ticks.store(0, std::memory_order_relaxed);
            LARGE_INTEGER clock{};
            QueryPerformanceCounter(&clock);
            d.pop_start.store(clock.QuadPart, std::memory_order_relaxed);
            d.pop_until.store(now + dark_pop_pop_ms, std::memory_order_relaxed);
        }
        d.route_exits.fetch_add(1, std::memory_order_acq_rel);
        return chosen;
    }
    if (!(dark_pop_air(current) && (armed || popping))) return chosen;
    Owner owner;
    if (!local_owner(selector, &Owner::selector, &owner)) return chosen;
    // 1.0 early dismounts: an armed flight the game wants to end in 504 straight from the air, the board still dark,
    // is routed to 601 as the landing tick would have been (see the early-dismounts note above).
    bool caught = false;
    if (armed && chosen == offboard_physics_state && d.route.load(std::memory_order_relaxed) &&
        d.catch_dismount.load(std::memory_order_relaxed)) {
        float up{};
        if (dark_pop_board_up(up) && up < dark_pop_dark_below) {
            chosen = route_physics_state;
            caught = true;
            d.catches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // 1.9 timed-out landings. The landing window opens when the game raises the landing stage (+0x13c8 bit 26) in 201,
    // at the end of a short sequence in +0x13c4 (bits 0x300000, then 0x880000, then the stage). 201 is capped at ~50
    // ticks; when the cap comes first the game drops to 200 with the sequence unfinished, the board (already resting
    // upside down on the ground) rolls over in physics and the landing rides away with no pop. 8 Oct session: 10 of 19
    // armed landings, every nollie 360 flip among them (a longer animation). An armed flight the game wants to send
    // 201 -> 200 with the board dark and resting is routed to 601 like the landing tick would have been.
    bool timed_out = false;
    if (armed && !caught && current == 201 && chosen == 200 && d.route.load(std::memory_order_relaxed) &&
        d.catch_timeout.load(std::memory_order_relaxed) && d.rest_ticks.load(std::memory_order_relaxed) >= dark_pop_rest_ticks) {
        float up{};
        if (dark_pop_board_up(up) && up < dark_pop_dark_below) {
            chosen = route_physics_state;
            timed_out = true;
            d.timeout_catches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const bool pops = current != 200 && chosen == 200;
    const bool leaves_air = !dark_pop_air(chosen);
    if (armed) {
        if (!pops && !leaves_air) return chosen;
        d.last_caught.store(caught, std::memory_order_relaxed);
        d.last_timed_out.store(timed_out, std::memory_order_relaxed);
        d.last_hold_landing_ticks.store(d.hold_landing_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.armed.store(false, std::memory_order_relaxed);
        d.pressed.store(false, std::memory_order_relaxed);
        if (d.one_press.load(std::memory_order_relaxed)) d.need_release.store(true, std::memory_order_relaxed);
        d.last_route_ticks.store(d.route_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        if (chosen == route_physics_state) {
            // 2.1 dark exit: the board as it landed, for the exit tick.
            d.exit_have.store(false, std::memory_order_relaxed);
            d.exit_written.store(false, std::memory_order_relaxed);
            {
                const auto board = d.parts[0].load(std::memory_order_relaxed);
                std::array<float, body_transform_floats> landed{};
                if (board && read(board + body_transform_offset, landed)) {
                    bool finite = true;
                    for (float value : landed) finite = finite && std::isfinite(value);
                    if (finite) {
                        for (std::size_t i = 0; i < landed.size(); ++i) d.exit_snap[i].store(landed[i], std::memory_order_relaxed);
                        d.exit_have.store(true, std::memory_order_relaxed);
                    }
                }
            }
            // 1.10 pop strength: floor the context's vertical velocity before 601's first step reads it.
            {
                const float floor = d.pop_strength.load(std::memory_order_relaxed);
                float vy{};
                bool written = false;
                if (dark_pop_read_float(owner.context + context_velocity_offset + 4, vy) && std::isfinite(vy)) {
                    d.strength_found.store(vy, std::memory_order_relaxed);
                    (void)floor; // 2.2: the entry write is retired (601 rewrote it and the launch is not read from here)
                } else {
                    d.strength_found.store(9.0f, std::memory_order_relaxed);
                }
                d.strength_written.store(written, std::memory_order_relaxed);
                d.strength_seen.store(9.0f, std::memory_order_relaxed);
            }
            d.route_live.store(true, std::memory_order_relaxed);
            d.route_live_ticks.store(0, std::memory_order_relaxed);
            d.adaptive_tick.store(0, std::memory_order_relaxed);
            d.freeze_have.store(false, std::memory_order_relaxed);
            d.freeze_writes.store(0, std::memory_order_relaxed);
            d.freeze_held.store(0, std::memory_order_relaxed);
            d.freeze_lost.store(0, std::memory_order_relaxed);
            d.route_clears.store(0, std::memory_order_relaxed);
            d.route_stay_clears.store(0, std::memory_order_relaxed);
            // Keep speed: the board body's velocity as the game decided the landing.
            std::array<float, 3> v{};
            const auto board = d.parts[0].load(std::memory_order_relaxed);
            if (board && read(board + body_velocity_offset, v) && std::isfinite(v[0]) && std::isfinite(v[2]) &&
                std::abs(v[0]) < 1000.0f && std::abs(v[2]) < 1000.0f) {
                d.landing_vx.store(v[0], std::memory_order_relaxed);
                d.landing_vz.store(v[2], std::memory_order_relaxed);
                d.landing_speed.store(std::sqrt(v[0] * v[0] + v[2] * v[2]), std::memory_order_relaxed);
            } else {
                d.landing_vx.store(0.0f, std::memory_order_relaxed);
                d.landing_vz.store(0.0f, std::memory_order_relaxed);
                d.landing_speed.store(0.0f, std::memory_order_relaxed);
            }
        }
        d.wanted.store(chosen, std::memory_order_relaxed);
        d.last_board_up.store(d.board_up.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_unpatch_ticks.store(d.unpatch_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_calls.store(d.place_calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_latched_before.store(d.place_latched_before.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_latched_after.store(d.place_latched_after.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_flag.store(d.place_flag.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_arg_flag.store(d.place_arg_flag.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_side.store(d.place_side.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_place_forced.store(d.place_forced.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_hold_starts.store(d.hold_starts.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_hold_resets.store(d.hold_resets.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_hold_ticks.store(d.hold_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_attach_requests.store(d.attach_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_attach_bone.store(d.attach_bone.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_hold_ground.store(d.hold_ground.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_hold_blend.store(d.hold_blend.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_feet_skips.store(d.feet_skips.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_feet_requests.store(d.feet_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
        if (pops) {
            d.pop_ticks.store(0, std::memory_order_relaxed);
            LARGE_INTEGER clock{};
            QueryPerformanceCounter(&clock);
            d.pop_start.store(clock.QuadPart, std::memory_order_relaxed);
            d.pop_until.store(now + dark_pop_pop_ms, std::memory_order_relaxed);
        } else {
            d.pop_until.store(0, std::memory_order_relaxed);
        }
        d.landings.fetch_add(1, std::memory_order_acq_rel);
        return chosen;
    }
    // The pop's own flight.
    if (leaves_air || pops) {
        d.pop_until.store(0, std::memory_order_relaxed);
        if (d.one_press.load(std::memory_order_relaxed)) d.need_release.store(true, std::memory_order_relaxed);
        LARGE_INTEGER clock{};
        QueryPerformanceCounter(&clock);
        d.pop_seconds.store(dark_pop_seconds(clock.QuadPart - d.pop_start.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        d.pop_landed.store(chosen, std::memory_order_relaxed);
        d.last_flight_v0.store(d.flight_v0.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_flight_total.store(d.flight_total.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_flight_found.store(d.flight_found.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_flight_floors.store(d.flight_floors.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_flight_stop.store(d.flight_stop.load(std::memory_order_relaxed) ? d.flight_stop.load(std::memory_order_relaxed) : 3u, std::memory_order_relaxed);
        d.flight_stop.store(3, std::memory_order_relaxed);
        d.last_hold_blocks.store(d.hold_blocks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_stay_clears.store(d.stay_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_early_jump.store(d.early_jump.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_writes.store(d.reaim_writes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_tick.store(d.reaim_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_frames.store(d.reaim_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_from.store(d.reaim_from.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_time.store(d.reaim_time.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_body_vy.store(d.reaim_body_vy.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_context_vy.store(d.reaim_context_vy.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_reaim_skip.store(d.reaim_skip.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (std::size_t i = 0; i < 3; ++i) {
            d.last_reaim_pos[i].store(d.reaim_pos[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            d.last_reaim_vel[i].store(d.reaim_vel[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        d.last_hold_contacts_max.store(d.hold_contacts_max.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_guard_clears.store(d.guard_clears.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_speed_reasserts.store(d.speed_reasserts.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.guard_left.store(0, std::memory_order_relaxed);
        float up{};
        d.pop_board_up.store(dark_pop_board_up(up) ? up : 9.0f, std::memory_order_relaxed);
        // The last hold counters now include the pop's first ticks.
        d.last_hold_ticks.store(d.hold_ticks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_attach_requests.store(d.attach_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_feet_skips.store(d.feet_skips.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.last_feet_requests.store(d.feet_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
        d.pop_landings.fetch_add(1, std::memory_order_acq_rel);
    }
    return chosen;
}
constexpr std::size_t fast_probe_capacity = 8192; // rows; about a minute of choices per drain interval at most
struct FastProbe {
    std::atomic<bool> enabled{};
    std::atomic<std::uint64_t> head{}, tail{};
    FastProbeRow rows[fast_probe_capacity];
};
FastProbe& fast_probe() { static auto* value = new FastProbe; return *value; }
void fast_probe_record(std::uintptr_t selector, std::uint32_t current, std::uint32_t wanted, std::uint32_t chosen) noexcept {
    auto& f = fast_probe();
    if (!f.enabled.load(std::memory_order_relaxed)) return;
    Owner owner;
    if (!local_owner(selector, &Owner::selector, &owner)) return;
    const auto head = f.head.load(std::memory_order_relaxed);
    if (head - f.tail.load(std::memory_order_acquire) >= fast_probe_capacity) return;
    auto& row = f.rows[head % fast_probe_capacity];
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    row.ticks = now.QuadPart;
    row.current = current;
    row.wanted = wanted;
    row.chosen = chosen;
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, false);
    row.buttons = pad.available ? pad.buttons : 0u;
    {
        auto& d = dark_pop();
        float up{};
        row.board_up = dark_pop_board_up(up) ? up : 9.0f; // 9 = not readable
        // The board body's three unit axes (floats 8-10, 12-14, 16-18; the up axis is the middle one).
        const auto board = d.board.load(std::memory_order_relaxed);
        for (int i = 0; i < 9; ++i) {
            float value{};
            row.axes[i] = board && read(board + 0x20 + 4 * (i + i / 3), value) && std::isfinite(value) ? value : 9.0f;
        }
        const auto tick = GetTickCount64();
        row.dark_pop = (d.pressed.load(std::memory_order_relaxed) ? 1u : 0u) | (d.armed.load(std::memory_order_relaxed) ? 2u : 0u) |
            (tick < d.pop_until.load(std::memory_order_relaxed) ? 4u : 0u);
        // 1.1: the footplant's plant point and up axis, the steering's two stick values, the placement's start position.
        for (auto& e : row.extra) e = 9.0f;
        for (int i = 0; i < 4; ++i) {
            float value{};
            if (dark_pop_read_float(owner.context + context_plant_offset + 4 * i, value)) row.extra[i] = value;
            if (dark_pop_read_float(owner.context + context_plant_up_offset + 4 * i, value)) row.extra[4 + i] = value;
        }
        for (int i = 0; i < 2; ++i) {
            float value{};
            if (dark_pop_read_float(owner.context + context_stick_offset + 4 * i, value)) row.extra[8 + i] = value;
        }
        const auto self = d.placement_self.load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) {
            float value{};
            if (self && dark_pop_read_float(self + placement_start_offset + 4 * i, value)) row.extra[10 + i] = value;
        }
    }
    {
        auto& d = dark_pop();
        for (int i = 0; i < 3; ++i) row.stage_up[i] = d.stage_up[i].exchange(9.0f, std::memory_order_relaxed);
        const auto board = d.parts[0].load(std::memory_order_relaxed);
        for (int i = 0; i < 6; ++i) {
            float value{};
            row.board_vel[i] = board && read(board + body_velocity_offset + 4 * (i + i / 3), value) && std::isfinite(value) ? value : 9.0f;
        }
    }
    {
        float value{};
        row.gates[0] = dark_pop_read_float(owner.context + context_gate_a_offset, value) ? value : 9.0f;
        row.gates[1] = dark_pop_read_float(owner.context + context_gate_b_offset, value) ? value : 9.0f;
        unsigned word{};
        row.word_13f8 = read(owner.context + context_word_13f8_offset, word) ? word : 0u;
        // 2.3: the contact count (as found before any hold this call) and the physics block's ground/wheel bytes.
        auto& d = dark_pop();
        std::uint32_t contacts{};
        row.contacts = d.hold_restore.load(std::memory_order_relaxed) && d.hold_context.load(std::memory_order_relaxed) == owner.context && (d.hold_mask.load(std::memory_order_relaxed) & context_contact_bits)
            ? d.hold_saved_contacts.load(std::memory_order_relaxed) : (read(owner.context + context_contacts_offset, contacts) ? contacts : 0xffffffffu);
        std::uintptr_t iface{}, physics{};
        std::uint8_t ground{};
        std::array<std::uint8_t, 4> wheels{};
        row.phys_flags = read(owner.core + core_interface_offset, iface) && iface && read(iface + interface_physics_offset, physics) && physics &&
                read(physics + physics_ground_byte_offset, ground) && read(physics + physics_wheel_bytes_offset, wheels)
            ? (static_cast<std::uint32_t>(ground != 0) | (static_cast<std::uint32_t>(wheels[0] != 0) << 1) | (static_cast<std::uint32_t>(wheels[1] != 0) << 2) |
               (static_cast<std::uint32_t>(wheels[2] != 0) << 3) | (static_cast<std::uint32_t>(wheels[3] != 0) << 4))
            : 0xffu;
    }
    for (int i = 0; i < 8; ++i) { unsigned word{}; row.words[i] = read(owner.context + 0x13c4 + 4 * i, word) ? word : 0u; }
    for (int i = 0; i < 10; ++i) { unsigned word{}; row.words[8 + i] = read(owner.context + 0x140c + 4 * i, word) ? word : 0u; }
    f.head.store(head + 1, std::memory_order_release);
}
std::uint32_t choose_state(std::uintptr_t selector, std::uint32_t current) {
    {
        LastError error;
        hold_off_board(selector, current);
        dark_pop_before(selector, current);
    }
    // A wipeout asked for (no_bail_request_wipeout): its cause goes in as the game's contact tests
    // record theirs, and the pick is Wipeout, as some of those tests return it directly.
    bool force_wipeout{};
    if (auto& wanted = wanted_wipeout(); wanted.until.load(std::memory_order_acquire) &&
        selector == state_watch().selector.load(std::memory_order_acquire)) {
        LastError error;
        if (const auto until = wanted.until.exchange(0, std::memory_order_acq_rel); GetTickCount64() < until)
            if (const auto causes = state_watch().causes.load(std::memory_order_acquire); causes && !protected_owner(selector, &Owner::selector)) {
                record_cause(causes, wanted_wipeout_reason, wanted.magnitude.load(std::memory_order_relaxed));
                force_wipeout = true;
            }
    }
    // Clear shared requests before selection so native ground/air/walking
    // transitions can still run. Some contact tests return Wipeout directly;
    // retain the current state only for that result, never ordinary Offboard.
    const bool filtered = filter_requests(selector, &Owner::selector);
    auto next = protection().choose_original(selector, current);
    LastError error;
    if (force_wipeout && next != wipeout_physics_state) {
        logging::log(logging::Level::debug, logging::Channel::skater, "No Bail: a wipeout was asked for (state {} would have been {}).", current, next);
        next = wipeout_physics_state;
    }
    auto chosen = filtered && next == wipeout_physics_state && protected_owner(selector, &Owner::selector) ? current : next;
    chosen = dark_pop_after(selector, current, chosen);
    fast_probe_record(selector, current, next, chosen);
    auto& w = state_watch();
    if (selector == w.selector.load(std::memory_order_acquire) && GetTickCount64() < w.until.load(std::memory_order_acquire)) {
        if (const auto before = w.state.exchange(chosen, std::memory_order_acq_rel); before != chosen) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            w.previous.store(before, std::memory_order_relaxed);
            w.previous_ticks.store(now.QuadPart - w.since.exchange(now.QuadPart, std::memory_order_relaxed), std::memory_order_relaxed);
            w.changes.fetch_add(1, std::memory_order_relaxed);
            if (chosen == wipeout_physics_state) {
                // Before the count: the tick that sees the new wipeout finds its cause there.
                skater_events::note_wipeout(w.causes.load(std::memory_order_acquire));
                w.wipeouts.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    track_spin(selector, chosen);
    return chosen;
}
void skeleton_response(std::uintptr_t rig, float seconds, bool wipeout) {
    // The state post-update can raise another request after the selector ran.
    // Filter at this consumer, then let native constraints and recovery run.
    if (filter_requests(rig, &Owner::rig)) wipeout = false;
    const auto observer = protection().step_observer.load(std::memory_order_acquire);
    std::array<NoBailStepObserver, no_bail_step_listener_slots> listeners{};
    bool any = observer != nullptr;
    for (std::size_t i = 0; i < listeners.size(); ++i) {
        listeners[i] = protection().listeners[i].load(std::memory_order_acquire);
        any = any || listeners[i];
    }
    if (any) {
        LastError error;
        NoBailSkater skater;
        if (published_skater(rig, skater)) {
            if (observer) observer(skater, seconds, wipeout);
            for (const auto listener : listeners)
                if (listener) listener(skater, seconds, wipeout);
        }
    }
    protection().skeleton_original(rig, seconds, wipeout);
}
bool clear_contact_output(std::uintptr_t contacts) noexcept {
    if (contacts < 0x10000 || contacts > highest - body_contact_output_offset) return false;
    __try {
        _InterlockedExchange8(reinterpret_cast<volatile char*>(contacts + body_contact_output_offset), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void filter_contact_animation(std::uintptr_t core) noexcept {
    LastError error;
    Owner owner;
    std::uint32_t state{};
    if (!protected_owner(core, &Owner::core, &owner) || !read(owner.context + 0x1414, state)) return;
    // Ground/air and grind state families. Offboard, mounting, handplants and
    // existing ragdolls retain their native contact reporting and recovery.
    if (!((state >= 100 && state < 300) || (state >= 400 && state < 500))) return;
    const auto contacts = pointer(pointer(core, 0x3b8), 0x30);
    std::uint8_t contact{};
    if (!contacts || !read(contacts + body_contact_output_offset, contact) || contact > 1) return;
    // Animation gets this sensitive-body hit independently of the ordinary
    // wipeout output and cause collector. Filter it after native publication,
    // before the animation contact context copies it. Keep the general contact
    // latch, collision timers, per-bone records, impulses and physics state.
    if (clear_contact_output(contacts))
        (void)cancel_request(owner.context, body_contact_context_offset, body_contact_context_mask);
}
void publish_animation(std::uintptr_t core) {
    // Animation also reads requests without consulting the cause collector.
    // Clear flags and pending causes before native publication, including
    // its early recovery query and cause export. Leave the native query result
    // unchanged: forcing it true used to put the skater into a stumbling state.
    (void)filter_requests(core, &Owner::core);
    protection().publish_original(core);
    filter_contact_animation(core);
    // Sample the game's own failed-landing/runout result at its publication
    // boundary. It may be true for only one physics frame, between mode ticks.
    // Counting its rising edge retains that event without forcing a native bail
    // or treating an ordinary voluntary dismount as a failed landing.
    LastError error;
    auto& w=state_watch();
    if(core==w.core.load(std::memory_order_acquire) && GetTickCount64()<w.until.load(std::memory_order_acquire) &&
       pointer(core,0x440)==w.selector.load(std::memory_order_acquire)) {
        const auto animation=pointer(pointer(core,addr::skater_state::trick_state_offset),recovery_animation_output);
        std::uint8_t runout{};
        if(animation && read(animation+recovery_animation_flag,runout) && runout<=1 &&
           w.runout.exchange(runout!=0,std::memory_order_acq_rel)!=(runout!=0) && runout)
            w.runouts.fetch_add(1,std::memory_order_relaxed);
    }
}
bool compatible(std::uintptr_t base) noexcept {
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    std::array<unsigned char, reset_bail_causes_code.size()> reset_code{};
    if (!read(base + reset_bail_causes_rva, reset_code) || reset_code != reset_bail_causes_code) return false;
    for (const auto& contract : {record_bail_cause_contract, choose_physics_state_contract,
            bail_animation_caller_contract, bail_state_caller_contract, bail_skeleton_contract, bail_publish_contract}) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : bail_consumer_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& contract : body_contact_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    for (const auto& impact : impact_bail_calls) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + impact.caller.rva, actual) || actual != impact.caller.bytes) return false;
    }
    return true;
}
}
bool start_no_bail(std::uintptr_t base) noexcept {
    LastError error;
    try {
        auto& p = protection();
        if (p.ready.load()) return p.base == base;
        if (!compatible(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "No Bail is unavailable: native bail contract did not match.");
            return false;
        }
        p.base = base;
        p.reset_causes = reinterpret_cast<ResetCauses>(base + reset_bail_causes_rva);
        std::array<unsigned char, 32> mount_test{};
        p.mount_test = read(base + mount_request_test_contract.rva, mount_test) &&
                       mount_test == mount_request_test_contract.bytes;
        const std::array targets{
            reinterpret_cast<void*>(base + record_bail_cause_contract.rva),
            reinterpret_cast<void*>(base + choose_physics_state_contract.rva),
            reinterpret_cast<void*>(base + bail_skeleton_contract.rva),
            reinterpret_cast<void*>(base + bail_publish_contract.rva)};
        const std::array replacements{
            reinterpret_cast<void*>(&record_cause), reinterpret_cast<void*>(&choose_state),
            reinterpret_cast<void*>(&skeleton_response), reinterpret_cast<void*>(&publish_animation)};
        std::array<void*, 4> originals{};
        auto status = HookOk;
        std::size_t prepared{};
        for (; prepared < targets.size(); ++prepared) {
            status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
            if (status != HookOk) break;
            if (!originals[prepared]) { ++prepared; status = HookUnsupportedFunction; break; }
        }
        if (status == HookOk) {
            // Publish every relay before enabling any target.
            p.cause_original = reinterpret_cast<RecordCause>(originals[0]);
            p.choose_original = reinterpret_cast<ChooseState>(originals[1]);
            p.skeleton_original = reinterpret_cast<SkeletonResponse>(originals[2]);
            p.publish_original = reinterpret_cast<PublishAnimation>(originals[3]);
            for (auto target : targets) {
                status = hook_enable(target);
                if (status != HookOk) break;
            }
            if (status == HookOk) {
                p.ready.store(true, std::memory_order_release);
                logging::write(logging::Level::info, logging::Channel::skater,
                    "No Bail ready: collision and landing causes filtered before physics/animation consume them; noclip is protected.");
                (void)start_dark_pop_unpatch(base);
                (void)start_dark_pop_hold(base);
                (void)start_dark_pop_feet(base);
                (void)start_dark_pop_mover(base);
                return true;
            }
        }
        // Published relays remain callable even if Detours reports an uncertain
        // attach result. With ready=false any remaining hook simply forwards.
        logging::log(logging::Level::warning, logging::Channel::skater,
            "No Bail hook setup failed (status {}); protected noclip is unavailable.", static_cast<LONG>(status));
        while (prepared) (void)hook_remove(targets[--prepared]);
    } catch (...) {}
    return false;
}
bool no_bail_available() noexcept { return protection().ready.load(std::memory_order_acquire); }
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept {
    LastError error;
    auto& p = protection();
    Lease next;
    const bool available = p.ready.load(std::memory_order_acquire) && resolve(client, entity, next.owner);
    if (available) {
        const auto now = GetTickCount64();
        next.manual_until = manual ? now + 500 : 0;
        next.flight_until = flying ? flight_expires : 0;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.lease = available ? next : Lease{};
    ReleaseSRWLockExclusive(&p.lock);
    return available;
}
bool set_teleport_on_board(std::uintptr_t component) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(component + 0xc0) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool prepare_on_board_teleport(std::uintptr_t client, std::uintptr_t entity) noexcept {
    LastError error;
    Owner owner;
    if(!protection().ready.load(std::memory_order_acquire) || !resolve(client,entity,owner))return false;
    std::uint8_t board{};
    if(!read(owner.component+0xc0,board))return false;
    if(board==1)return true;
    if(!set_teleport_on_board(owner.component))return false;
    logging::write(logging::Level::info,logging::Channel::skater,"Match countdown: native on-board spawn option prepared.");
    return true;
}
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept {
    LastError error;
    auto& p = protection();
    if (!p.ready.load(std::memory_order_acquire)) return;
    static bool rearm{}; // client tick only
    if (locked) {
        // The owner does not resolve while a teleport is under way: the last one stands
        // until it expires.
        BoardLock next;
        if (resolve(client, entity, next.owner)) {
            next.until = GetTickCount64() + 500;
            AcquireSRWLockExclusive(&p.lock);
            p.board = next;
            ReleaseSRWLockExclusive(&p.lock);
        }
        rearm = true;
        return;
    }
    AcquireSRWLockExclusive(&p.lock);
    p.board = {};
    ReleaseSRWLockExclusive(&p.lock);
    // Released: teleports that keep the skater's own choice (the SDK's /tp) put it on the
    // board again. Once the skater resolves, outside a teleport.
    Owner owner;
    if (rearm && resolve(client, entity, owner) && set_teleport_on_board(owner.component)) rearm = false;
}
void clear_no_bail() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease = {};
    ReleaseSRWLockExclusive(&p.lock);
}
bool no_bail_skater(NoBailSkater& skater) noexcept { return published_skater(0, skater); }
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept {
    protection().step_observer.store(observer, std::memory_order_release);
}
void dark_pop_update(bool enabled, std::uintptr_t board, bool unpatch, bool hold, bool one_press,
    unsigned route_stay, unsigned route_guard, bool keep_speed, bool catch_dismount, bool feet, bool adaptive, bool catch_timeout, float pop_strength, bool hold_landing, bool dark_exit, bool full_flight, bool hold_flight, unsigned stay_physical, bool early_physics, bool reaim, bool hide_dpad) noexcept {
    auto& d = dark_pop();
    d.hide_dpad.store(hide_dpad, std::memory_order_relaxed);
    d.reaim.store(reaim, std::memory_order_relaxed);
    d.early_physics.store(early_physics, std::memory_order_relaxed);
    d.stay_physical.store(stay_physical, std::memory_order_relaxed);
    d.hold_flight.store(hold_flight, std::memory_order_relaxed);
    d.hold_landing.store(hold_landing, std::memory_order_relaxed);
    d.dark_exit.store(dark_exit, std::memory_order_relaxed);
    d.full_flight.store(full_flight, std::memory_order_relaxed);
    d.catch_timeout.store(catch_timeout, std::memory_order_relaxed);
    d.pop_strength.store(pop_strength, std::memory_order_relaxed);
    d.adaptive.store(adaptive, std::memory_order_relaxed);
    d.feet.store(feet, std::memory_order_relaxed);
    d.catch_dismount.store(catch_dismount, std::memory_order_relaxed);
    d.keep_speed.store(keep_speed, std::memory_order_relaxed);
    d.route_stay.store(route_stay, std::memory_order_relaxed);
    d.route_guard.store(route_guard, std::memory_order_relaxed);
    d.one_press.store(one_press, std::memory_order_relaxed);
    d.unpatch.store(unpatch, std::memory_order_relaxed);
    d.hold.store(hold, std::memory_order_relaxed);
    d.board.store(enabled ? board : 0, std::memory_order_relaxed);
    d.enabled.store(enabled, std::memory_order_relaxed);
}
void dark_pop_publish_bodies(const std::uintptr_t* parts, std::size_t count) noexcept {
    auto& d = dark_pop();
    const auto n = std::min(count, body_parts);
    for (std::size_t i = 0; i < body_parts; ++i) d.parts[i].store(i < n && parts ? parts[i] : 0, std::memory_order_relaxed);
    d.part_count.store(static_cast<unsigned>(n), std::memory_order_relaxed);
}
DarkPopLast dark_pop_last() noexcept {
    auto& d = dark_pop();
    DarkPopLast last;
    last.landings = d.landings.load(std::memory_order_acquire);
    last.wanted = d.wanted.load(std::memory_order_relaxed);
    last.board_up = d.last_board_up.load(std::memory_order_relaxed);
    last.pop_landings = d.pop_landings.load(std::memory_order_acquire);
    last.pop_landed = d.pop_landed.load(std::memory_order_relaxed);
    last.pop_seconds = d.pop_seconds.load(std::memory_order_relaxed);
    last.unpatch = d.unpatch.load(std::memory_order_relaxed);
    last.unpatch_hooked = d.hooked.load(std::memory_order_relaxed);
    last.unpatch_ticks = d.last_unpatch_ticks.load(std::memory_order_relaxed);
    last.place_calls = d.last_place_calls.load(std::memory_order_relaxed);
    last.place_latched_before = d.last_place_latched_before.load(std::memory_order_relaxed);
    last.place_latched_after = d.last_place_latched_after.load(std::memory_order_relaxed);
    last.place_flag = d.last_place_flag.load(std::memory_order_relaxed);
    last.place_arg_flag = d.last_place_arg_flag.load(std::memory_order_relaxed);
    last.place_side = d.last_place_side.load(std::memory_order_relaxed);
    last.place_forced = d.last_place_forced.load(std::memory_order_relaxed);
    last.hold = d.hold.load(std::memory_order_relaxed);
    last.hold_hooked = d.hold_hooked.load(std::memory_order_relaxed);
    last.hold_starts = d.last_hold_starts.load(std::memory_order_relaxed);
    last.hold_resets = d.last_hold_resets.load(std::memory_order_relaxed);
    last.hold_ticks = d.last_hold_ticks.load(std::memory_order_relaxed);
    last.attach_requests = d.last_attach_requests.load(std::memory_order_relaxed);
    last.attach_bone = d.last_attach_bone.load(std::memory_order_relaxed);
    last.hold_ground = d.last_hold_ground.load(std::memory_order_relaxed);
    last.hold_blend = d.last_hold_blend.load(std::memory_order_relaxed);
    last.releases = d.releases.load(std::memory_order_relaxed);
    last.one_press = d.one_press.load(std::memory_order_relaxed);
    last.route = d.route.load(std::memory_order_relaxed);
    last.route_ticks = d.last_route_ticks.load(std::memory_order_relaxed);
    last.route_live_ticks = d.last_route_live_ticks.load(std::memory_order_relaxed);
    last.route_clears = d.last_route_clears.load(std::memory_order_relaxed);
    last.route_stay = d.route_stay.load(std::memory_order_relaxed);
    last.route_stay_clears = d.last_route_stay_clears.load(std::memory_order_relaxed);
    last.route_let_go = d.route_let_go.load(std::memory_order_relaxed);
    last.route_guard = d.route_guard.load(std::memory_order_relaxed);
    last.keep_speed = d.keep_speed.load(std::memory_order_relaxed);
    last.landing_speed = d.last_landing_speed.load(std::memory_order_relaxed);
    last.added_speed = d.last_added_speed.load(std::memory_order_relaxed);
    last.speed_bodies = d.last_speed_bodies.load(std::memory_order_relaxed);
    last.speed_reasserts = d.last_speed_reasserts.load(std::memory_order_relaxed);
    last.feet = d.feet.load(std::memory_order_relaxed);
    last.feet_ready = d.feet_hooked.load(std::memory_order_relaxed);
    last.feet_skips = d.last_feet_skips.load(std::memory_order_relaxed);
    last.feet_requests = d.last_feet_requests.load(std::memory_order_relaxed);
    last.mover_ready = d.mover_hooked.load(std::memory_order_relaxed);
    last.mover_captures = d.last_mover_captures.load(std::memory_order_relaxed);
    last.mover_held = d.last_mover_held.load(std::memory_order_relaxed);
    last.adaptive = d.adaptive.load(std::memory_order_relaxed);
    last.adaptive_tick = d.last_adaptive_tick.load(std::memory_order_relaxed);
    last.freeze_writes = d.last_freeze_writes.load(std::memory_order_relaxed);
    last.freeze_held = d.last_freeze_held.load(std::memory_order_relaxed);
    last.freeze_lost = d.last_freeze_lost.load(std::memory_order_relaxed);
    last.catch_dismount = d.catch_dismount.load(std::memory_order_relaxed);
    last.caught = d.last_caught.load(std::memory_order_relaxed);
    last.catches = d.catches.load(std::memory_order_relaxed);
    last.catch_timeout = d.catch_timeout.load(std::memory_order_relaxed);
    last.timed_out = d.last_timed_out.load(std::memory_order_relaxed);
    last.timeout_catches = d.timeout_catches.load(std::memory_order_relaxed);
    last.pop_strength = d.pop_strength.load(std::memory_order_relaxed);
    last.hold_landing = d.hold_landing.load(std::memory_order_relaxed);
    last.hold_landing_ticks = d.last_hold_landing_ticks.load(std::memory_order_relaxed);
    last.hold_landing_staged = d.hold_landing_staged.load(std::memory_order_relaxed);
    last.dark_exit = d.dark_exit.load(std::memory_order_relaxed);
    last.exit_written = d.last_exit_written.load(std::memory_order_relaxed);
    last.exit_up_before = d.last_exit_up_before.load(std::memory_order_relaxed);
    last.exit_up_after = d.last_exit_up_after.load(std::memory_order_relaxed);
    last.full_flight = d.full_flight.load(std::memory_order_relaxed);
    last.hold_flight = d.hold_flight.load(std::memory_order_relaxed);
    last.hold_blocks = d.last_hold_blocks.load(std::memory_order_relaxed);
    last.hold_contacts_max = d.last_hold_contacts_max.load(std::memory_order_relaxed);
    last.stay_physical = d.stay_physical.load(std::memory_order_relaxed);
    last.stay_clears = d.last_stay_clears.load(std::memory_order_relaxed);
    last.early_physics = d.early_physics.load(std::memory_order_relaxed);
    last.early_jump = d.last_early_jump.load(std::memory_order_relaxed);
    last.reaim = d.reaim.load(std::memory_order_relaxed);
    last.reaim_writes = d.last_reaim_writes.load(std::memory_order_relaxed);
    last.reaim_tick = d.last_reaim_tick.load(std::memory_order_relaxed);
    last.reaim_frames = d.last_reaim_frames.load(std::memory_order_relaxed);
    last.reaim_from = d.last_reaim_from.load(std::memory_order_relaxed);
    last.reaim_time = d.last_reaim_time.load(std::memory_order_relaxed);
    last.reaim_body_vy = d.last_reaim_body_vy.load(std::memory_order_relaxed);
    last.reaim_context_vy = d.last_reaim_context_vy.load(std::memory_order_relaxed);
    last.reaim_skip = d.last_reaim_skip.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) {
        last.reaim_pos[i] = d.last_reaim_pos[i].load(std::memory_order_relaxed);
        last.reaim_vel[i] = d.last_reaim_vel[i].load(std::memory_order_relaxed);
    }
    last.flight_v0 = d.last_flight_v0.load(std::memory_order_relaxed);
    last.flight_total = d.last_flight_total.load(std::memory_order_relaxed);
    last.flight_found = d.last_flight_found.load(std::memory_order_relaxed);
    last.flight_floors = d.last_flight_floors.load(std::memory_order_relaxed);
    last.flight_stop = d.last_flight_stop.load(std::memory_order_relaxed);
    last.strength_found = d.last_strength_found.load(std::memory_order_relaxed);
    last.strength_written = d.last_strength_written.load(std::memory_order_relaxed);
    last.strength_seen = d.last_strength_seen.load(std::memory_order_relaxed);
    last.strength_writes = d.strength_writes.load(std::memory_order_relaxed);
    last.jump_ticks = d.jump_ticks.load(std::memory_order_relaxed);
    last.jump_vy = d.jump_vy.load(std::memory_order_relaxed);
    last.launch_state_ok = d.launch_state_ok.load(std::memory_order_relaxed);
    last.launch_flag = d.launch_flag.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) last.launch_found[i] = d.launch_found[i].load(std::memory_order_relaxed);
    last.launch_written = d.launch_written.load(std::memory_order_relaxed);
    last.launch_writes = d.launch_writes.load(std::memory_order_relaxed);
    last.lift_found = d.lift_found.load(std::memory_order_relaxed);
    last.lift_added[0] = d.lift_added[0].load(std::memory_order_relaxed);
    last.lift_added[1] = d.lift_added[1].load(std::memory_order_relaxed);
    last.lift_bodies = d.lift_bodies.load(std::memory_order_relaxed);
    last.lift_ticks = d.lift_ticks.load(std::memory_order_relaxed);
    last.lifts = d.lifts.load(std::memory_order_relaxed);
    last.guard_clears = d.last_guard_clears.load(std::memory_order_relaxed);
    last.route_exit = d.route_exit.load(std::memory_order_relaxed);
    last.route_exits = d.route_exits.load(std::memory_order_acquire);
    last.pop_board_up = d.pop_board_up.load(std::memory_order_relaxed);
    return last;
}
void fast_probe_enable(bool on) noexcept {
    auto& f = fast_probe();
    if (on) f.tail.store(f.head.load(std::memory_order_acquire), std::memory_order_release); // start empty
    f.enabled.store(on, std::memory_order_release);
}
std::size_t fast_probe_drain(FastProbeRow* rows, std::size_t capacity) noexcept {
    auto& f = fast_probe();
    auto tail = f.tail.load(std::memory_order_relaxed);
    const auto head = f.head.load(std::memory_order_acquire);
    std::size_t count = 0;
    while (tail != head && count < capacity) rows[count++] = f.rows[tail++ % fast_probe_capacity];
    f.tail.store(tail, std::memory_order_release);
    return count;
}
void no_bail_request_wipeout(float magnitude) noexcept {
    auto& wanted = wanted_wipeout();
    wanted.magnitude.store(magnitude, std::memory_order_relaxed);
    wanted.until.store(GetTickCount64() + 250, std::memory_order_release);
}
bool add_no_bail_step_listener(NoBailStepObserver listener) noexcept {
    if (!listener) return false;
    auto& slots = protection().listeners;
    for (auto& slot : slots)
        if (slot.load(std::memory_order_acquire) == listener) return true;
    for (auto& slot : slots) {
        NoBailStepObserver empty{};
        if (slot.compare_exchange_strong(empty, listener, std::memory_order_acq_rel)) return true;
    }
    return false;
}
void remove_no_bail_step_listener(NoBailStepObserver listener) noexcept {
    if (!listener) return;
    for (auto& slot : protection().listeners) {
        auto expected = listener;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept {
    auto& w = state_watch();
    Owner owner;
    if (!protection().ready.load(std::memory_order_acquire) || !resolve(client, entity, owner)) return;
    if (w.selector.exchange(owner.selector, std::memory_order_acq_rel) != owner.selector) {
        w.state.store(0, std::memory_order_release);
        w.runout.store(false,std::memory_order_release);
    }
    w.causes.store(owner.causes, std::memory_order_release);
    w.core.store(owner.core, std::memory_order_release);
    w.until.store(GetTickCount64() + 500, std::memory_order_release);
}
PhysicsStateWatch watched_physics_state() noexcept {
    auto& w = state_watch();
    PhysicsStateWatch result;
    result.valid = protection().ready.load(std::memory_order_acquire) && w.selector.load(std::memory_order_acquire) &&
        GetTickCount64() < w.until.load(std::memory_order_acquire);
    result.state = w.state.load(std::memory_order_acquire);
    result.previous = w.previous.load(std::memory_order_relaxed);
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    result.previous_seconds = static_cast<float>(static_cast<double>(w.previous_ticks.load(std::memory_order_relaxed)) / frequency);
    result.changes = w.changes.load(std::memory_order_relaxed);
    result.wipeouts = w.wipeouts.load(std::memory_order_relaxed);
    result.runouts = w.runouts.load(std::memory_order_relaxed);
    if(result.valid) {
        // Failed-trick animated/simulated ragdolls commonly stay in 504, just
        // like ordinary walking. The selector's 300 counter alone misses them.
        // Revalidate the published core/selector pair before reading the native
        // offboard flag; retain unknown on a transient unreadable owner.
        const auto core=w.core.load(std::memory_order_acquire);
        if(pointer(core,0x440)==w.selector.load(std::memory_order_acquire)) {
            if(result.state!=offboard_physics_state)result.ragdoll_known=true;
            else {
                namespace live=addr::skater_state;
                const auto trick=pointer(core,live::trick_state_offset);
                const auto offboard=pointer(trick,live::offboard_state_offset);
                std::uint8_t flag{};
                if(offboard && read(offboard+live::ragdoll_offset,flag) && flag<=1) {
                    result.ragdoll_known=true;result.ragdoll=flag!=0;
                }
            }
        }
    }
    return result;
}
void clear_no_bail_flight() noexcept {
    auto& p = protection();
    AcquireSRWLockExclusive(&p.lock);
    p.lease.flight_until = 0;
    ReleaseSRWLockExclusive(&p.lock);
}
void watch_revert_spin(std::uintptr_t selector, std::uintptr_t transform, std::uintptr_t board) noexcept {
    auto& s = spin_watch();
    s.transform.store(transform, std::memory_order_release);
    s.board.store(board, std::memory_order_release);
    s.selector.store(selector, std::memory_order_release);
    s.until.store(GetTickCount64() + 500, std::memory_order_release);
}
RevertLanding last_revert_landing() noexcept {
    auto& s = spin_watch();
    AcquireSRWLockShared(&s.lock);
    const auto landing = s.last;
    ReleaseSRWLockShared(&s.lock);
    return landing;
}
}
