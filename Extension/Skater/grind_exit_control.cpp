#include "grind_exit_control.h"
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
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace dingosdk::grind_exit_control {
namespace {
namespace sstate = addr::skater_state;
namespace gx = addr::grind_exit;

constexpr std::uint8_t press_level = 102, release_level = 51; // of 255: about 40 % in, 20 % out
constexpr double fresh_seconds = 0.1;                         // a grind step this recent means "grinding now"
constexpr double release_grace_seconds = 0.1;                 // a trigger let go this shortly before the pop still counts
constexpr double axes_fresh_seconds = 0.25;                   // the skater's facing, sampled on the client tick
constexpr std::size_t rail_steps = 6;                         // grind steps the rail's direction is averaged over
constexpr float min_rail_speed = 0.5f, min_kept_side_speed = 1.0f;
constexpr float min_toe_across = 0.35f; // below this the toe points along the rail (slides): left/right of travel instead

enum class Stance : int { unset, regular, goofy };
enum class Sides : int { toeheel, travel };
enum class Basis : std::uint8_t { travel, toeheel, slide, no_stance, no_facing };
enum class Reason : std::uint8_t { applied, kept, no_trigger, both_triggers, slow, not_ours };
const char* reason_text(Reason r) {
    switch (r) {
    case Reason::applied: return "side set";
    case Reason::kept: return "game already chose that side";
    case Reason::no_trigger: return "no trigger: the game's own exit";
    case Reason::both_triggers: return "both triggers: the game's own exit";
    case Reason::slow: return "skipped: too slow to tell the rail's direction";
    case Reason::not_ours: return "skipped: the launch did not match the local skater's grind";
    }
    return "";
}
const char* basis_text(Basis b) {
    switch (b) {
    case Basis::travel: return "left/right of travel";
    case Basis::toeheel: return "toe/heel";
    case Basis::slide: return "toe/heel along the rail (slide): left/right of travel";
    case Basis::no_stance: return "stance not set: left/right of travel";
    case Basis::no_facing: return "facing unknown: left/right of travel";
    }
    return "";
}
struct Decision {
    std::int64_t qpc{};
    Reason reason{};
    Basis basis{};
    int trigger{};            // +1 LT, -1 RT, 0 none
    int toward{};             // +1 the tester's left of travel, -1 right
    bool switch_stance{}, lean_blocked{};
    std::uint32_t state{};
    float along{}, before_side{}, after_side{}, up{}, rail_speed{}, toe_across{};
};
struct Trigger {
    bool pressed{}, claimed{};
    std::int64_t pressed_at{}, released{};
};

struct Control {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{}, enabled{}, guard{true}, lean_guard{true}, hooked{};
    std::atomic<float> side_speed{gx::measured_side_speed}, prewindow{0.4f};
    std::atomic<int> stance{static_cast<int>(Stance::unset)}, sides{static_cast<int>(Sides::toeheel)};
    std::atomic<bool> wanted{}; // the player's on/off choice, kept; `enabled` is whether it is installed now
    // Physics thread -> the launch hook and the pad filter.
    std::atomic<std::uint32_t> state{};
    std::atomic<std::int64_t> step_qpc{}, grind_start{};
    std::atomic<float> rail_x{}, rail_z{}, rail_speed{};
    std::atomic<bool> switch_stance{};
    // Client tick -> the launch hook: the skater root's right axis, flattened.
    std::atomic<float> right_x{}, right_z{};
    std::atomic<std::int64_t> axes_qpc{};
    std::uint64_t next_axes_try{};
    // Physics thread only.
    std::array<std::array<float, 2>, rail_steps> rail{};
    std::size_t rail_head{}, rail_count{};
    std::uint32_t last_state{};
    // The game's input thread(s) and the launch hook.
    SRWLOCK trigger_lock = SRWLOCK_INIT;
    std::array<std::array<Trigger, 2>, 4> triggers{}; // [XInput slot][0 LT, 1 RT]
    std::atomic<std::int64_t> lean_masked{};          // last time the lean guard hid a pushed stick
    // Physics thread -> client tick.
    SRWLOCK decision_lock = SRWLOCK_INIT;
    std::array<Decision, 32> decisions{};
    std::size_t decision_count{};
    std::atomic<std::uint64_t> pops{}, applied{};
    std::filesystem::path settings_path;
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
std::int64_t ticks(double s) noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<std::int64_t>(s * frequency);
}
bool grind_state(std::uint32_t state) noexcept { return state >= gx::grind_states_first && state < gx::grind_states_end; }
bool grinding_now(const Control& c, std::int64_t now) noexcept {
    return grind_state(c.state.load(std::memory_order_acquire)) &&
           seconds(now - c.step_qpc.load(std::memory_order_acquire)) < fresh_seconds;
}

// ---- settings ------------------------------------------------------------------------------
std::filesystem::path settings_file() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"grindexit" / L"control.txt";
    if (raw) CoTaskMemFree(raw);
    return result;
}
void load_settings(Control& c) {
    if (c.settings_path.empty()) return;
    std::ifstream in(c.settings_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line == "stance=goofy") c.stance.store(static_cast<int>(Stance::goofy));
        else if (line == "stance=regular") c.stance.store(static_cast<int>(Stance::regular));
        else if (line == "sides=travel") c.sides.store(static_cast<int>(Sides::travel));
        else if (line == "sides=toeheel") c.sides.store(static_cast<int>(Sides::toeheel));
        else if (line == "enabled=1") c.wanted.store(true);
        else if (line == "enabled=0") c.wanted.store(false);
    }
}
void save_settings(Control& c) {
    if (c.settings_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(c.settings_path.parent_path(), error);
    std::ofstream out(c.settings_path, std::ios::trunc);
    const auto stance = static_cast<Stance>(c.stance.load());
    if (stance != Stance::unset) out << "stance=" << (stance == Stance::goofy ? "goofy" : "regular") << "\n";
    out << "sides=" << (static_cast<Sides>(c.sides.load()) == Sides::travel ? "travel" : "toeheel") << "\n";
    out << "enabled=" << (c.wanted.load() ? 1 : 0) << "\n";
}

// ---- physics side --------------------------------------------------------------------------
// The board's root body velocity (the same checks as Hall of Meat's read).
bool board_velocity(const NoBailSkater& skater, std::array<float, 3>& velocity) noexcept {
    std::uintptr_t holder{}, physics{}, type{}, bodies{}, owner{};
    std::uint32_t count{};
    if (!memory::peek(skater.core + sstate::board_holder_offset, holder) ||
        !memory::peek(holder + sstate::board_physics_offset, physics) || !memory::peek(physics, type) ||
        type != skater.base + sstate::board_physics_vtable || !memory::peek(physics + sstate::physics_bodies_offset, bodies) ||
        !memory::peek(bodies, count) || count != sstate::board_body_count)
        return false;
    const auto body = bodies + sstate::board_root_body * sstate::physics_body_size;
    if (!memory::peek(body + sstate::body_owner_offset, owner) || owner != physics ||
        !memory::peek(body + sstate::body_velocity_offset, velocity))
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
        c.switch_stance.store((flags & gx::stance_switch_mask) != 0, std::memory_order_relaxed);
    if (grind_state(state)) {
        if (!grind_state(c.last_state)) c.grind_start.store(now, std::memory_order_release);
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
    c.last_state = state;
    c.step_qpc.store(now, std::memory_order_release);
    c.state.store(state, std::memory_order_release);
}

// ---- input side ----------------------------------------------------------------------------
// Any thread the game polls its pad from.
void filter_pad(unsigned user, overlay::GamepadValues& pad) noexcept {
    auto& c = control();
    if (!c.enabled.load(std::memory_order_acquire)) return;
    const auto now = qpc_now();
    const bool grinding = grinding_now(c, now);
    const auto window_start = c.grind_start.load(std::memory_order_acquire) - ticks(c.prewindow.load(std::memory_order_relaxed));
    const bool guard = c.guard.load(std::memory_order_acquire);
    std::uint8_t* values[2]{&pad.left_trigger, &pad.right_trigger};
    bool any_claimed = false;
    AcquireSRWLockExclusive(&c.trigger_lock);
    for (std::size_t side = 0; side < 2; ++side) {
        auto& t = c.triggers[user & 3][side];
        const auto raw = *values[side];
        if (!t.pressed && raw >= press_level) {
            t.pressed = true;
            t.pressed_at = now;
            if (grinding) t.claimed = true;
        } else if (t.pressed && raw <= release_level) {
            t.pressed = false;
            if (t.claimed) {
                t.claimed = false;
                t.released = now;
            }
        }
        // Pressed a moment before the grind connected: it counts too.
        if (grinding && t.pressed && !t.claimed && t.pressed_at >= window_start) t.claimed = true;
        if (t.claimed && guard) *values[side] = 0;
    }
    for (const auto& slot : c.triggers)
        for (const auto& t : slot) any_claimed = any_claimed || t.claimed;
    ReleaseSRWLockExclusive(&c.trigger_lock);
    if (grinding && any_claimed && c.lean_guard.load(std::memory_order_acquire)) {
        if (pad.left_x > 8000 || pad.left_x < -8000) c.lean_masked.store(now, std::memory_order_relaxed);
        pad.left_x = 0;
    }
}
// +1 LT, -1 RT, 0: none, or both.
int chosen_trigger(Control& c, std::int64_t now, bool& both) noexcept {
    bool held[2]{}, recent[2]{};
    AcquireSRWLockShared(&c.trigger_lock);
    for (const auto& slot : c.triggers)
        for (std::size_t side = 0; side < 2; ++side) {
            held[side] = held[side] || slot[side].claimed;
            recent[side] = recent[side] || (slot[side].released && seconds(now - slot[side].released) < release_grace_seconds);
        }
    ReleaseSRWLockShared(&c.trigger_lock);
    both = false;
    if (held[0] && held[1]) { both = true; return 0; }
    if (held[0] || held[1]) return held[0] ? 1 : -1;
    if (recent[0] && recent[1]) { both = true; return 0; }
    if (recent[0] || recent[1]) return recent[0] ? 1 : -1;
    return 0;
}
void record(Control& c, const Decision& d) noexcept {
    AcquireSRWLockExclusive(&c.decision_lock);
    if (c.decision_count < c.decisions.size()) c.decisions[c.decision_count++] = d;
    ReleaseSRWLockExclusive(&c.decision_lock);
}

// ---- the launch ----------------------------------------------------------------------------
// Which way across the rail (+1 the tester's "left" = n = (tz, -tx), the skater root's right axis
// when riding forward; -1 the other) the claimed trigger points.
int toward_side(Control& c, int trigger, float nx, float nz, std::int64_t now, Decision& d) noexcept {
    d.switch_stance = c.switch_stance.load(std::memory_order_relaxed);
    if (static_cast<Sides>(c.sides.load()) == Sides::travel) { d.basis = Basis::travel; return trigger; }
    const auto stance = static_cast<Stance>(c.stance.load());
    if (stance == Stance::unset) { d.basis = Basis::no_stance; return trigger; }
    if (seconds(now - c.axes_qpc.load(std::memory_order_acquire)) > axes_fresh_seconds) { d.basis = Basis::no_facing; return trigger; }
    float rx = c.right_x.load(std::memory_order_relaxed), rz = c.right_z.load(std::memory_order_relaxed);
    const float length = std::hypot(rx, rz);
    if (length < 0.2f) { d.basis = Basis::no_facing; return trigger; }
    rx /= length;
    rz /= length;
    // The toes point to the root's right axis when the right foot leads (goofy riding normal or
    // fakie: measured), to the other side when the left foot leads (regular, or goofy switch).
    const bool right_foot_leads = (stance == Stance::goofy) != d.switch_stance;
    const float toe_x = right_foot_leads ? rx : -rx, toe_z = right_foot_leads ? rz : -rz;
    d.toe_across = toe_x * nx + toe_z * nz;
    if (std::abs(d.toe_across) < min_toe_across) { d.basis = Basis::slide; return trigger; }
    d.basis = Basis::toeheel;
    const int toe = d.toe_across > 0 ? 1 : -1;
    return trigger > 0 ? toe : -toe; // LT toe, RT heel
}
// Physics thread: every call of the jump-trajectory setter.
void modify_launch(std::uintptr_t caller_rva, std::uintptr_t, float (&v)[3]) noexcept {
    auto& c = control();
    if (!c.enabled.load(std::memory_order_acquire) || caller_rva != gx::grind_pop_call) return;
    const auto now = qpc_now();
    if (!grinding_now(c, now)) return; // no complies and pops from a manual launch from the same call
    c.pops.fetch_add(1, std::memory_order_relaxed);
    Decision d;
    d.qpc = now;
    d.state = c.state.load(std::memory_order_acquire);
    d.up = v[1];
    d.lean_blocked = seconds(now - c.lean_masked.load(std::memory_order_relaxed)) < 0.15;
    const float tx = c.rail_x.load(std::memory_order_relaxed), tz = c.rail_z.load(std::memory_order_relaxed);
    const float speed = c.rail_speed.load(std::memory_order_relaxed);
    const float nx = tz, nz = -tx;
    d.rail_speed = speed;
    d.along = v[0] * tx + v[2] * tz;
    d.before_side = d.after_side = v[0] * nx + v[2] * nz;
    if (speed < min_rail_speed) { d.reason = Reason::slow; record(c, d); return; }
    // The launch must be this skater's: mostly along its rail, at about its speed (measured 0.93x).
    const float horizontal = std::hypot(v[0], v[2]);
    if (!(d.along > 0.5f * speed && d.along < 1.6f * speed && horizontal < 2.0f * speed + 3.0f)) {
        d.reason = Reason::not_ours;
        record(c, d);
        return;
    }
    bool both{};
    d.trigger = chosen_trigger(c, now, both);
    if (d.trigger == 0) { d.reason = both ? Reason::both_triggers : Reason::no_trigger; record(c, d); return; }
    d.toward = toward_side(c, d.trigger, nx, nz, now, d);
    const float side = static_cast<float>(d.toward);
    if (d.before_side * side >= min_kept_side_speed) { d.reason = Reason::kept; record(c, d); return; }
    const float magnitude = std::abs(d.before_side) >= min_kept_side_speed ? std::abs(d.before_side)
                                                                           : c.side_speed.load(std::memory_order_relaxed);
    const float change = side * magnitude - d.before_side;
    v[0] += change * nx;
    v[2] += change * nz;
    d.after_side = side * magnitude;
    d.reason = Reason::applied;
    c.applied.fetch_add(1, std::memory_order_relaxed);
    record(c, d);
}

void clear_triggers(Control& c) noexcept {
    AcquireSRWLockExclusive(&c.trigger_lock);
    c.triggers = {};
    ReleaseSRWLockExclusive(&c.trigger_lock);
}
std::string status_text(Control& c) {
    if (!c.started.load(std::memory_order_acquire)) return "Grind exit control is unavailable for this game build.";
    const bool on = c.enabled.load(std::memory_order_acquire);
    const auto stance = static_cast<Stance>(c.stance.load());
    const bool toeheel = static_cast<Sides>(c.sides.load()) == Sides::toeheel;
    std::string text = std::format(
        "Grind exit control: {}. Sides: {}. Stance: {}. Grab guard {}, lean guard {}, pre-press window {:.2f} s, side speed {:.2f} m/s. "
        "Pops out of grinds seen: {}, sides set: {}.",
        on ? "ON" : "off", toeheel ? "LT toe / RT heel" : "LT left / RT right of travel",
        stance == Stance::goofy ? "goofy" : stance == Stance::regular ? "regular" : "NOT SET (type: grindexit stance goofy|regular)",
        c.guard.load() ? "on" : "off", c.lean_guard.load() ? "on" : "off", c.prewindow.load(), c.side_speed.load(), c.pops.load(),
        c.applied.load());
    if (on && toeheel && stance == Stance::unset) text += " Until the stance is set, LT/RT are left/right of travel.";
    if (on && !c.hooked.load()) text += " The launch hook is not ready yet (it installs once the game's code is recognised).";
    return text;
}
bool install(Control& c) {
    if (c.enabled.load(std::memory_order_acquire)) return true;
    if (!add_no_bail_step_listener(&on_step)) return false;
    c.state.store(0);
    c.step_qpc.store(0);
    c.last_state = 0;
    clear_triggers(c);
    trainer::set_trajectory_modifier(&modify_launch);
    DingoSDKOverlaySetGamepadFilter(&filter_pad);
    c.enabled.store(true, std::memory_order_release);
    logging::write(logging::Level::info, logging::Channel::skater, "Grind exit control on.");
    return true;
}
void uninstall(Control& c) {
    if (!c.enabled.exchange(false, std::memory_order_acq_rel)) return;
    DingoSDKOverlaySetGamepadFilter(nullptr);
    trainer::set_trajectory_modifier(nullptr);
    remove_no_bail_step_listener(&on_step);
    clear_triggers(c);
    logging::write(logging::Level::info, logging::Channel::skater, "Grind exit control off.");
}
} // namespace

bool available() noexcept { return control().started.load(std::memory_order_acquire); }
bool enabled() noexcept { return control().enabled.load(std::memory_order_acquire); }
bool toe_heel() noexcept { return static_cast<Sides>(control().sides.load()) == Sides::toeheel; }
int stance() noexcept { return control().stance.load(); }

bool start(std::uintptr_t image_base) noexcept {
    auto& c = control();
    if (c.started.load(std::memory_order_acquire)) return true;
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
                       "Grind exit control is unavailable: it follows the skater's physics steps through No Bail's hooks, which did not start.");
        return false;
    }
    try {
        c.settings_path = settings_file();
        load_settings(c);
    } catch (...) {}
    c.base.store(image_base, std::memory_order_release);
    c.started.store(true, std::memory_order_release);
    if (c.wanted.load() && !install(c))
        logging::write(logging::Level::warning, logging::Channel::skater, "Grind exit control could not start: no free step listener.");
    return true;
}

void on_client_tick(std::uintptr_t client) noexcept {
    auto& c = control();
    if (!c.started.load(std::memory_order_acquire) || !c.enabled.load(std::memory_order_acquire)) return;
    const auto base = c.base.load(std::memory_order_acquire);
    if (!c.hooked.load(std::memory_order_acquire) && trainer::start_trick_heights(base)) c.hooked.store(true, std::memory_order_release);
    // The skater's facing (rows: right, up, forward, position), for toe/heel.
    const auto tick = GetTickCount64();
    if (tick >= c.next_axes_try) {
        try {
            overlay::DebugModel skater;
            const auto matrix = client_source::detail::debug_skater(base, client, skater);
            if (skater.skater_position_valid && std::isfinite(matrix[0]) && std::isfinite(matrix[2])) {
                c.right_x.store(matrix[0], std::memory_order_relaxed);
                c.right_z.store(matrix[2], std::memory_order_relaxed);
                c.axes_qpc.store(qpc_now(), std::memory_order_release);
            }
        } catch (...) {
            c.next_axes_try = tick + 250; // no skater: menus, loading, a respawn
        }
    }
    std::array<Decision, 32> taken{};
    std::size_t count{};
    AcquireSRWLockExclusive(&c.decision_lock);
    count = c.decision_count;
    std::copy_n(c.decisions.begin(), count, taken.begin());
    c.decision_count = 0;
    ReleaseSRWLockExclusive(&c.decision_lock);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& d = taken[i];
        try {
            const char* trigger = d.trigger > 0 ? "LT" : d.trigger < 0 ? "RT" : "-";
            const char* toward = d.toward > 0 ? "left of travel" : d.toward < 0 ? "right of travel" : "-";
            logging::log(logging::Level::debug, logging::Channel::skater,
                         "Grind exit: {} ({} -> {}; {}; {}{}). state {}, rail speed {:.2f}, along {:.2f}, up {:.2f}, sideways {:+.2f} -> {:+.2f}, toe across {:+.2f}",
                         reason_text(d.reason), trigger, toward, d.trigger ? basis_text(d.basis) : "-",
                         d.switch_stance ? "switch" : "normal/fakie", d.lean_blocked ? ", lean held back" : "", d.state, d.rail_speed,
                         d.along, d.up, d.before_side, d.after_side, d.toe_across);
        } catch (...) {}
    }
}

std::string command(const std::vector<std::string>& words) {
    auto& c = control();
    const auto verb = words.empty() ? std::string("status") : words[0];
    const auto arg = words.size() > 1 ? words[1] : std::string{};
    const auto number = [&](float low, float high, float& out) {
        const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), out);
        return parsed.ec == std::errc{} && out >= low && out <= high;
    };
    if (verb == "status") return status_text(c);
    if (verb == "exits") {
        if (arg == "on" || arg == "off") {
            if (!c.started.load()) return status_text(c);
            c.wanted.store(arg == "on");
            try { save_settings(c); } catch (...) {}
            if (arg == "off") uninstall(c);
            else if (!install(c)) return "Grind exit control could not follow the skater (no free step listener).";
            return status_text(c);
        }
        if (arg.empty() || arg == "status") return status_text(c);
        return "Usage: grindexit exits on|off|status";
    }
    if (verb == "stance") {
        if (arg != "goofy" && arg != "regular") return "Usage: grindexit stance goofy|regular (your skater's stance in the game settings)";
        c.stance.store(static_cast<int>(arg == "goofy" ? Stance::goofy : Stance::regular));
        try { save_settings(c); } catch (...) {}
        return status_text(c);
    }
    if (verb == "sides") {
        if (arg != "toeheel" && arg != "travel") return "Usage: grindexit sides toeheel|travel";
        c.sides.store(static_cast<int>(arg == "travel" ? Sides::travel : Sides::toeheel));
        try { save_settings(c); } catch (...) {}
        return status_text(c);
    }
    if (verb == "guard" || verb == "leanguard") {
        if (arg != "on" && arg != "off") return "Usage: grindexit " + verb + " on|off";
        (verb == "guard" ? c.guard : c.lean_guard).store(arg == "on");
        if (verb == "guard" && arg == "off") clear_triggers(c);
        return status_text(c);
    }
    if (verb == "prewindow") {
        float value{};
        if (!number(0.0f, 1.0f, value)) return "Usage: grindexit prewindow <0 to 1> (seconds a trigger may be pressed before the grind connects; 0.4)";
        c.prewindow.store(value);
        return status_text(c);
    }
    if (verb == "sidespeed") {
        float value{};
        if (!number(0.5f, 4.0f, value)) return "Usage: grindexit sidespeed <0.5 to 4> (metres per second; the game's own is 1.8)";
        c.side_speed.store(value);
        return status_text(c);
    }
    return "Usage: grindexit exits on|off|status, stance goofy|regular, sides toeheel|travel, guard on|off, leanguard on|off, prewindow <s>, sidespeed <m/s>";
}
} // namespace dingosdk::grind_exit_control
