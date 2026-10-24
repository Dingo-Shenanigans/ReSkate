#include "launches.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/launches.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Trainer/trainer_jump.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

namespace dingosdk::launches {
namespace {
namespace sstate = addr::skater_state;
namespace gl = addr::launches;

constexpr double fresh_seconds = 0.1;       // a physics step this recent means "the state now"
constexpr double axes_fresh_seconds = 0.25; // the skater's facing, sampled on the client tick
constexpr std::size_t travel_steps = 6;     // physics steps the direction of travel is averaged over
constexpr float min_travel_speed = 0.5f;
constexpr std::size_t most_launches = 32;
constexpr std::size_t kinds = 5;

struct Control {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{}, listening{}, modifying{}, hooked{};
    // Physics thread -> the launch hook.
    std::atomic<std::uint32_t> state{};
    std::atomic<std::int64_t> step_qpc{};
    std::atomic<float> travel_x{}, travel_z{}, travel_speed{};
    std::atomic<bool> switch_stance{};
    // Client tick -> reskate.skater.get: the skater root's right axis.
    std::atomic<float> right_x{}, right_y{}, right_z{};
    std::atomic<std::int64_t> axes_qpc{};
    std::atomic<bool> facing_asked{};
    std::uint64_t next_axes_try{};
    // Physics thread only.
    std::array<std::array<float, 2>, travel_steps> travel{};
    std::size_t travel_head{}, travel_count{};
    // The plugins' shapes by kind (client thread -> the launch hook), and the launches seen.
    std::mutex mutex;
    std::array<std::string, kinds> shaper;
    std::array<std::optional<Shape>, kinds> shapes;
    std::vector<Launch> launches; // under mutex
};
Control& control() {
    static auto* value = new Control; // process lifetime: hooks may call in until exit
    return *value;
}
std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double seconds(std::int64_t ticks) noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
bool grinding_now(const Control& c, std::int64_t now) noexcept {
    return gl::is_grind_state(c.state.load(std::memory_order_acquire)) && seconds(now - c.step_qpc.load(std::memory_order_acquire)) < fresh_seconds;
}

// ---- physics side --------------------------------------------------------------------------
// The board's root body velocity (as skater_events.cpp reads it).
bool board_velocity(const NoBailSkater& skater, std::array<float, 3>& velocity) noexcept {
    std::uintptr_t holder{}, physics{}, type{}, bodies{}, owner{};
    std::uint32_t count{};
    if (!memory::peek(skater.core + sstate::board_holder_offset, holder) || !memory::peek(holder + sstate::board_physics_offset, physics) ||
        !memory::peek(physics, type) || type != skater.base + sstate::board_physics_vtable ||
        !memory::peek(physics + sstate::physics_bodies_offset, bodies) || !memory::peek(bodies, count) || count != sstate::board_body_count)
        return false;
    const auto body = bodies + sstate::board_root_body * sstate::physics_body_size;
    if (!memory::peek(body + sstate::body_owner_offset, owner) || owner != physics || !memory::peek(body + sstate::body_velocity_offset, velocity))
        return false;
    return std::all_of(velocity.begin(), velocity.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1000; });
}
// Physics thread, every step of the local skater.
void on_step(const NoBailSkater& skater, float, bool) noexcept {
    auto& c = control();
    std::uint32_t state{};
    if (!memory::peek(skater.context + sstate::physics_state_offset, state)) return;
    const auto now = qpc_now();
    std::uint8_t flags{};
    if (memory::peek(skater.context + gl::stance_flags_offset, flags))
        c.switch_stance.store((flags & gl::stance_switch_mask) != 0, std::memory_order_relaxed);
    // The direction of travel: the board's horizontal velocity over the last steps on the board.
    std::array<float, 3> v{};
    if (state != gl::pop_state && board_velocity(skater, v)) {
        c.travel[c.travel_head] = {v[0], v[2]};
        c.travel_head = (c.travel_head + 1) % travel_steps;
        c.travel_count = std::min(c.travel_count + 1, travel_steps);
        float x{}, z{};
        for (std::size_t i = 0; i < c.travel_count; ++i) {
            x += c.travel[i][0];
            z += c.travel[i][1];
        }
        x /= static_cast<float>(c.travel_count);
        z /= static_cast<float>(c.travel_count);
        const float speed = std::hypot(x, z);
        if (speed > 0.0f) {
            c.travel_x.store(x / speed, std::memory_order_relaxed);
            c.travel_z.store(z / speed, std::memory_order_relaxed);
        }
        c.travel_speed.store(speed, std::memory_order_relaxed);
    }
    c.step_qpc.store(now, std::memory_order_release);
    c.state.store(state, std::memory_order_release);
}

// ---- the launch ----------------------------------------------------------------------------
From classify(Control& c, std::uintptr_t caller_rva, std::int64_t now) noexcept {
    if (caller_rva == gl::ollie_call) return From::ollie;
    if (caller_rva == gl::boneless_call) return From::boneless;
    // The grind pop's call is also the no comply's (from the crouch).
    if (caller_rva == gl::grind_pop_call) return grinding_now(c, now) ? From::grind : From::no_comply;
    return From::other;
}
void record(Control& c, Launch launch) {
    std::lock_guard lock(c.mutex);
    if (c.launches.size() < most_launches) c.launches.push_back(std::move(launch));
}
// Physics thread: every call of the jump-trajectory setter.
void modify_launch(std::uintptr_t caller_rva, std::uintptr_t, float (&v)[3]) noexcept {
    auto& c = control();
    try {
        Launch launch;
        launch.from = classify(c, caller_rva, qpc_now());
        launch.game = launch.velocity = {v[0], v[1], v[2]};
        const float tx = c.travel_x.load(std::memory_order_relaxed), tz = c.travel_z.load(std::memory_order_relaxed);
        launch.travel_speed = c.travel_speed.load(std::memory_order_relaxed);
        if (launch.travel_speed >= min_travel_speed) launch.travel = {tx, tz};
        std::optional<Shape> shape;
        {
            std::lock_guard lock(c.mutex);
            const auto kind = static_cast<std::size_t>(launch.from);
            shape = c.shapes[kind];
            if (shape) launch.by = c.shaper[kind];
        }
        if (!shape) {
            launch.reason = "none";
            return record(c, std::move(launch));
        }
        if (shape->up) v[1] = *shape->up;
        if (shape->across || shape->along) {
            if (launch.travel_speed < min_travel_speed) {
                launch.reason = "slow";
                launch.by.clear();
                return record(c, std::move(launch));
            }
            // A launch along its travel, at about its speed (out of a grind, measured 0.93x): a
            // launch of something else through the same call is left alone.
            const float along = v[0] * tx + v[2] * tz;
            const float horizontal = std::hypot(v[0], v[2]);
            if (!(along > 0.5f * launch.travel_speed && along < 1.6f * launch.travel_speed && horizontal < 2.0f * launch.travel_speed + 3.0f)) {
                launch.reason = "not_ours";
                launch.by.clear();
                return record(c, std::move(launch));
            }
            const float nx = tz, nz = -tx; // the left of travel
            const float across_now = v[0] * nx + v[2] * nz;
            const float new_along = shape->along.value_or(along), new_across = shape->across.value_or(across_now);
            v[0] = new_along * tx + new_across * nx;
            v[2] = new_along * tz + new_across * nz;
        }
        launch.velocity = {v[0], v[1], v[2]};
        launch.reason = "applied";
        record(c, std::move(launch));
    } catch (...) {
    }
}

// The step listener and the launch hook, from the first use on.
void listen(Control& c) {
    if (!c.listening.load(std::memory_order_acquire) && add_no_bail_step_listener(&on_step)) c.listening.store(true, std::memory_order_release);
    if (!c.modifying.exchange(true)) trainer::set_trajectory_modifier(&modify_launch);
}
} // namespace

bool start(std::uintptr_t image_base) noexcept {
    auto& c = control();
    if (c.started.load(std::memory_order_acquire)) return true;
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
                       "Launches for plugins are unavailable: they follow the skater's physics steps through No Bail's hooks, which did not start.");
        return false;
    }
    c.base.store(image_base, std::memory_order_release);
    c.started.store(true, std::memory_order_release);
    return true;
}
bool available() noexcept { return control().started.load(std::memory_order_acquire); }

const char* name(From from) noexcept {
    switch (from) {
    case From::grind: return "grind";
    case From::ollie: return "ollie";
    case From::no_comply: return "no_comply";
    case From::boneless: return "boneless";
    default: return "other";
    }
}
std::optional<From> from_name(std::string_view name) noexcept {
    for (const auto from : {From::grind, From::ollie, From::no_comply, From::boneless})
        if (name == launches::name(from)) return from;
    return std::nullopt;
}

std::string shape(std::string_view owner, From from, std::optional<Shape> s) {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return "launches are unavailable for this game build";
    std::lock_guard lock(c.mutex);
    const auto kind = static_cast<std::size_t>(from);
    if (!c.shaper[kind].empty() && c.shaper[kind] != owner) return c.shaper[kind] + " shapes " + name(from) + " launches";
    c.shaper[kind] = s ? std::string(owner) : std::string{};
    c.shapes[kind] = s;
    listen(c);
    return {};
}
std::vector<Launch> take() {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(c.mutex);
    listen(c);
    return std::exchange(c.launches, {});
}
void release(std::string_view owner) {
    auto& c = control();
    std::lock_guard lock(c.mutex);
    for (std::size_t kind = 0; kind < kinds; ++kind)
        if (c.shaper[kind] == owner) {
            c.shaper[kind].clear();
            c.shapes[kind].reset();
        }
}
std::optional<Facing> facing() noexcept {
    auto& c = control();
    c.facing_asked.store(true, std::memory_order_relaxed);
    if (!c.started.load(std::memory_order_acquire) || seconds(qpc_now() - c.axes_qpc.load(std::memory_order_acquire)) > axes_fresh_seconds)
        return std::nullopt;
    return Facing{{c.right_x.load(std::memory_order_relaxed), c.right_y.load(std::memory_order_relaxed), c.right_z.load(std::memory_order_relaxed)},
                  c.switch_stance.load(std::memory_order_relaxed)};
}
bool grinding() noexcept {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return false;
    if (!c.listening.load(std::memory_order_acquire)) {
        std::lock_guard lock(c.mutex);
        listen(c);
    }
    return grinding_now(c, qpc_now());
}

void tick(std::uintptr_t client) noexcept {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return;
    const auto base = c.base.load(std::memory_order_acquire);
    if (c.modifying.load(std::memory_order_acquire) && !c.hooked.load(std::memory_order_acquire) && trainer::start_trick_heights(base))
        c.hooked.store(true, std::memory_order_release);
    // The skater's facing (rows: right, up, forward, position), once it is asked for.
    if (!c.facing_asked.load(std::memory_order_relaxed)) return;
    const auto now = GetTickCount64();
    if (now < c.next_axes_try) return;
    try {
        overlay::DebugModel skater;
        const auto matrix = client_source::detail::debug_skater(base, client, skater);
        if (skater.skater_position_valid && std::isfinite(matrix[0]) && std::isfinite(matrix[1]) && std::isfinite(matrix[2])) {
            c.right_x.store(matrix[0], std::memory_order_relaxed);
            c.right_y.store(matrix[1], std::memory_order_relaxed);
            c.right_z.store(matrix[2], std::memory_order_relaxed);
            c.axes_qpc.store(qpc_now(), std::memory_order_release);
        }
    } catch (...) {
        c.next_axes_try = now + 250; // no skater: menus, loading, a respawn
    }
}
}
