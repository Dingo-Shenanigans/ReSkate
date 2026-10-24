#include "grind_exit.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/grind_exit.h"
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

namespace dingosdk::grind_exit {
namespace {
namespace sstate = addr::skater_state;
namespace gx = addr::grind_exit;

constexpr double fresh_seconds = 0.1;       // a grind step this recent means "grinding now"
constexpr double axes_fresh_seconds = 0.25; // the skater's facing, sampled on the client tick
constexpr std::size_t rail_steps = 6;       // grind steps the rail's direction is averaged over
constexpr float min_rail_speed = 0.5f, min_kept_side_speed = 1.0f;
constexpr float min_toe_across = 0.35f; // below this the toe points along the rail (slides): left/right of travel instead
constexpr std::size_t most_pops = 32;

struct Control {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{}, listening{}, modifying{}, hooked{};
    // Physics thread -> the launch hook and the pad filter.
    std::atomic<std::uint32_t> state{};
    std::atomic<std::int64_t> step_qpc{};
    std::atomic<float> rail_x{}, rail_z{}, rail_speed{};
    std::atomic<bool> left_foot_leads{};
    // Client tick -> the launch hook: the skater root's right axis, flattened.
    std::atomic<float> right_x{}, right_z{};
    std::atomic<std::int64_t> axes_qpc{};
    std::uint64_t next_axes_try{};
    // Physics thread only.
    std::array<std::array<float, 2>, rail_steps> rail{};
    std::size_t rail_head{}, rail_count{};
    // The plugin's wish (client thread -> the launch hook), and the pad's hidden controls.
    std::mutex mutex;
    std::string wisher;
    std::optional<Wish> wanted;
    std::map<std::string, Hidden, std::less<>> hidden;
    std::atomic<bool> hide_lt{}, hide_rt{}, hide_left_x{}, hide_right_stick{}, hide_l3{}, hide_r3{};
    std::vector<Pop> pops; // under mutex
    std::atomic<float> side_speed{gx::measured_side_speed};
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
bool grind_state(std::uint32_t state) noexcept { return state >= gx::grind_states_first && state < gx::grind_states_end; }
bool grinding_now(const Control& c, std::int64_t now) noexcept {
    return grind_state(c.state.load(std::memory_order_acquire)) && seconds(now - c.step_qpc.load(std::memory_order_acquire)) < fresh_seconds;
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
    if (memory::peek(skater.context + gx::stance_flags_offset, flags))
        c.left_foot_leads.store((flags & gx::left_foot_leads_mask) != 0, std::memory_order_relaxed);
    if (grind_state(state)) {
        std::array<float, 3> v{};
        if (board_velocity(skater, v)) {
            c.rail[c.rail_head] = {v[0], v[2]};
            c.rail_head = (c.rail_head + 1) % rail_steps;
            c.rail_count = std::min(c.rail_count + 1, rail_steps);
            float x{}, z{};
            for (std::size_t i = 0; i < c.rail_count; ++i) {
                x += c.rail[i][0];
                z += c.rail[i][1];
            }
            x /= static_cast<float>(c.rail_count);
            z /= static_cast<float>(c.rail_count);
            const float speed = std::hypot(x, z);
            if (speed > 0.0f) {
                c.rail_x.store(x / speed, std::memory_order_relaxed);
                c.rail_z.store(z / speed, std::memory_order_relaxed);
            }
            c.rail_speed.store(speed, std::memory_order_relaxed);
        }
    } else {
        c.rail_count = c.rail_head = 0;
    }
    c.step_qpc.store(now, std::memory_order_release);
    c.state.store(state, std::memory_order_release);
}

// ---- input side ----------------------------------------------------------------------------
// Any thread the game polls its pad from.
void filter_pad(unsigned, overlay::GamepadValues& pad) noexcept {
    auto& c = control();
    if (c.hide_lt.load(std::memory_order_acquire)) pad.left_trigger = 0;
    if (c.hide_rt.load(std::memory_order_acquire)) pad.right_trigger = 0;
    if (c.hide_left_x.load(std::memory_order_acquire) && grinding_now(c, qpc_now())) pad.left_x = 0;
    if (c.hide_right_stick.load(std::memory_order_acquire)) pad.right_x = pad.right_y = 0;
    if (c.hide_l3.load(std::memory_order_acquire)) pad.buttons &= static_cast<std::uint16_t>(~0x0040u); // XINPUT_GAMEPAD_LEFT_THUMB
    if (c.hide_r3.load(std::memory_order_acquire)) pad.buttons &= static_cast<std::uint16_t>(~0x0080u); // XINPUT_GAMEPAD_RIGHT_THUMB
}
void publish_hidden(Control& c) {
    Hidden all;
    for (const auto& [owner, h] : c.hidden) {
        all.left_trigger = all.left_trigger || h.left_trigger;
        all.right_trigger = all.right_trigger || h.right_trigger;
        all.left_x_grinding = all.left_x_grinding || h.left_x_grinding;
        all.right_stick = all.right_stick || h.right_stick;
        all.left_stick_click = all.left_stick_click || h.left_stick_click;
        all.right_stick_click = all.right_stick_click || h.right_stick_click;
    }
    c.hide_lt.store(all.left_trigger, std::memory_order_release);
    c.hide_rt.store(all.right_trigger, std::memory_order_release);
    c.hide_left_x.store(all.left_x_grinding, std::memory_order_release);
    c.hide_right_stick.store(all.right_stick, std::memory_order_release);
    c.hide_l3.store(all.left_stick_click, std::memory_order_release);
    c.hide_r3.store(all.right_stick_click, std::memory_order_release);
    DingoSDKOverlaySetGamepadFilter(c.hidden.empty() ? nullptr : &filter_pad);
}

// ---- the launch ----------------------------------------------------------------------------
// Which way across the rail (+1 the left of travel, n = (tz, -tx); -1 the right) the wish points.
int toward_side(Control& c, const Wish& w, float nx, float nz, std::int64_t now, Pop& pop) noexcept {
    using Side = Wish::Side;
    const int fallback = w.side == Side::toe || w.side == Side::left ? 1 : -1;
    if (w.side == Side::left || w.side == Side::right) { pop.basis = "travel"; return fallback; }
    if (w.stance != 1 && w.stance != 2) { pop.basis = "no_stance"; return fallback; }
    if (seconds(now - c.axes_qpc.load(std::memory_order_acquire)) > axes_fresh_seconds) { pop.basis = "no_facing"; return fallback; }
    float rx = c.right_x.load(std::memory_order_relaxed), rz = c.right_z.load(std::memory_order_relaxed);
    const float length = std::hypot(rx, rz);
    if (length < 0.2f) { pop.basis = "no_facing"; return fallback; }
    rx /= length;
    rz /= length;
    // The toes point to the root's right axis when the right foot leads (goofy riding normal or
    // fakie: measured), to the other side when the left foot does (regular, or goofy switch).
    // The measured flag already names the leading foot, independent of the
    // rider's base stance. Reinterpreting it as switch flips regular riders.
    const bool right_foot_leads = !c.left_foot_leads.load(std::memory_order_relaxed);
    const float toe_x = right_foot_leads ? rx : -rx, toe_z = right_foot_leads ? rz : -rz;
    const float across = toe_x * nx + toe_z * nz;
    if (std::abs(across) < min_toe_across) { pop.basis = "slide"; return fallback; }
    pop.basis = "toe_heel";
    const int toe = across > 0 ? 1 : -1;
    return w.side == Side::toe ? toe : -toe;
}
void record(Control& c, Pop pop) {
    std::lock_guard lock(c.mutex);
    if (c.pops.size() < most_pops) c.pops.push_back(std::move(pop));
}
// Physics thread: every call of the jump-trajectory setter.
void modify_launch(std::uintptr_t caller_rva, std::uintptr_t, float (&v)[3]) noexcept {
    auto& c = control();
    if (caller_rva != gx::grind_pop_call) return;
    const auto now = qpc_now();
    if (!grinding_now(c, now)) return; // no complies and pops from a manual launch from the same call
    try {
        std::optional<Wish> wish;
        {
            std::lock_guard lock(c.mutex);
            if (c.wisher.empty()) return;
            wish = c.wanted;
        }
        Pop pop;
        const float tx = c.rail_x.load(std::memory_order_relaxed), tz = c.rail_z.load(std::memory_order_relaxed);
        const float speed = c.rail_speed.load(std::memory_order_relaxed);
        const float nx = tz, nz = -tx;
        const float along = v[0] * tx + v[2] * tz;
        pop.before = pop.after = v[0] * nx + v[2] * nz;
        if (!wish) { pop.reason = "none"; return record(c, std::move(pop)); }
        if (speed < min_rail_speed) { pop.reason = "slow"; return record(c, std::move(pop)); }
        // The launch must be this skater's: mostly along its rail, at about its speed (measured 0.93x).
        const float horizontal = std::hypot(v[0], v[2]);
        if (!(along > 0.5f * speed && along < 1.6f * speed && horizontal < 2.0f * speed + 3.0f)) {
            pop.reason = "not_ours";
            return record(c, std::move(pop));
        }
        pop.toward = toward_side(c, *wish, nx, nz, now, pop);
        const float side = static_cast<float>(pop.toward);
        // Strength scales the sideways speed; at full strength the game's own exit toward the chosen
        // side is kept as it is.
        const float strength = std::clamp(wish->strength, 0.1f, 1.0f);
        if (strength >= 1.0f && pop.before * side >= min_kept_side_speed) { pop.reason = "kept"; return record(c, std::move(pop)); }
        const float full = std::abs(pop.before) >= min_kept_side_speed ? std::abs(pop.before) : c.side_speed.load(std::memory_order_relaxed);
        const float magnitude = full * strength;
        const float change = side * magnitude - pop.before;
        v[0] += change * nx;
        v[2] += change * nz;
        pop.after = side * magnitude;
        pop.applied = true;
        pop.reason = "applied";
        record(c, std::move(pop));
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
                       "Grind exits for plugins are unavailable: they follow the skater's physics steps through No Bail's hooks, which did not start.");
        return false;
    }
    c.base.store(image_base, std::memory_order_release);
    c.started.store(true, std::memory_order_release);
    return true;
}
bool available() noexcept { return control().started.load(std::memory_order_acquire); }

std::string wish(std::string_view owner, std::optional<Wish> w) {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return "grind exits are unavailable for this game build";
    std::lock_guard lock(c.mutex);
    if (!c.wisher.empty() && c.wisher != owner) return c.wisher + " picks the grind exits";
    c.wisher = owner;
    c.wanted = w;
    listen(c);
    return {};
}
void hide(std::string_view owner, Hidden hidden) {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire)) return;
    std::lock_guard lock(c.mutex);
    if (hidden.any()) c.hidden.insert_or_assign(std::string(owner), hidden);
    else if (const auto found = c.hidden.find(owner); found != c.hidden.end()) c.hidden.erase(found);
    if (hidden.left_x_grinding) listen(c);
    publish_hidden(c);
}
void release(std::string_view owner) {
    auto& c = control();
    std::lock_guard lock(c.mutex);
    if (c.wisher == owner) {
        c.wisher.clear();
        c.wanted.reset();
        c.pops.clear();
    }
    if (const auto found = c.hidden.find(owner); found != c.hidden.end()) {
        c.hidden.erase(found);
        publish_hidden(c);
    }
}
std::vector<Pop> take_pops(std::string_view owner) {
    auto& c = control();
    std::lock_guard lock(c.mutex);
    if (c.wisher != owner) return {};
    return std::exchange(c.pops, {});
}
bool grinding() noexcept { return grinding_now(control(), qpc_now()); }

void tick(std::uintptr_t client) noexcept {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire) || !c.modifying.load(std::memory_order_acquire)) return;
    const auto base = c.base.load(std::memory_order_acquire);
    if (!c.hooked.load(std::memory_order_acquire) && trainer::start_trick_heights(base)) c.hooked.store(true, std::memory_order_release);
    // The skater's facing (rows: right, up, forward, position), for toe and heel.
    const auto now = GetTickCount64();
    if (now < c.next_axes_try) return;
    try {
        overlay::DebugModel skater;
        const auto matrix = client_source::detail::debug_skater(base, client, skater);
        if (skater.skater_position_valid && std::isfinite(matrix[0]) && std::isfinite(matrix[2])) {
            c.right_x.store(matrix[0], std::memory_order_relaxed);
            c.right_z.store(matrix[2], std::memory_order_relaxed);
            c.axes_qpc.store(qpc_now(), std::memory_order_release);
        }
    } catch (...) {
        c.next_axes_try = now + 250; // no skater: menus, loading, a respawn
    }
}
}
