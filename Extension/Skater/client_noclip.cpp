#include "client_source_spawn.h"
#include "skater_push.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "offboard_flight.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "free_flight.h"
#include "Engine/Game/Multiplayer/session_competition.h"
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
            source_require(session_boosts_allowed(), "Boosts are disabled while competition physics are on.");
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
    // Animator Studio open: the skater (and his board) stop where they are, as noclip's writers do it:
    // velocity at +0x70 with dirty bit 8 at +0x60, every step until the studio closes.
    if (auto& brake = debug.studio_brake; brake.valid && brake.core == core) {
        try {
            if (GetTickCount64() >= brake.expires) brake.valid = false;
            else {
                const auto bodies = debug_noclip_bodies(state.trial.base, brake.client, brake.entity);
                if (bodies.core == core && !bodies.wipeout) {
                    // Off the board, the carried board's bodies (0-8) are the game's alone, as noclip leaves them.
                    const std::size_t first = bodies.offboard ? 9 : 0;
                    SourceReader reader;
                    std::array<std::uint32_t, 32> flags{};
                    std::array<std::array<float, 3>, 32> velocities{};
                    for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                        flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
                        velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
                    }
                    reader.verify();
                    // Only the rolling stops; up and down stay the game's (gravity, the ground), the way the
                    // velocity boosts change a body's speed. A hard stop on every axis tore the board apart.
                    for (std::size_t i = first; i < bodies.parts.size(); ++i) {
                        auto v = velocities[i];
                        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                        v[0] *= 0.6f;
                        v[2] *= 0.6f;
                        body_write(bodies.parts[i] + 0x70, v);
                        body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
                    }
                }
            }
        } catch (...) { brake.valid = false; }
    }
    const auto& request = debug.noclip_velocity;
    if (!debug.noclip || !request.valid || request.core != core) return;
    try {
        source_require(session_noclip_allowed(), "Noclip is disabled while competition physics are on.");
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
// Trainer: called after each physics update (see set_physics_step_callback).
std::atomic<void (*)() noexcept> physics_step_callback{};
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
    // A plugin's cruise control (set_plugin_cruise): the speed to ride at (0 none), whether it may
    // ease the skater off, and until when it holds.
    std::atomic<float> plugin{};
    std::atomic<bool> plugin_slow{};
    std::atomic<ULONGLONG> plugin_expires{};
    // Physics thread only.
    int pushing{}, since_push{hold_steps};
    bool carried{}; // a held push is being carried past the game's last step
    float reached{}; // the speed pushing has been carried to past the game's own ceiling (0: none)
};
PushSpeed& push_speed() { static auto* value = new PushSpeed; return *value; }
void trainer_push_speed(std::uintptr_t core) noexcept {
    auto& p = push_speed();
    const float factor = p.factor.load(std::memory_order_relaxed), cruise = p.cruise.load(std::memory_order_relaxed);
    const ULONGLONG ticks = GetTickCount64();
    const float plugin = ticks < p.plugin_expires.load(std::memory_order_acquire) ? p.plugin.load(std::memory_order_relaxed) : 0.0f;
    const bool plugin_slow = plugin > 0 && p.plugin_slow.load(std::memory_order_relaxed);
    if ((factor == 1 && cruise <= 0 && plugin <= 0) || ticks >= p.expires.load(std::memory_order_acquire)) return;
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
        // The push class's speeds (the trainer raises those) are what a push aims for, but the
        // game's speed model settles near 10 to 11 m/s however high they are (measured with them at
        // 360 to 860 m/s: its own target stayed at 9.6 to 10 m/s). Past the stock ceiling a push is
        // carried on here, at the game's own gain, to the stock ceiling x factor, and that speed
        // is kept for as long as the game keeps a pushed speed.
        const float ceiling = stock * factor;
        if (braking || factor <= 1 || p.since_push >= hold_steps || speed < stock * 0.85f) p.reached = 0;
        if (braking) {
        } else if (factor > 1 && p.pushing > 0 && speed >= stock * 0.85f && speed < ceiling) {
            change = std::min(push_gain, ceiling - speed);
            p.reached = std::max(p.reached, speed + change);
        } else if (factor > 1 && p.reached > speed && p.since_push < hold_steps) {
            change = std::min(push_gain, p.reached - speed);
        } else if (pushed && factor < 1 && speed > target * factor && speed <= target * 1.1f) {
            change = -std::min(push_gain, speed - target * factor); // pushed speed only: a hill's is faster than the target
        } else if (plugin_slow && !pushed && speed > plugin + 0.3f) {
            change = -std::min(0.1f, speed - plugin); // riding up on what it keeps pace with: ease off (never while pushing)
        } else if (plugin > 0 && speed >= 1.0f && speed < plugin) {
            change = std::min(cruise_gain, plugin - speed); // a plugin's cruise control, before auto push
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
// A plugin's push (skater_push.h), written by the next on-board physics step.
struct PushVelocity {
    std::atomic<bool> pending{};
    std::uintptr_t client{},entity{},core{};
    std::array<float,3> velocity{};
    std::uint64_t expires{};
};
PushVelocity& push_velocity(){static PushVelocity r;return r;}
void apply_push_velocity(std::uintptr_t core) noexcept {
    auto& request=push_velocity();auto& state=source_state();
    if(!request.pending.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire))return;
    SourceBusyScope scope{state.busy};
    try {
        const auto wanted=skater_push::peek();
        // Held with no push: the board stays still; a push is written once it is asked for.
        const bool hold=skater_push::holding() && request.velocity==std::array<float,3>{};
        if(!competition_stock_physics() || GetTickCount64()>=request.expires || !wanted || *wanted!=request.velocity) {
            request.pending.store(false,std::memory_order_release);return;
        }
        if(request.core!=core)return;
        const auto bodies=debug_noclip_bodies(state.trial.base,request.client,request.entity);
        if(bodies.core!=core || bodies.offboard)return;
        SourceReader reader;if(reader.value<std::uint8_t>(request.entity,0x7e0)!=0)return;
        std::array<std::array<float,3>,32> velocities{};std::array<std::uint32_t,32> flags{};
        for(std::size_t i=0;i<bodies.parts.size();++i) {
            velocities[i]=reader.value<std::array<float,3>>(bodies.parts[i],0x70);
            flags[i]=reader.value<std::uint32_t>(bodies.parts[i],0x60);
            // A push must discard fall energy as well as set absolute horizontal speed.
            velocities[i]=request.velocity;
        }
        reader.verify();
        for(std::size_t i=0;i<bodies.parts.size();++i) {
            body_write(bodies.parts[i]+0x70,velocities[i]);
            if(hold)body_write(bodies.parts[i]+0x90,std::array<float,3>{});
            body_write(bodies.parts[i]+0x60,flags[i]|(hold?0x28u:8u));
        }
        // Hold the validated local board still while the plugin holds it (a
        // countdown, a pending return). Native teleport must not carry old fall
        // velocity away from its destination before the pushed speed is applied.
        if(hold)return;
        // A queued request is not delivery: bail/teleport physics may still be
        // busy or have replaced the body. Acknowledge only the actual write.
        skater_push::accepted(request.velocity);
        request.pending.store(false,std::memory_order_release);
    }catch(...){}
}
// A plugin's velocity (set_plugin_velocity): written, or added, by the next physics step that has the
// local skater on its board, on the ground or in the air, within 500 ms. It rides on the trainer's
// skater (set_push_speed), as cruise control does.
struct PluginVelocity {
    std::mutex mutex;
    std::array<float, 3> velocity{};
    bool add{};
    ULONGLONG expires{};
    std::atomic<bool> pending{};
};
PluginVelocity& plugin_velocity() { static auto* value = new PluginVelocity; return *value; }
void apply_plugin_velocity(std::uintptr_t core) noexcept {
    auto& v = plugin_velocity();
    if (!v.pending.load(std::memory_order_acquire)) return;
    std::unique_lock lock(v.mutex, std::try_to_lock);
    if (!lock) return;
    const ULONGLONG ticks = GetTickCount64();
    if (ticks >= v.expires) {
        v.pending.store(false, std::memory_order_release);
        return;
    }
    auto& p = push_speed();
    if (ticks >= p.expires.load(std::memory_order_acquire)) return;
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, p.client, p.entity);
        if (bodies.core != core || bodies.offboard || bodies.parts.empty()) return;
        SourceReader reader;
        if (reader.value<std::uint8_t>(p.entity, 0x7e0) != 0) return; // teleporting
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t a = 0; a < 3; ++a) velocities[i][a] = v.add ? velocities[i][a] + v.velocity[a] : v.velocity[a];
            body_write(bodies.parts[i] + 0x70, velocities[i]);
            body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
        v.pending.store(false, std::memory_order_release);
    } catch (...) {}
}
// ---- pump power --------------------------------------------------------------------------------
// What a pump gains, times a factor. The game's trick scripts decide when the skater is pumping
// (Bool.Intent.Pumping, read by the trainer) and the game's physics then speeds the skater up;
// which numbers size that has not been found. Measured on a 12 m ramp (the skater's energy, speed
// and height, after each physics step against the step before): a pump of the game's own gains
// about 0.3 to 1 m/s over at most 28 steps, and the game gives less the faster the skater already
// is, so a share of the game's gain fades to nothing at speed (x 100 felt like x 1). The trainer's
// share is therefore its own: for every physics step of a pump it adds pump_rate x (factor - 1)
// along the direction of travel, and under 1 takes that much away. The game takes about half of
// an addition back while the pump lasts (measured at x 10 and x 100; PumpMaxDeceleration is not
// what does it), so the rate is twice what one more of the game's pumps would need at x 2.
// Owner, 2026-10-07: x 100 is strong on a small quarter and on a 12 m half pipe.
constexpr float pump_gravity = 9.81f; // m/s2, as flights measure it
constexpr float pump_rate = 0.03f;    // m/s each physics step
constexpr float pump_step = 3.0f;     // m/s the trainer may add or take in one physics step
struct PumpPower {
    std::atomic<float> factor{1};
    std::atomic<bool> pumping{}, measure{};
    std::uintptr_t client{}, entity{};
    std::atomic<ULONGLONG> expires{};
    std::atomic<std::uint32_t> pumps{};
    std::atomic<float> gained{}, given{}, speed{};
    std::atomic<std::uint32_t> steps{};
    // Physics thread only.
    std::uint32_t count{};
    bool primed{}, running{};
    float energy{}, sum{}, credit{};
};
PumpPower& pump_power() { static auto* value = new PumpPower; return *value; }
void trainer_pump_power(std::uintptr_t core) noexcept {
    auto& p = pump_power();
    const float factor = p.factor.load(std::memory_order_relaxed);
    const auto stop = [&] { p.primed = p.running = false; };
    if ((factor == 1 && !p.measure.load(std::memory_order_relaxed)) || GetTickCount64() >= p.expires.load(std::memory_order_acquire)) return stop();
    const auto watch = watched_physics_state();
    if (!watch.valid || (watch.state != 100 && watch.state != 103)) return stop(); // riding, standing or crouched
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, p.client, p.entity);
        if (bodies.core != core || bodies.offboard || bodies.parts.empty()) return stop();
        SourceReader reader;
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        const auto& v = velocities[0];
        const float squared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2], speed = std::sqrt(squared);
        const float height = bodies.root[1];
        if (!(speed >= 1.0f) || !(speed < 1000.0f) || !std::isfinite(height)) return stop();
        const float energy = 0.5f * squared + pump_gravity * height;
        if (!p.primed || !p.pumping.load(std::memory_order_relaxed)) { // coasting: only keep up
            p.primed = true;
            p.running = false;
            p.energy = energy;
            return;
        }
        if (!p.running) {
            p.running = true;
            p.sum = p.credit = 0;
            p.count = 0;
            p.pumps.fetch_add(1, std::memory_order_relaxed);
        }
        const float step = energy - p.energy;
        p.energy = energy;
        if (std::abs(step) > speed * 2.0f + 2.0f) return; // a teleport or a hit, not a pump
        p.sum += step;
        float change = 0;
        if (factor != 1) change = std::clamp((factor - 1) * pump_rate, -pump_step, pump_step);
        if (speed + change < 1.0f || speed + change > 300.0f) change = 0;
        ++p.count;
        if (std::abs(change) >= 0.002f) {
            const float scale = (speed + change) / speed;
            // Like the native velocity writers: XYZ at +70 and the dirty bit 8 at +60.
            for (std::size_t i = 0; i < count; ++i) {
                for (auto& axis : velocities[i]) axis *= scale;
                body_write(bodies.parts[i] + 0x70, velocities[i]);
                body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
            const float given = 0.5f * ((speed + change) * (speed + change) - squared);
            p.credit += given;
            p.energy += given; // the trainer's own share is not the game's gain
        }
        p.gained.store(p.sum, std::memory_order_relaxed);
        p.given.store(p.credit, std::memory_order_relaxed);
        p.speed.store(speed, std::memory_order_relaxed);
        p.steps.store(p.count, std::memory_order_relaxed);
    } catch (...) {}
}
// ---- gravity -----------------------------------------------------------------------------------
// In the air on the board the skater follows a record the game keeps in its trajectory object: a start point, a launch
// velocity, gravity (0, -9.8, 0) and a step, read as start + velocity t + gravity t^2 / 2 at every physics step. Measured
// with a scripted ollie (2026-10-12): the game's tuning, the gravity its jump planner reads and the bodies' own speeds
// all leave the fall at 9.8 m/s2; that record's gravity held at -4.9 gives a fall of 5.5 m/s2, an ollie of 0.70 m and
// 1.05 s (the game's: 0.36 m, 0.58 s), at -19.6 a fall of 20.4 m/s2. The game writes the record again at every launch,
// so the trainer writes its gravity after each physics step, and lets go by writing nothing.
constexpr std::uintptr_t flight_record_gravity = 0x18a0, trajectory_context = 0xb808;
constexpr float game_gravity = -9.8f;
struct AirGravity {
    std::atomic<float> factor{1};
    std::atomic<std::uint64_t> expires{};
    std::atomic<std::uintptr_t> client{}, entity{};
    std::atomic<std::uint32_t> steps{};
    // The trajectory objects the game has launched lately (the local skater's is the one that points at its context).
    std::array<std::atomic<std::uintptr_t>, 8> records{};
    std::atomic<std::uint32_t> noted{};
    std::uintptr_t held{}; // physics thread: the record whose gravity is the trainer's now
    std::uintptr_t searches{}; // physics thread: the landing searches' gravity (below) the trainer wrote, 0 when stock
    // The landing searches' gravity (0: factor), and how far the air animations keep the game's timing (0 .. 1).
    std::atomic<float> landing{}, timing{};
    // The rate that does it now, and the countdown seen last (physics thread).
    std::atomic<float> animation_rate{1};
    float countdown{-1};
    LONGLONG countdown_moved{};
};
AirGravity& air_gravity() {
    static auto* value = new AirGravity;
    return *value;
}
// The landing searches. Where a flight lands, and when, the game finds by walking trajectories through the world
// (reckoning__set_launch 0x4752aa0 every 7 steps in the air, the grind searches 0x47fb08f, 0x4803148); the gravity of each
// walk is copied from the trajectory query manager (the object at rva 0x7785da8, +0x30, (0, -9.8, 0)) into the query
// (0x4787820, +0xd0). The air state then follows the record it chose and lands at its step, and the animation counts
// down to it (TimeToLand). 0.8.1 set only that countdown again, for flat ground; on a real jump the record the skater
// followed was still planned at 9.8, so the landing stayed late (a player at x1.25, 2026-10-21). So the manager's gravity
// is the trainer's while it holds one, and goes back to the game's when it lets go: every search then plans with it.
constexpr std::uintptr_t query_manager = 0x7785da8, query_manager_gravity = 0x30;
// mov rcx, [rip + query_manager] in the reckoning's query launch (0x473e280): this build's manager.
constexpr std::uintptr_t query_manager_load = 0x473e312;
constexpr unsigned char query_manager_load_bytes[]{0x48, 0x8b, 0x0d, 0x8f, 0x7a, 0x04, 0x03};
void air_gravity_searches(AirGravity& g, std::uintptr_t base, float wanted) {
    static const bool known = std::memcmp(reinterpret_cast<const void*>(base + query_manager_load), query_manager_load_bytes,
                                          sizeof(query_manager_load_bytes)) == 0;
    if (!known) return;
    SourceReader reader;
    const auto manager = reader.pointer(base + query_manager);
    reader.verify();
    if (!manager) return;
    const auto now = reader.value<std::array<float, 3>>(manager, query_manager_gravity);
    reader.verify();
    if (now[0] != 0 || now[2] != 0 || !(now[1] < -0.1f) || !(now[1] > -250.0f)) return;
    if (now[1] != wanted) body_write(manager + query_manager_gravity + 4, wanted);
    g.searches = wanted == game_gravity ? 0 : manager;
}
// Air animations at the game's timing. The game fits a trick into the flight by the time to land (the animation's
// countdown, [[core+0x3b8]+0x10]+0xd8): the clips' time is warped toward it, but the catch at most to its own speed
// (animation-timelines.md, the type-3 windows: up to 1.5, then up to 1.0). A flight shorter than the game's gravity
// would make it (x1.31 on flat ground: a quarter shorter) so lands with the catch still playing: the stutter players
// see. With "timing" at 1 the skater's clips run faster by the time this flight would take at 9.8 over the time it
// takes, so the game sees the flight it planned for and its animations come out as they would at x1; between 0 and 1
// part of the way. The countdown is planned with the landing gravity, the flight falls with the trainer's.
constexpr std::uintptr_t core_animation_offset = 0x3b8, animation_output_offset = 0x10, output_landing_countdown = 0xd8;
constexpr std::uintptr_t flight_record_velocity = 0x1890;
void air_gravity_animation(AirGravity& g, std::uintptr_t core, std::uintptr_t record, float factor, float landing, float timing) {
    SourceReader reader;
    const auto animation = reader.pointer(core, core_animation_offset);
    reader.verify();
    const auto output = animation ? reader.pointer(animation, animation_output_offset) : 0;
    reader.verify();
    float countdown = output ? reader.value<float>(output, output_landing_countdown) : -1.0f;
    const float up = reader.value<float>(record, flight_record_velocity + 4);
    reader.verify();
    if (!std::isfinite(countdown) || countdown > 10.0f) countdown = -1.0f;
    LARGE_INTEGER clock{}, rate{};
    QueryPerformanceCounter(&clock);
    QueryPerformanceFrequency(&rate);
    if (countdown != g.countdown) g.countdown_moved = clock.QuadPart;
    // A new plan (the countdown set again, higher): the rate for this flight, from the drop it plans for.
    if (countdown > 0.02f && std::isfinite(up) && (g.countdown <= 0 || countdown > g.countdown + 0.05f)) {
        const float drop = up * countdown + 0.5f * game_gravity * landing * countdown * countdown;
        // The time to come down that far with a gravity of g x the game's.
        const auto flight = [&](float g) {
            const float root = up * up + 2.0f * game_gravity * g * drop;
            return root >= 0 ? (up + std::sqrt(root)) / (-game_gravity * g) : -1.0f;
        };
        const float stock = flight(1.0f), real = flight(factor);
        const float full = stock > 0 && real > 0 ? std::clamp(stock / real, 0.25f, 4.0f) : 1.0f;
        g.animation_rate.store(1.0f + (full - 1.0f) * timing, std::memory_order_relaxed);
    }
    g.countdown = countdown;
    // Landed, or the countdown has stood still (a bail, another state): the game's own rate.
    const bool still = clock.QuadPart - g.countdown_moved > rate.QuadPart / 10;
    if (!(countdown > 0) || still) g.animation_rate.store(1.0f, std::memory_order_relaxed);
}
void trainer_air_gravity(std::uintptr_t core) noexcept {
    auto& g = air_gravity();
    float factor = g.factor.load(std::memory_order_relaxed);
    float landing = g.landing.load(std::memory_order_relaxed);
    const float timing = g.timing.load(std::memory_order_relaxed);
    if (GetTickCount64() >= g.expires.load(std::memory_order_acquire)) factor = 1, landing = 1;
    if (!(landing > 0)) landing = factor;
    if (factor == 1 && landing == 1 && !g.held && !g.searches) {
        g.animation_rate.store(1.0f, std::memory_order_relaxed);
        return;
    }
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, g.client.load(std::memory_order_relaxed), g.entity.load(std::memory_order_relaxed));
        if (bodies.core != core || !bodies.context) return;
        bool found = false;
        for (const auto& slot : g.records) {
            const auto record = slot.load(std::memory_order_relaxed);
            if (!record) continue;
            SourceReader reader;
            const auto context = reader.pointer(record, trajectory_context);
            const auto now = reader.value<std::array<float, 3>>(record, flight_record_gravity);
            reader.verify();
            // Only a record that reads as a gravity is written: straight down, and the game's or one of the trainer's.
            if (context != bodies.context || now[0] != 0 || now[2] != 0 || !(now[1] < -0.1f) || !(now[1] > -250.0f)) continue;
            const float wanted = game_gravity * factor;
            if (now[1] != wanted) body_write(record + flight_record_gravity + 4, wanted);
            g.held = factor == 1 ? 0 : record;
            if (factor != 1) g.steps.fetch_add(1, std::memory_order_relaxed);
            if (factor != 1 && timing > 0) air_gravity_animation(g, core, record, factor, landing, timing);
            else g.animation_rate.store(1.0f, std::memory_order_relaxed);
            found = true;
            break;
        }
        if (!found) {
            g.held = 0;
            g.animation_rate.store(1.0f, std::memory_order_relaxed);
        }
        // The searches plan with the landing gravity while one is set, in the air or not (a search starts at the launch).
        if (landing != 1 || g.searches) air_gravity_searches(g, state.trial.base, game_gravity * landing);
    } catch (...) {}
}
// ---- revert boost ------------------------------------------------------------------------------
// When the game auto reverts a landing the board is still well out of line with the body and the
// game swings it round. Earlier builds of the game gained speed there; this gives it back: a
// forward speed along the direction of travel on the step of that landing. The measuring, the
// thresholds and the size of the boost are AutoRevertBoost's (Sivaes, jaq and OVM,
// github.com/Sivaes/AutoRevertBoost, GPL-3.0), taken from their play logs: clean and short 180s
// land with the board 1-3 degrees off the body, auto reverts 71-130 degrees off.
// The thresholds and sizes are RevertTuning's (client_source_spawn.h), the player's to change.
// A second rule, off as shipped (min_slip 90): board and body together out of line with the
// direction of travel, which the game then swings round. 0.4.1 had it on at 12 degrees and it paid
// ordinary short or crooked 180s, flip trick 180s among them, whose landing the game merely
// straightens (AutoRevertBoost's authors, with 60 Hz logs: those land 0 to 3 degrees off the body,
// real auto reverts 70 to 82, board bends 85 to 90). A player may still turn it on; a landing that
// counts by it alone never gets more than an auto revert does.
constexpr ULONGLONG revert_max_age_ms = 400;
// How much, at strength 1, from how far the board turned relative to the body over the flight:
// a board bend (up to 140 degrees) gives +2 m/s at 40 degrees rising to +2.6 at 140; an auto
// revert (past 140) a flat +1.5 m/s, less than any bend.
float revert_boost_amount(const RevertTuning &t, float board_rotation) {
    if (board_rotation > 140.0f) return t.auto_boost;
    return t.bend_boost + (t.full_boost - t.bend_boost) * std::clamp((board_rotation - 40.0f) / 320.0f, 0.0f, 1.0f);
}
struct RevertBoost {
    std::atomic<float> strength{}; // 0: off
    std::uintptr_t client{}, entity{};
    std::atomic<ULONGLONG> expires{};
    SRWLOCK report_lock = SRWLOCK_INIT; // the report and the tuning
    RevertBoostReport report;
    RevertTuning tuning;
    // Physics thread only.
    bool primed{};          // the landing count has been read once since the boost was switched on
    std::uint64_t seen{};   // the last landing handled
    ULONGLONG cooldown_until{};
};
RevertBoost& revert_boost() { static auto* value = new RevertBoost; return *value; }
void trainer_revert_boost(std::uintptr_t core) noexcept {
    auto& r = revert_boost();
    const float strength = r.strength.load(std::memory_order_relaxed);
    const auto now = GetTickCount64();
    if (!(strength > 0) || now >= r.expires.load(std::memory_order_acquire)) {
        r.primed = false; // nothing is read while it is off
        return;
    }
    SourceLastError error;
    auto& state = source_state();
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{state.busy};
    try {
        const auto bodies = debug_noclip_bodies(state.trial.base, r.client, r.entity);
        if (bodies.core != core || bodies.parts.empty()) return;
        SourceReader reader;
        const auto selector = reader.pointer(bodies.core, 0x440);
        source_require(reader.pointer(selector, 8) == bodies.context, "Skater physics selector changed.");
        // The skater's world matrix, where debug_skater reads it.
        const auto collection = reader.pointer(r.entity, 0x70);
        source_require(reader.pointer(collection) == r.entity, "Skater transform owner changed.");
        const auto first = reader.value<std::uint8_t>(collection, 9), extra = reader.value<std::uint8_t>(collection, 10);
        source_require(reader.value<std::uint8_t>(collection, 8) <= 128 && first <= 128 && extra <= 32, "Skater transform layout changed.");
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        watch_revert_spin(selector, collection + std::uintptr_t{0x10} + (std::uintptr_t{first} + 2 * std::uintptr_t{extra}) * 0x20, bodies.parts[0]);
        const auto landing = last_revert_landing();
        AcquireSRWLockShared(&r.report_lock);
        const auto t = r.tuning;
        ReleaseSRWLockShared(&r.report_lock);
        if (!r.primed) { // landings from before it was switched on are not ours
            r.primed = true;
            r.seen = landing.sequence;
            return;
        }
        if (landing.sequence == r.seen) return;
        r.seen = landing.sequence;
        float board_offset = std::fmod(std::abs(landing.board_offset_degrees), 180.0f);
        board_offset = std::min(board_offset, 180.0f - board_offset); // 0: lined up, forward or fakie
        const float spin = std::abs(landing.spin_degrees);
        const float board_rotation = landing.board_valid ? std::abs(landing.board_spin_degrees - landing.spin_degrees) : 0.0f;
        const float speed = std::hypot(velocities[0][0], velocities[0][2]);
        // Out of line with the travel, forward or fakie: 0 = lined up, 90 = sideways.
        float slip = 0.0f;
        if (std::isfinite(speed) && speed >= 1.0f) {
            slip = std::fmod(std::abs(landing.heading_degrees - std::atan2(velocities[0][0], velocities[0][2]) * 57.29578f), 180.0f);
            slip = std::min(slip, 180.0f - slip);
        }
        const bool twisted = landing.board_valid && board_offset >= t.min_twist;
        const bool slipped = t.min_slip < 90.0f && slip >= t.min_slip;
        float amount = revert_boost_amount(t, board_rotation);
        if (!twisted) amount = std::min(amount, t.auto_boost);
        const float added = std::isfinite(speed) ? std::min(amount * strength, t.max_speed - speed) : 0.0f;
        const char *outcome = spin < t.min_spin ? "no boost: not enough spin"
            : !twisted && !slipped ? "no boost: the board landed in line with the body (a clean landing)"
            : landing.to < 100 || landing.to >= 200 || bodies.offboard ? "no boost: not riding"
            : now < landing.landed_at || now - landing.landed_at > revert_max_age_ms ? "no boost: seen too late"
            : static_cast<float>(landing.air_ms) < t.min_air * 1000.0f ? "no boost: too short a flight"
            : now < r.cooldown_until ? "no boost: too soon after the last"
            : !std::isfinite(speed) || speed < 1.0f || !(added > 0.005f) ? "no boost: standing still or already at the speed limit"
            : "boost";
        const bool fire = std::string_view(outcome) == "boost";
        AcquireSRWLockExclusive(&r.report_lock);
        r.report = {r.report.sequence + 1, spin, board_offset, board_rotation, slip, speed, fire ? added : 0.0f, static_cast<std::uint32_t>(landing.air_ms),
                    landing.to, landing.board_valid, outcome};
        ReleaseSRWLockExclusive(&r.report_lock);
        if (!fire) return;
        const float scale = (speed + added) / speed;
        // Like the native velocity writers: XYZ at +70 and the dirty bit 8 at +60.
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i][0] *= scale;
            velocities[i][2] *= scale;
            body_write(bodies.parts[i] + 0x70, velocities[i]);
            body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
        r.cooldown_until = now + static_cast<ULONGLONG>(std::clamp(t.cooldown, 0.0f, 60.0f) * 1000.0f);
    } catch (...) {}
}
// ---- transition assist -------------------------------------------------------------------------
// Coming off a coping grind, a stall or a handplant back into the ramp. The trainer's client tick says when the
// skater is on a coping (a grind that began on steep ground), which way the coping runs and which side the ramp is on;
// the rest happens here, on the physics steps after the grind ends:
// - carry: the speed the skater had along the coping is put back for a third of a second, and the speed across it
//   (into the ramp) is capped, so the skater rolls off at about the angle they came in at. Skate 3 never turned the
//   velocity at the end of a grind: it nudged the board sideways up to 1 m/s and left the rest alone.
// - unstick: Skate 3's hang-up rule. Slower than 1.1 m/s for more than ten steps after leaving the coping (or after
//   landing a flight off a transition) is a hang-up: the skater is pushed into the ramp and down until moving again.
// - plant: when a handplant ends, the skater leaves the lip at least 0.9 m/s into the ramp and without the downward
//   speed the plant left them with (Skate 3: 0.9 m/s outward, a ballistic arc from above the lip).
// Everything here is a first build on behaviour not yet measured in this game (2026-10-11): the report is how each
// is checked in play.
constexpr std::uint32_t transition_carry_steps = 20, transition_watch_steps = 90, transition_plant_steps = 12;
constexpr float transition_hang_speed = 1.1f; // m/s, Skate 3's
constexpr std::uint32_t transition_hang_steps = 10;
constexpr float transition_kick = 0.12f, transition_kick_down = 0.10f; // m/s a step at strength 1
constexpr float transition_plant_out = 0.9f; // m/s into the ramp, Skate 3's
struct TransitionRun {
    std::atomic<ULONGLONG> expires{};
    std::uintptr_t client{}, entity{};
    SRWLOCK lock = SRWLOCK_INIT;
    TransitionAssist assist;   // under lock
    TransitionReport report;   // under lock
    // Physics thread only.
    int phase{}; // 0 idle, 1 on a coping, 2 just left one, 3 handplanting, 4 just left a handplant, 5 in a flight off a transition, 6 just landed one
    std::uint32_t steps{}, slow{};
    std::array<float, 2> edge{}, ramp{};
    float along{}, top{}, low{};
    TransitionReport now;
};
TransitionRun& transition_run() { static auto* value = new TransitionRun; return *value; }
void trainer_transition(std::uintptr_t core) noexcept {
    auto& r = transition_run();
    if (GetTickCount64() >= r.expires.load(std::memory_order_acquire)) {
        r.phase = 0;
        return;
    }
    AcquireSRWLockShared(&r.lock);
    const auto a = r.assist;
    ReleaseSRWLockShared(&r.lock);
    const bool acting = a.carry > 0 || a.unstick > 0 || a.plant > 0;
    if (!acting && !a.measure) {
        r.phase = 0;
        return;
    }
    const auto watch = watched_physics_state();
    if (!watch.valid) return;
    const std::uint32_t state = watch.state;
    const bool grinding = (state >= 400 && state < 500) || state == 603; // grinds and slides; the lip trick
    const bool planting = state == 600;
    const bool flying = state >= 200 && state < 300;
    const bool gone = state == 300 || (state >= 500 && state < 600); // a wipeout, or off the board
    if (r.phase == 0 && !(a.coping && grinding) && !planting && !(a.transition && flying)) return;
    SourceLastError error;
    auto& source = source_state();
    if (!source.initialized.load(std::memory_order_acquire) || source.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{source.busy};
    try {
        const auto bodies = debug_noclip_bodies(source.trial.base, r.client, r.entity);
        if (bodies.core != core || bodies.parts.empty()) return;
        SourceReader reader;
        std::array<std::array<float, 3>, 32> velocities{};
        std::array<std::uint32_t, 32> flags{};
        const auto count = std::min<std::size_t>(bodies.parts.size(), velocities.size());
        for (std::size_t i = 0; i < count; ++i) {
            velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
        }
        reader.verify();
        const auto v = velocities[0]; // as the game left it this step
        const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(speed) || speed > 300.0f) return;
        const auto add = [&](float x, float y, float z) {
            // Like the native velocity writers: XYZ at +70 and the dirty bit 8 at +60.
            for (std::size_t i = 0; i < count; ++i) {
                velocities[i][0] += x;
                velocities[i][1] += y;
                velocities[i][2] += z;
                body_write(bodies.parts[i] + 0x70, velocities[i]);
                body_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
        };
        const auto publish = [&] {
            AcquireSRWLockExclusive(&r.lock);
            const auto sequence = r.report.sequence + 1;
            r.report = r.now;
            r.report.sequence = sequence;
            ReleaseSRWLockExclusive(&r.lock);
        };
        const float height = bodies.root[1];
        // A hang-up: slower than Skate 3's limit for longer than its ten frames. `x`, `z`: the way to push.
        const auto hang = [&](float x, float z) {
            if (speed < transition_hang_speed) {
                ++r.slow;
                ++r.now.hung;
            } else {
                r.slow = 0;
            }
            if (!(a.unstick > 0) || r.slow <= transition_hang_steps) return;
            add(x * transition_kick * a.unstick, -transition_kick_down * a.unstick, z * transition_kick * a.unstick);
            ++r.now.kicked;
        };
        if (a.coping && grinding && r.phase != 2) {
            // On the coping: remember how the skater is moving along and across it. Nothing is written.
            if (r.phase != 1) r.now = {};
            r.phase = 1;
            r.edge = a.edge;
            r.ramp = a.ramp;
            r.along = v[0] * r.edge[0] + v[2] * r.edge[1];
            r.now.kind = 1;
            r.now.from = state;
            r.now.in_along = r.along;
            r.now.in_across = v[0] * r.ramp[0] + v[2] * r.ramp[1];
            r.now.in_down = -v[1];
            r.top = height;
            return;
        }
        if (r.phase == 1) {
            // The grind ended. A pop is the player's own way off; a bail or a step off ends it too.
            r.now.to = state;
            if (state == 200 || gone || planting) {
                r.phase = 0;
                return;
            }
            r.phase = 2;
            r.steps = r.slow = 0;
        }
        if (r.phase == 2) {
            if (gone || state == 200 || planting || (grinding && r.steps > 2)) { // popped, bailed, or locked on again
                r.now.to_last = state;
                publish();
                r.phase = 0;
                return;
            }
            ++r.steps;
            const float along = v[0] * r.edge[0] + v[2] * r.edge[1], across = v[0] * r.ramp[0] + v[2] * r.ramp[1];
            if (a.carry > 0 && r.steps <= transition_carry_steps && std::abs(r.along) > 0.5f) {
                const float wanted = r.along * std::min(a.carry, 1.0f);
                float change_along = 0, change_across = 0;
                if (along * wanted < 0 || std::abs(along) < std::abs(wanted)) change_along = wanted - along;
                if (a.drop_speed > 0 && across > a.drop_speed) change_across = a.drop_speed - across;
                if (std::abs(change_along) > 0.01f || std::abs(change_across) > 0.01f) {
                    add(r.edge[0] * change_along + r.ramp[0] * change_across, 0.0f, r.edge[1] * change_along + r.ramp[1] * change_across);
                    ++r.now.carried;
                }
            }
            hang(r.ramp[0], r.ramp[1]);
            if (r.steps == transition_carry_steps) {
                r.now.out_along = velocities[0][0] * r.edge[0] + velocities[0][2] * r.edge[1];
                r.now.out_across = velocities[0][0] * r.ramp[0] + velocities[0][2] * r.ramp[1];
                r.now.out_down = -velocities[0][1];
                r.now.game_along = along;
                r.now.game_across = across;
                r.now.drop = r.top - height;
            }
            if (r.steps >= transition_watch_steps) {
                r.now.to_last = state;
                r.now.fall = r.top - height;
                publish();
                r.phase = 0;
            }
            return;
        }
        if (planting && r.phase != 4) {
            // On a handplant: how far the plant itself carries the skater down before it lets go.
            if (r.phase != 3) {
                r.now = {};
                r.now.kind = 2;
                r.now.from = state;
                r.ramp = a.ramp;
                r.edge = a.edge;
                r.top = height;
            }
            r.phase = 3;
            r.top = std::max(r.top, height);
            r.low = height;
            return;
        }
        if (r.phase == 3) {
            r.now.to = state;
            r.now.drop = r.top - r.low; // lost inside the plant
            r.now.in_across = v[0] * r.ramp[0] + v[2] * r.ramp[1];
            r.now.in_along = v[0] * r.edge[0] + v[2] * r.edge[1];
            r.now.in_down = -v[1];
            if (gone) {
                publish();
                r.phase = 0;
                return;
            }
            r.phase = 4;
            r.steps = r.slow = 0;
        }
        if (r.phase == 4) {
            if (gone || planting) {
                r.now.to_last = state;
                publish();
                r.phase = 0;
                return;
            }
            ++r.steps;
            const float across = v[0] * r.ramp[0] + v[2] * r.ramp[1];
            if (a.plant > 0 && r.steps <= transition_plant_steps) {
                const float blend = std::min(a.plant, 1.0f);
                const float change_across = across < transition_plant_out ? (transition_plant_out - across) * blend : 0.0f;
                const float change_up = r.steps == 1 && v[1] < 0 ? -v[1] * blend : 0.0f; // the fall starts from rest, as off the top of an arc
                if (std::abs(change_across) > 0.01f || change_up > 0.01f) {
                    add(r.ramp[0] * change_across, change_up, r.ramp[1] * change_across);
                    ++r.now.carried;
                }
            }
            hang(r.ramp[0], r.ramp[1]);
            if (r.steps == transition_plant_steps) {
                r.now.out_across = velocities[0][0] * r.ramp[0] + velocities[0][2] * r.ramp[1];
                r.now.out_along = velocities[0][0] * r.edge[0] + velocities[0][2] * r.edge[1];
                r.now.out_down = -velocities[0][1];
                r.now.game_across = across;
            }
            if (r.steps >= transition_watch_steps) {
                r.now.to_last = state;
                r.now.fall = r.top - height; // below the plant's highest point a second and a half later
                publish();
                r.phase = 0;
            }
            return;
        }
        if (flying && a.transition && r.phase != 6) {
            // A flight off a transition: which way it goes over the ground, for a hang-up where it comes down.
            if (r.phase != 5) {
                r.now = {};
                r.now.kind = 3;
                r.now.from = state;
            }
            r.phase = 5;
            const float over = std::hypot(v[0], v[2]);
            if (over > 0.3f) r.ramp = {v[0] / over, v[2] / over};
            r.now.in_along = over;
            r.now.in_down = -v[1];
            return;
        }
        if (r.phase == 5) {
            r.now.to = state;
            if (gone || planting || flying) {
                r.phase = 0;
                return;
            }
            r.phase = 6;
            r.steps = r.slow = 0;
        }
        if (r.phase == 6) {
            if (gone || flying || planting) {
                r.now.to_last = state;
                if (r.now.hung) publish();
                r.phase = 0;
                return;
            }
            ++r.steps;
            // Riding away is not a hang-up however slow (the top of a kick turn): only a skater held on an edge or in
            // one of the game's in-between states counts here.
            if (state >= 100 && state < 200) r.slow = 0;
            else hang(r.ramp[0], r.ramp[1]);
            if (r.steps >= transition_watch_steps / 2) {
                r.now.to_last = state;
                if (r.now.hung) publish(); // only a landing that hung is worth a line
                r.phase = 0;
            }
            return;
        }
        r.phase = 0;
    } catch (...) {}
}
void noclip_physics_update(std::uintptr_t core) {
    const auto original = source_state().velocity_update_original.load(std::memory_order_acquire);
    if (original) original(core);
    noclip_apply_velocity(core);
    trainer_apply_jump_scale(core);
    if (const auto callback = physics_step_callback.load(std::memory_order_acquire)) callback();
    trainer_push_speed(core);
    apply_push_velocity(core);
    apply_plugin_velocity(core);
    trainer_pump_power(core);
    trainer_air_gravity(core);
    trainer_revert_boost(core);
    trainer_transition(core);
}
bool noclip_motion_target(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float,16>* supplied, std::array<float,16>& target) noexcept {
    if(competition_stock_physics())return false;
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

bool queue_skater_push_velocity(std::uintptr_t client,std::uintptr_t entity,const std::array<float,3>& velocity) noexcept {
    try {
        auto& state=source_state();auto& request=push_velocity();
        if(!competition_stock_physics() || !start_client_noclip_velocity(state.trial.base) ||
            !skater_push::valid(velocity))return false;
        if(state.busy.test_and_set(std::memory_order_acquire))return false;
        SourceBusyScope scope{state.busy};
        if(request.pending.load(std::memory_order_acquire)) {
            if(skater_push::holding() && velocity==std::array<float,3>{} &&
               request.velocity==velocity && request.client==client && request.entity==entity) {
                request.expires=GetTickCount64()+500;return true;
            }
            if(GetTickCount64()<request.expires)return false;
            request.pending.store(false,std::memory_order_release);
        }
        const auto bodies=debug_noclip_bodies(state.trial.base,client,entity);if(bodies.offboard)return false;
        request.client=client;request.entity=entity;request.core=bodies.core;request.velocity=velocity;request.expires=GetTickCount64()+500;
        request.pending.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
void set_physics_step_callback(void (*callback)() noexcept) noexcept { physics_step_callback.store(callback, std::memory_order_release); }
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
void set_plugin_velocity(const std::array<float, 3>& velocity, bool add) noexcept {
    auto& v = client_source::detail::plugin_velocity();
    std::lock_guard lock(v.mutex);
    v.velocity = velocity;
    v.add = add;
    v.expires = GetTickCount64() + 500;
    v.pending.store(true, std::memory_order_release);
}
void set_plugin_cruise(float speed, bool may_slow) noexcept {
    auto& p = push_speed();
    p.plugin.store(speed > 0 && speed <= 30 ? speed : 0.0f, std::memory_order_relaxed);
    p.plugin_slow.store(may_slow, std::memory_order_relaxed);
    p.plugin_expires.store(speed > 0 ? GetTickCount64() + 300 : 0, std::memory_order_release);
}
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept {
    auto& p = push_speed();
    p.client = client;
    p.entity = entity;
    p.cruise.store(cruise > 0 && cruise < 100 ? cruise : 0.0f, std::memory_order_relaxed);
    p.stock.store(stock > 1 && stock < 100 ? stock : 9.25f, std::memory_order_relaxed);
    p.factor.store(factor > 0.02f && factor <= 1.0e6f ? factor : 1.0f, std::memory_order_relaxed);
    p.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
void set_air_gravity(std::uintptr_t client, std::uintptr_t entity, float factor, float landing, float timing) noexcept {
    auto& g = air_gravity();
    g.landing.store(std::isfinite(landing) && landing >= 0.02f && landing <= 20.0f ? landing : 0.0f, std::memory_order_relaxed);
    g.timing.store(std::isfinite(timing) ? std::clamp(timing, 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    g.client.store(client, std::memory_order_relaxed);
    g.entity.store(entity, std::memory_order_relaxed);
    g.factor.store(std::isfinite(factor) && factor >= 0.02f && factor <= 20.0f ? factor : 1.0f, std::memory_order_relaxed);
    g.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
float air_animation_rate() noexcept {
    auto& g = air_gravity();
    if (!(g.timing.load(std::memory_order_relaxed) > 0) || GetTickCount64() >= g.expires.load(std::memory_order_acquire)) return 1.0f;
    return g.animation_rate.load(std::memory_order_relaxed);
}
std::uint32_t air_gravity_steps() noexcept { return air_gravity().steps.load(std::memory_order_relaxed); }
void note_flight_record(std::uintptr_t trajectory) noexcept {
    auto& g = air_gravity();
    for (const auto& slot : g.records)
        if (slot.load(std::memory_order_relaxed) == trajectory) return;
    g.records[g.noted.fetch_add(1, std::memory_order_relaxed) % g.records.size()].store(trajectory, std::memory_order_relaxed);
}
void set_pump_power(std::uintptr_t client, std::uintptr_t entity, float factor, bool pumping, bool measure) noexcept {
    auto& p = pump_power();
    p.client = client;
    p.entity = entity;
    p.factor.store(factor >= 0 && factor <= 1.0e6f ? factor : 1.0f, std::memory_order_relaxed);
    p.pumping.store(pumping, std::memory_order_relaxed);
    p.measure.store(measure, std::memory_order_relaxed);
    p.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
PumpPowerReport pump_power_report() noexcept {
    auto& p = pump_power();
    return {p.pumps.load(std::memory_order_relaxed), p.gained.load(std::memory_order_relaxed), p.given.load(std::memory_order_relaxed),
            p.speed.load(std::memory_order_relaxed), p.steps.load(std::memory_order_relaxed)};
}
void set_revert_boost(std::uintptr_t client, std::uintptr_t entity, float strength, const RevertTuning &tuning) noexcept {
    auto& r = revert_boost();
    AcquireSRWLockExclusive(&r.report_lock);
    r.tuning = tuning;
    ReleaseSRWLockExclusive(&r.report_lock);
    r.client = client;
    r.entity = entity;
    r.strength.store(std::isfinite(strength) && strength > 0 ? std::min(strength, 1000.0f) : 0.0f, std::memory_order_relaxed);
    r.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
void set_transition_assist(std::uintptr_t client, std::uintptr_t entity, const TransitionAssist &assist) noexcept {
    auto& r = transition_run();
    r.client = client;
    r.entity = entity;
    AcquireSRWLockExclusive(&r.lock);
    r.assist = assist;
    ReleaseSRWLockExclusive(&r.lock);
    r.expires.store(GetTickCount64() + 500, std::memory_order_release);
}
TransitionReport transition_report() noexcept {
    auto& r = transition_run();
    AcquireSRWLockShared(&r.lock);
    const auto report = r.report;
    ReleaseSRWLockShared(&r.lock);
    return report;
}
RevertBoostReport revert_boost_report() noexcept {
    auto& r = revert_boost();
    AcquireSRWLockShared(&r.report_lock);
    const auto report = r.report;
    ReleaseSRWLockShared(&r.report_lock);
    return report;
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
