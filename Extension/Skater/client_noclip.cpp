#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "offboard_flight.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "free_flight.h"
#include <cmath>

namespace dingosdk::client_source::detail {
namespace {
// Physics bodies already verified writable (a VirtualQuery each), so the
// every-frame and every-simulation-step body reads below skip that system call.
// A body at a new address, and every body once a second, is checked again.
// Only touched with SourceState::busy held, which every caller holds.
struct WritableBodies {
    std::array<std::uintptr_t, 32> bodies{};
    ULONGLONG until{};
};
WritableBodies& writable_bodies() { static WritableBodies value; return value; }
bool body_writable(std::size_t index, std::uintptr_t body) {
    auto& cache = writable_bodies();
    const auto now = GetTickCount64();
    if (now >= cache.until) {
        cache.bodies = {};
        cache.until = now + 1000;
    }
    if (cache.bodies[index] == body) return true;
    if (!source_writable(body + 0x60, 0x20)) return false;
    cache.bodies[index] = body;
    return true;
}
bool copy_to(std::uintptr_t address, const void* data, std::size_t size) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// debug_write for a physics body field inside the region debug_noclip_bodies
// verified writable: a guarded copy and read-back instead of VirtualQuery,
// WriteProcessMemory and a ReadProcessMemory on every simulation step.
template<class T> void body_write(std::uintptr_t address, const T& value) {
    source_require(source_range(address, sizeof(T)) && copy_to(address, &value, sizeof(T)), "Debug setting write failed.");
    T written{};
    source_require(memory::peek(address, written) && written == value, "Debug setting write could not be verified.");
}
}
void debug_stop_noclip(InteractiveDebug& debug) noexcept {
    debug.noclip = false;
    debug.noclip_entity = 0;
    debug.noclip_velocity.valid = false;
    clear_no_bail_flight();
}
NoclipBodies debug_noclip_bodies(std::uintptr_t base, std::uintptr_t client, std::uintptr_t entity) {
    overlay::DebugModel skater;
    (void)debug_skater(base, client, skater);
    source_require(skater.skater_identity == entity, "Skater changed; flight stopped.");
    SourceReader reader;
    const auto collection = reader.pointer(entity, 0x70);
    const auto component = reader.pointer(entity, 0x628);
    source_require(reader.pointer(component) == base + addr::engine::skater_component_vtable && reader.pointer(component, 0x18) == collection,
        "Skater physics ownership changed.");
    NoclipBodies result;
    result.root = skater.skater_position;
    result.core = reader.pointer(component, 0x70);
    source_require(reader.pointer(result.core) == base + addr::no_bail::bail_core_vtable, "Skater physics is unavailable.");
    const auto board = reader.pointer(reader.pointer(result.core, 0x430), 0x18);
    result.rig_wrapper = reader.pointer(result.core, 0x438);
    const auto rig = reader.pointer(result.rig_wrapper, 0x2f10);
    result.context = reader.pointer(result.core, 0x3c0);
    source_require(reader.pointer(result.rig_wrapper) == result.context && reader.pointer(result.rig_wrapper, 0x4630) == result.core,
        "Skater motion ownership changed.");
    result.seconds = reader.value<float>(result.context, 0x17ec);
    source_require(std::isfinite(result.seconds) && result.seconds >= 0 && result.seconds <= .1f, "Invalid physics timestep.");
    result.offboard = reader.pointer(reader.pointer(result.core, 0x3b0)) == base + addr::offboard_flight::offboard_flight_vtable;
    // Read the committed native state, not the selector's proposed next state
    // (which is remapped by its caller and may be stale between selections).
    const auto physics = reader.value<std::uint32_t>(result.context, 0x1414);
    result.wipeout = physics == addr::no_bail::wipeout_physics_state ||
        physics == addr::no_bail::offboard_physics_state;
    source_require(reader.pointer(board) == base + spawn::board_physics_vtable && reader.pointer(rig) == base + spawn::rig_physics_vtable,
        "Unsupported board or skeleton physics.");
    const auto board_parts = reader.pointer(board, 0x20), rig_parts = reader.pointer(rig, 0x20);
    source_require(reader.value<std::uint32_t>(board_parts, 0) == 9 && reader.value<std::uint32_t>(rig_parts, 0) == 26,
        "Unsupported physics body layout.");
    for (std::size_t i = 0; i < result.parts.size(); ++i) {
        const auto body = i < 9 ? board_parts + i * 0x130 : rig_parts + (i - 8) * 0x130;
        source_require(reader.pointer(body, 0x10) == (i < 9 ? board : rig) && body_writable(i, body),
            "Physics body ownership changed.");
        const auto velocity = reader.value<std::array<float, 3>>(body, 0x70);
        for (const auto v : velocity) source_require(std::isfinite(v) && std::abs(v) <= 100000,
            "Invalid physics velocity.");
        result.parts[i] = body;
    }
    result.board_height = reader.value<float>(board_parts, 0x54);
    source_require(std::isfinite(result.board_height) && std::abs(result.board_height) <= 1000000, "Invalid board altitude.");
    reader.verify();
    return result;
}
namespace {
void noclip_apply_velocity(std::uintptr_t core) noexcept {
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
        state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    auto& debug = state.trial.debug;
    const auto apply_boost = [&](InteractiveDebug::VelocityRequest& boost, std::uint64_t& updates, bool up, bool offboard = false) {
        if (!boost.valid || boost.core != core) return;
        try {
            if (offboard && boost.applied && GetTickCount64() >= boost.expires) {
                boost.valid = false;
                state.offboard_boost_core.store(0, std::memory_order_release);
                return;
            }
            source_require(GetTickCount64() < boost.expires, "Velocity boost timed out.");
            const auto bodies = debug_noclip_bodies(state.trial.base, boost.client, boost.entity);
            source_require(bodies.core == core, "Skater physics was replaced before the velocity boost could apply.");
            source_require(offboard ? (boost.applied || bodies.offboard || bodies.wipeout) : !bodies.offboard,
                offboard ? "Off-board Up Boost requires walking, falling or gliding." :
                "Velocity boosts require the skater to be on the board.");
            source_require(!debug.noclip && !debug.park_editor, "Velocity boost cancelled while editing or flying.");
            SourceReader reader;
            source_require(reader.value<std::uint8_t>(boost.entity, 0x7e0) == 0,
                "Wait for the current teleport before using velocity boosts.");
            std::array<std::array<float, 3>, 32> velocities{};
            std::array<std::uint32_t, 32> flags{};
            const std::size_t first = offboard ? 9 : 0; // Leave the carried/detached board alone.
            if (offboard && !boost.applied) {
                const auto current_up = reader.value<float>(bodies.parts[9], 0x74);
                boost.velocity[1] = std::max(boost.velocity[1], current_up + boost.velocity[1]);
            }
            for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
                flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (offboard && axis == 1) velocities[i][axis] = std::max(velocities[i][axis], boost.velocity[1]);
                    else velocities[i][axis] += boost.velocity[axis];
                    source_require(std::isfinite(velocities[i][axis]) && std::abs(velocities[i][axis]) <= 100000,
                        "Velocity boost produced an invalid physics velocity.");
                }
            }
            reader.verify();
            if (offboard && bodies.offboard)
                source_require(ensure_offboard_up_velocity(state.trial.base, core, bodies.context,
                    bodies.rig_wrapper, boost.velocity[1]), "Off-board boost motion ownership changed.");
            for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                body_write(bodies.parts[i] + 0x70, velocities[i]);
                body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
            if (offboard) {
                if (!boost.applied) {
                    ++updates;
                    boost.applied = true;
                    boost.expires = GetTickCount64() + 180;
                    debug.status = "Off-board up velocity added.";
                }
            } else {
                boost.valid = false;
                ++updates;
                debug.status = up ? "Up velocity added." : "Forward velocity added.";
            }
        } catch (const SourceGuard& issue) {
            boost.valid = false;
            if (offboard) state.offboard_boost_core.store(0, std::memory_order_release);
            debug.status = issue.message;
            if (offboard) logging::write(logging::Level::warning, logging::Channel::skater, issue.message);
        } catch (...) {
            boost.valid = false;
            if (offboard) state.offboard_boost_core.store(0, std::memory_order_release);
            debug.status = "Velocity boost failed during the physics update.";
            if (offboard) logging::write(logging::Level::warning, logging::Channel::skater, debug.status);
        }
    };
    apply_boost(debug.forward_velocity, debug.forward_velocity_updates, false);
    apply_boost(debug.up_velocity, debug.up_velocity_updates, true);
    apply_boost(debug.offboard_up_velocity, debug.offboard_up_velocity_updates, true, true);
    const auto& request = debug.noclip_velocity;
    if (!debug.noclip || !request.valid || request.core != core) return;
    try {
        if (GetTickCount64() >= request.expires) { debug_stop_noclip(debug); debug.status = "Flight input timed out."; return; }
        const auto bodies = debug_noclip_bodies(state.trial.base, request.client, request.entity);
        source_require(bodies.core == core, "Skater physics was replaced; flight stopped.");
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(request.entity, 0x7e0) == 0,
            "A teleport started; flight stopped.");
        for (float v : request.velocity) source_require(std::isfinite(v) && std::abs(v) <= 6000, "Invalid flight input.");
        reader.verify();
        if (bodies.seconds == 0) { debug.noclip_altitude_valid = false; return; }
        // Off-board the native drive only chases the motion target through a
        // spring, at a capped walking speed while grounded. Give the skeleton
        // bodies the flight velocity directly so the root (and the camera) keeps
        // up at any speed and stops dead on release. The carried/detached board
        // is left alone; its bodies only receive velocity while riding.
        const float height = bodies.offboard ? bodies.root[1] : bodies.board_height;
        source_require(std::isfinite(height) && std::abs(height) <= 1000000, "Invalid flight altitude.");
        if (!debug.noclip_altitude_valid || debug.noclip_altitude_offboard != bodies.offboard) {
            debug.noclip_altitude = height;
            debug.noclip_altitude_offboard = bodies.offboard;
        }
        debug.noclip_altitude_valid = true;
        debug.noclip_altitude = std::clamp(debug.noclip_altitude, height - 3.0f, height + 3.0f);
        auto velocity = request.velocity;
        // Correct gravity drift without changing world gravity or teleporting.
        velocity[1] += std::clamp((debug.noclip_altitude - height) * 12.0f, -8.0f, 8.0f);
        debug.noclip_altitude += request.velocity[1] * bodies.seconds;
        // Match native velocity writers: XYZ at +70 and dirty bit 8 at +60.
        // Preserve W, angular velocity, transforms, contacts and other flags.
        // Runs after the simulation's movement-state update.
        for (std::size_t i = bodies.offboard ? 9 : 0; i < bodies.parts.size(); ++i) {
            const auto body = bodies.parts[i];
            const auto flags = reader.value<std::uint32_t>(body, 0x60);
            body_write(body + 0x70, velocity);
            body_write(body + 0x60, flags | 8u);
        }
        ++debug.noclip_velocity_updates;
    } catch (const SourceGuard& issue) { debug_stop_noclip(debug); debug.status = issue.message; }
      catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a physics error."; }
}
struct JumpScale {
    std::atomic<bool> pending{};
    std::uintptr_t client{}, entity{}, core{};
    ULONGLONG expires{};
    float factor{1};
    std::atomic<int> outcome{};
    std::atomic<float> up_speed{};
};
JumpScale& jump_scale() { static auto* value = new JumpScale; return *value; }
// The trainer's hippy jump height: scale the upward velocity the game gave the off-board skater.
void trainer_apply_jump_scale(std::uintptr_t core) noexcept {
    auto& j = jump_scale();
    if (!j.pending.load(std::memory_order_acquire) || j.core != core) return;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    j.pending.store(false, std::memory_order_release);
    try {
        if (GetTickCount64() >= j.expires) { j.outcome.store(-3); return; }
        const auto bodies = debug_noclip_bodies(state.trial.base, j.client, j.entity);
        source_require(bodies.core == core, "Skater physics was replaced.");
        // On the board the game drives a jump along its own path and undoes a velocity change.
        if (!bodies.offboard) { j.outcome.store(-2); return; }
        float before{};
        const bool scaled = scale_offboard_up_velocity(state.trial.base, core, bodies.context, bodies.rig_wrapper, j.factor, &before);
        j.up_speed.store(before);
        j.outcome.store(scaled ? 2 : before < 0.5f ? -1 : -2);
    } catch (...) { j.outcome.store(-3); }
}
// The trainer's push speed. On the board the game steers the skater to the speed its trick
// scripts ask for (context +0x17f4) and never runs its own code for the push tuning values.
// Measured: a tapped push asks for the skater's own speed kept between 4 and about 9.5 m/s, a
// held one walks up 4.95, 6.98, 8.5 and 9.1 m/s, and the game holds that speed for about six
// seconds after the last push before the skater coasts. A multiplier on that target would
// feed back (the target follows the speed: x2 ran away to 17 m/s), so after each physics step
// of a riding skater:
//  - `factor` over 1: once the game has the skater at its tap speed it is carried on to
//    4 m/s x factor; a held push that has reached the game's last step is carried on to that
//    step x factor, and stays there while the game holds its own;
//  - `factor` under 1: the skater is held down to the game's target x factor (it then never
//    reaches the speed the next step would need, so nothing feeds back).
// Auto push: the game hands its flag to the animation and nothing comes of it (measured: the
// same coast-down with it on). With `cruise` set, a rolling skater that is not braking gains
// speed up to it.
constexpr float push_gain = 0.2f;    // m/s the game's own push gains each physics step (0 to 4 m/s in 19 steps)
constexpr float tap_speed = 4.0f;    // m/s the game's tapped push settles at
constexpr float last_step = 0.95f;   // of the stock top pushing speed: the game's last step is 9.1 of 9.25 m/s
constexpr float cruise_gain = 0.15f; // m/s each physics step: the game steers the speed back toward its own, so less does nothing
constexpr int held_steps = 36;       // a push flagged this long is held (a tap lasts 19 steps)
constexpr int hold_steps = 360;      // how long after a push the game holds its speed
struct PushSpeed {
    std::atomic<float> factor{1}, stock{9.25f}, cruise{};
    std::uintptr_t client{}, entity{};
    std::atomic<ULONGLONG> expires{};
    // Physics thread only.
    int pushing{}, since_push{hold_steps};
    bool carried{}; // a held push is being carried past the game's last step
};
PushSpeed& push_speed() { static auto* value = new PushSpeed; return *value; }
void trainer_push_speed(std::uintptr_t core) noexcept {
    auto& p = push_speed();
    const float factor = p.factor.load(std::memory_order_relaxed), cruise = p.cruise.load(std::memory_order_relaxed);
    if ((factor == 1 && cruise <= 0) || GetTickCount64() >= p.expires.load(std::memory_order_acquire)) return;
    const auto watch = watched_physics_state();
    if (!watch.valid || watch.state != 100) return; // riding the ground
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, p.client, p.entity);
        if (bodies.core != core || bodies.offboard || bodies.parts.empty()) return;
        SourceReader reader;
        const float target = reader.value<float>(bodies.context, 0x17f4);
        // What the game's brake code tests: a brake held (bit 2) or one squeezed by some amount (bit 1, +0x1880).
        const auto requests = reader.value<std::uint32_t>(bodies.context, 0x13c4);
        const bool braking = (requests & 4u) != 0 || ((requests & 2u) != 0 && std::abs(reader.value<float>(bodies.context, 0x1880)) > 0.05f);
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        if (requests & 0x40u) { // the push request the game's own push code tests
            ++p.pushing;
            p.since_push = 0;
        } else {
            p.pushing = 0;
            if (p.since_push < hold_steps) ++p.since_push;
        }
        const float speed = std::sqrt(velocities[0][0] * velocities[0][0] + velocities[0][2] * velocities[0][2]);
        if (!(speed >= 0.5f) || !(speed < 1000.0f)) return;
        const float stock = p.stock.load(std::memory_order_relaxed);
        const bool pushed = target > 0.5f && target < 100.0f && p.since_push < hold_steps; // the game is holding a push speed
        const bool at_last_step = pushed && target >= stock * last_step && speed >= target * 0.9f;
        if (braking || !at_last_step) p.carried = false;
        else if (p.pushing >= held_steps) p.carried = true;
        float change = 0;
        if (braking) {
        } else if (pushed && factor > 1 && p.carried) {
            change = std::min(push_gain, target * factor - speed);
        } else if (pushed && factor > 1 && speed >= tap_speed * 0.85f && speed < tap_speed * factor) {
            change = std::min(push_gain, tap_speed * factor - speed);
        } else if (pushed && factor < 1 && speed > target * factor && speed <= target * 1.1f) {
            change = -std::min(push_gain, speed - target * factor); // pushed speed only: a hill's is faster than the target
        } else if (cruise > 0 && speed >= 1.0f && speed < cruise) {
            change = std::min(cruise_gain, cruise - speed);
        }
        if (std::abs(change) < 0.005f) return;
        const float scale = (speed + change) / speed;
        // Like the native velocity writers: XYZ at +70 and the dirty bit 8 at +60.
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i][0] *= scale;
            velocities[i][2] *= scale;
            body_write(bodies.parts[i] + 0x70, velocities[i]);
            body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
    } catch (...) {}
}
void noclip_physics_update(std::uintptr_t core) {
    const auto original = source_state().velocity_update_original.load(std::memory_order_acquire);
    if (original) original(core);
    noclip_apply_velocity(core);
    trainer_apply_jump_scale(core);
    trainer_push_speed(core);
}
bool noclip_motion_target(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float,16>* supplied, std::array<float,16>& target) noexcept {
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
        state.busy.test_and_set(std::memory_order_acquire)) return false;
    SourceBusyScope scope{state.busy};
    auto& debug = state.trial.debug;
    auto& boost = debug.offboard_up_velocity;
    if (!debug.noclip && boost.valid && boost.applied) {
        try {
            SourceReader reader;
            if (reader.pointer(boost.core, 0x438) != rig || reader.pointer(boost.core, 0x3c0) != context)
                return false;
            if (GetTickCount64() >= boost.expires) return false;
            const auto bodies = debug_noclip_bodies(state.trial.base, boost.client, boost.entity);
            source_require(bodies.core == boost.core && bodies.rig_wrapper == rig && bodies.context == context,
                "Off-board boost skater changed.");
            source_require(!debug.park_editor && reader.value<std::uint8_t>(boost.entity, 0x7e0) == 0,
                "Off-board boost interrupted by editing or teleporting.");
            source_require(reader.raw(reinterpret_cast<std::uintptr_t>(supplied), target.data(), sizeof(target)) &&
                valid_flight_transform(target), "Invalid off-board motion target.");
            // Native off-board movement drives the skeleton toward this target.
            // Body velocity alone is undone by that drive on the same step.
            // Lead only Y for the launch; keep native X/Z, rotation and collision flags.
            target[13] = std::max(target[13], bodies.root[1] + boost.velocity[1] * bodies.seconds);
            source_require(valid_flight_transform(target), "Off-board boost target is out of bounds.");
            reader.verify();
            if (bodies.offboard)
                source_require(ensure_offboard_up_velocity(state.trial.base, bodies.core, context, rig, boost.velocity[1]),
                    "Off-board boost motion ownership changed.");
            return bodies.seconds > 0;
        } catch (const SourceGuard& issue) { debug.status = issue.message; }
          catch (...) { debug.status = "Off-board boost motion failed."; }
        boost.valid = false;
        state.offboard_boost_core.store(0, std::memory_order_release);
        return false;
    }
    const auto& request = debug.noclip_velocity;
    if (!debug.noclip || !request.valid) return false;
    try {
        SourceReader reader;
        // Foreign native calls forward without altering the local flight session.
        if (reader.pointer(request.core, 0x438) != rig || reader.pointer(request.core, 0x3c0) != context) return false;
        if (GetTickCount64() >= request.expires) { debug_stop_noclip(debug); debug.status = "Flight input timed out."; return false; }
        const auto bodies = debug_noclip_bodies(state.trial.base, request.client, request.entity);
        source_require(bodies.core == request.core && bodies.rig_wrapper == rig && bodies.context == context,
            "Skater motion changed; flight stopped.");
        if (!bodies.offboard || bodies.seconds == 0) return false;
        source_require(reader.value<std::uint8_t>(request.entity, 0x7e0) == 0, "A teleport started; flight stopped.");
        std::array<float,16> previous{};
        source_require(reader.raw(reinterpret_cast<std::uintptr_t>(supplied), target.data(), sizeof(target)) &&
            reader.raw(rig + 0x4720, previous.data(), sizeof(previous)) &&
            valid_flight_transform(target) && valid_flight_transform(previous), "Invalid skater motion target.");
        // The new build moved the simulation counter to +180. +170 now holds
        // state flags and can stay constant, suppressing every subsequent move.
        (void)reader.value<std::uint32_t>(request.core, 0x180);
        // Lead the skater's current root by one step instead of integrating the
        // previous target. The native drive is a spring toward this target: a
        // free-running target stretched it without bound at high speed (the
        // skater outran the camera) and the ragdoll snapped back on release.
        // Anchoring to the root bounds the stretch to one step and leaves no
        // stored energy when the input stops.
        for (std::size_t i = 0; i < 3; ++i) {
            source_require(std::isfinite(request.velocity[i]) && std::abs(request.velocity[i]) <= 6000, "Invalid flight input.");
            source_require(std::isfinite(bodies.root[i]) && std::abs(bodies.root[i]) <= 1000000, "Invalid skater root position.");
            target[12+i] = bodies.root[i] + request.velocity[i] * bodies.seconds;
        }
        source_require(valid_flight_transform(target), "Flight motion target is out of bounds.");
        reader.verify();
        source_require(sync_offboard_flight_velocity(state.trial.base, bodies.core, context, rig, request.velocity),
            "Off-board flight velocity ownership changed; flight stopped.");
        ++debug.noclip_motion_updates;
        return true;
    } catch (const SourceGuard& issue) { debug_stop_noclip(debug); debug.status = issue.message; }
      catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a motion error."; }
    return false;
}
void noclip_skater_motion(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float,16>* supplied, std::uint8_t flags) {
    const auto original = source_state().motion_original.load(std::memory_order_acquire);
    alignas(16) std::array<float,16> target{};
    const auto* motion = noclip_motion_target(rig, context, supplied, target) ? &target : supplied;
    if (original) original(rig, context, motion, flags);
    if (source_state().offboard_boost_core.load(std::memory_order_acquire)) {
        std::uintptr_t core{};
        if (memory::peek(rig + 0x4630, core) &&
            core == source_state().offboard_boost_core.load(std::memory_order_acquire))
            noclip_apply_velocity(core);
    }
}
}
}

namespace dingosdk {
using namespace client_source::detail;

bool queue_jump_scale(std::uintptr_t client, std::uintptr_t entity, float factor) noexcept {
    SourceLastError error;
    try {
        auto& state = source_state();
        auto& j = jump_scale();
        if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
            !std::isfinite(factor) || factor <= 0 || factor > 20 || j.pending.load(std::memory_order_acquire)) return false;
        const auto bodies = debug_noclip_bodies(state.trial.base, client, entity);
        j.client = client;
        j.entity = entity;
        j.core = bodies.core;
        j.factor = factor;
        j.expires = GetTickCount64() + 150;
        j.outcome.store(0);
        j.pending.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept {
    auto& p = push_speed();
    p.client = client;
    p.entity = entity;
    p.cruise.store(cruise > 0 && cruise < 100 ? cruise : 0.0f, std::memory_order_relaxed);
    p.stock.store(stock > 1 && stock < 100 ? stock : 9.25f, std::memory_order_relaxed);
    p.factor.store(factor > 0.02f && factor <= 50 ? factor : 1.0f, std::memory_order_relaxed);
    p.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
JumpScaleResult take_jump_scale_result() noexcept {
    auto& j = jump_scale();
    if (j.pending.load(std::memory_order_acquire)) return {};
    return {j.outcome.exchange(0), j.up_speed.load()};
}

bool start_client_noclip_velocity(std::uintptr_t base) noexcept {
    SourceLastError error;
    try {
        auto& state = source_state();
        std::lock_guard lock(state.initialization_mutex);
        if (!state.initialized.load() || state.trial.base != base) return false;
        if (state.velocity_guard_attempted) return state.velocity_guard_active.load();
        state.velocity_guard_attempted = true;
        SourceReader reader;
        std::array<unsigned char, 32> bytes{};
        if (!reader.raw(base + spawn::physics_update, bytes.data(), bytes.size()) || bytes != spawn::physics_update_prefix ||
            reader.pointer(base + addr::no_bail::bail_core_vtable, 0x58) != base + spawn::physics_update ||
            !reader.raw(base + spawn::skater_motion, bytes.data(), bytes.size()) || bytes != spawn::skater_motion_prefix ||
            !offboard_flight_compatible(base)) return false;
        reader.verify();
        auto* target = reinterpret_cast<void*>(base + spawn::physics_update);
        auto* motion_target = reinterpret_cast<void*>(base + spawn::skater_motion);
        void* original{};
        if (hook_prepare(target, reinterpret_cast<void*>(&noclip_physics_update), &original) != HookOk) return false;
        if (!original) { (void)hook_remove(target); return false; }
        state.velocity_update_original.store(reinterpret_cast<SourcePhysicsUpdate>(original), std::memory_order_release);
        original = nullptr;
        if (hook_prepare(motion_target, reinterpret_cast<void*>(&noclip_skater_motion), &original) != HookOk) {
            (void)hook_remove(target); state.velocity_update_original.store(nullptr); return false;
        }
        if (!original) {
            (void)hook_remove(motion_target); (void)hook_remove(target);
            state.velocity_update_original.store(nullptr); return false;
        }
        state.motion_original.store(reinterpret_cast<SourceSkaterMotion>(original), std::memory_order_release);
        // Keep trampolines on uncertain enable results; handlers still forward.
        if (hook_enable(target) != HookOk || hook_enable(motion_target) != HookOk) return false;
        state.velocity_guard_active.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
}
