#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "offboard_flight.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "free_flight.h"
#include "Engine/Game/Skater/first_person_cycle.h"
#include "drone_flight.h"
#include "drone_audio.h"
#include "Engine/Game/Skater/velocity_boost.h"
#include "reskate_version.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <utility>

namespace dingosdk::client_source::detail {
double source_flight_now() {
    // GetTickCount64 advances by roughly 15.6 ms even at high frame rates.
    // Keep those coarse ticks for deadlines, never for integrating motion.
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 0.0;
    }();
    LARGE_INTEGER counter{};
    source_require(frequency > 0 && QueryPerformanceCounter(&counter) && counter.QuadPart >= 0,
        "The flight timer is unavailable.");
    return static_cast<double>(counter.QuadPart) / frequency;
}
namespace {
// Free-flight speed: any speed between the slowest and fastest the console offers (0.6 to 1500),
// so the FPV drone's presets and its top speed slider can set their own (drone_flight.h).
constexpr float min_debug_speed = 0.6f, max_debug_speed = 1500.0f;
bool valid_debug_speed(float speed) { return std::isfinite(speed) && speed >= min_debug_speed && speed <= max_debug_speed; }
struct DebugUi { std::uintptr_t object{}; std::uint8_t draw{}; };
// The UI settings object last verified writable, rechecked once a second: its
// VirtualQuery otherwise ran on every debug tick. Callers hold SourceState::busy.
struct WritableUi { std::uintptr_t object{}; ULONGLONG until{}; };
bool ui_writable(std::uintptr_t object) {
    static WritableUi cache;
    const auto now = GetTickCount64();
    if (cache.object == object && now < cache.until) return true;
    if (!source_writable(object + 0x4e, 1)) return false;
    cache = {object, now + 1000};
    return true;
}
DebugUi debug_ui(std::uintptr_t base) {
    SourceReader reader;
    DebugUi result{reader.pointer(base, spawn::ui_settings)};
    source_require(source_object(result.object) && reader.pointer(result.object) == base + spawn::ui_settings_vtable &&
        reader.pointer(result.object, 8) == base + spawn::ui_settings_type, "Game UI settings are unavailable.");
    result.draw = reader.value<std::uint8_t>(result.object, 0x4e);
    source_require(result.draw <= 1 && ui_writable(result.object), "Game UI draw flag is invalid.");
    reader.verify();
    return result;
}
std::array<float, 3> debug_position(SourceReader& reader, std::uintptr_t address) {
    // The fourth SIMD lane is not a coordinate and can contain a native sentinel.
    std::array<float, 4> row{};
    source_require(reader.raw(address, row.data(), sizeof(row)), "Position is unavailable.");
    for (std::size_t i = 0; i < 3; ++i)
        source_require(std::isfinite(row[i]) && std::abs(row[i]) <= 1000000.0f, "Position exceeds diagnostic bounds.");
    return {row[0], row[1], row[2]};
}
}
// ---- Dark Pop ---------------------------------------------------------------------------
// The selector hook in no_bail.cpp arms a dark-catch flight on a D-pad press, routes its landing through the game's
// own sink-and-pop state (601) and holds off bails around it. This tick publishes the switch, its settings and the
// board body, and reports each armed landing. Nothing is read until Dark Pop is switched on.
namespace {
void dark_pop_tick(InteractiveDebug& debug, const NoclipBodies& bodies, bool ready) {
    const bool allowed = debug.dark_pop && ready && session_boosts_allowed() && !debug.noclip && !debug.park_editor;
    dark_pop_publish_bodies(allowed && !bodies.offboard ? bodies.parts.data() : nullptr, allowed && !bodies.offboard ? bodies.parts.size() : 0);
    dark_pop_update(allowed, allowed ? bodies.parts[0] : 0, debug.dark_pop_unpatch, debug.dark_pop_hold, debug.dark_pop_one_press,
        static_cast<unsigned>(std::clamp(debug.dark_pop_route_stay, 0.0f, 600.0f)),
        static_cast<unsigned>(std::clamp(debug.dark_pop_route_guard, 0.0f, 600.0f)), debug.dark_pop_keep_speed, debug.dark_pop_catch,
        debug.dark_pop_feet, debug.dark_pop_adaptive, debug.dark_pop_timeout, std::clamp(debug.dark_pop_strength, 0.0f, 30.0f), debug.dark_pop_hold_landing,
        debug.dark_pop_dark_exit, debug.dark_pop_full_flight, debug.dark_pop_hold_flight,
        static_cast<unsigned>(std::clamp(debug.dark_pop_stay_physical, 0.0f, 60.0f)), debug.dark_pop_early_physics, debug.dark_pop_reaim, debug.dark_pop_hide_dpad);
    if (!debug.dark_pop) return;
    const auto last = dark_pop_last();
    if (last.landings != debug.dark_pop_seen) {
        debug.dark_pop_seen = last.landings;
        const bool popped = last.wanted == 200;
        if (popped) ++debug.dark_pop_count;
        const char* outcome = last.wanted == 601 ? (last.caught ? "went to 601 (the sink; an early dismount, caught)" :
            last.timed_out ? "went to 601 (the sink; a timed-out landing, caught)" : "went to 601 (the sink)") : popped ? "the game popped (201 timed out, no landing stage)" :
            last.wanted >= 100 && last.wanted < 200 ? "rode away (no pop)" : last.wanted == 504 ? "dismounted (no pop)" : "bailed (no pop)";
        logging::log(logging::Level::info, logging::Channel::skater,
            "Dark Pop: dark landing (board up {:.2f}), unpatch {}, hold {}, 601 route {}, early-dismount catch {}, timed-out catch {}, landing hold {} -> state {}, {}",
            last.board_up,
            !last.unpatch ? std::string("off") : !last.unpatch_hooked ? std::string("on but not hooked") :
                std::format("B set on {} tick(s), placement calls {} (latched before {}, after {}, forced {})",
                    last.unpatch_ticks, last.place_calls, last.place_latched_before, last.place_latched_after, last.place_forced),
            !last.hold ? std::string("off") : !last.hold_hooked ? std::string("on but not hooked") :
                std::format("{} (blend at start {:.3f}, active on {} tick(s), mode-2 requests {} on bone 0x{:x})",
                    last.hold_starts ? "started" : "not started", last.hold_blend, last.hold_ticks, last.attach_requests, last.attach_bone),
            last.route ? std::format("bits set on {} tick(s)", last.route_ticks) : std::string("off"),
            last.catch_dismount ? std::format("on ({} caught so far)", last.catches) : std::string("off"),
            last.catch_timeout ? std::format("on ({} caught so far)", last.timeout_catches) : std::string("off"),
            !last.hold_landing ? std::string("off") : std::format("bit up on {} call(s) ({} staged after a hold so far)", last.hold_landing_ticks, last.hold_landing_staged),
            last.wanted, outcome);
        if (last.feet)
            logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop feet-only hold: {}",
                !last.feet_ready ? std::string("on but its hook is not in") :
                std::format("the footplant's attach requests cleared on {} tick(s), two-tick foot attach requested on {} tick(s) so far", last.feet_skips, last.feet_requests));
    }
    if (last.route_exits != debug.dark_pop_route_seen) {
        debug.dark_pop_route_seen = last.route_exits;
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop: the sink (601) lasted {} tick(s) (sink length {}, package cleared on {}, stay bit cancelled on {}) -> state {}{}",
            last.route_live_ticks, last.route_stay, last.route_clears, last.route_stay_clears,
            last.route_exit,
            last.route_let_go ? " (the window was let go: no chooser exit)" :
            last.route_exit == 200 ? ", the pop" : last.route_exit == 201 ? ", back to the landing" : last.route_exit == 300 ? ", wipeout" : "");
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop dark exit {}; launch {}; adaptive sink {}; board freeze {}; stage probe {}",
            !last.dark_exit ? std::format("off (board up at the exit {:.2f})", last.exit_up_before) :
                last.exit_written ? std::format("wrote the board back as it landed (up {:.2f} -> {:.2f})", last.exit_up_before, last.exit_up_after) :
                std::format("nothing to write (board up at the exit {:.2f})", last.exit_up_before),
            std::format("{} armed at the exit (context velocity y {} at the landing, {} at the exit; anim jump {} at {} m/s)",
                    last.pop_strength > 0.0f ? std::format("{:.1f} m/s", last.pop_strength) : std::string("off"),
                    last.strength_found > 8.0f ? std::string("unread") : std::format("{:.2f}", last.strength_found),
                    last.strength_seen > 8.0f ? std::string("unread") : std::format("{:.2f}", last.strength_seen),
                    last.jump_ticks == 0xffffffffu ? std::string("ticks unread") : std::format("{} tick(s) ago", last.jump_ticks),
                    last.jump_vy > 8.5f ? std::string("?") : std::format("{:.2f}", last.jump_vy)),
            !last.adaptive ? std::string("off") : last.adaptive_tick ? std::format("ended the sink on tick {} (the board rising)", last.adaptive_tick) : std::string("did not fire"),
            !last.feet ? std::string("off") : std::format("wrote the board's transform on {} tick(s), found held on {}, overwritten on {}", last.freeze_writes, last.freeze_held, last.freeze_lost),
            !last.mover_ready ? std::string("hook not in") : std::format("saw the mover on {} tick(s)", last.mover_held));
    }
    if (last.pop_landings != debug.dark_pop_landings_seen) {
        debug.dark_pop_landings_seen = last.pop_landings;
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop: the pop's flight lasted {:.2f} s and landed (state {}, board up {:.2f}){}; launch {}; launch record {}; full flight {}{}{}; flight hold {}; stayed physical {}; re-aim {}",
            last.pop_seconds, last.pop_landed, last.pop_board_up, last.pop_landed >= 100 && last.pop_landed < 200 ? ", riding away" : ", not riding away",
            last.pop_strength <= 0.0f ? std::string("off (the game's own)") : last.lift_found > 8.5f ? std::string("second tick unread") :
                std::format("{:.1f} m/s: second tick found {:.1f}, lifted +{:.1f} on tick 2 and +{:.1f} on tick 3 ({} bodies, {} lift tick(s))",
                    last.pop_strength, last.lift_found, last.lift_added[0], last.lift_added[1], last.lift_bodies, last.lift_ticks),
            !last.launch_state_ok ? std::string("not found (the current state object was not the 200 one on the first call)") :
                std::format("flag {}, velocity found ({:.2f}, {:.2f}, {:.2f}), {}", last.launch_flag, last.launch_found[0], last.launch_found[1], last.launch_found[2],
                    last.launch_written ? std::format("y written to {:.1f}", last.pop_strength) : last.pop_strength > 0.0f ? std::string("not written") : std::string("left to the game")),
            !last.full_flight ? std::string("off") : last.flight_total <= 0.0f ? std::string("never reached 201") :
                std::format("launch {:.1f} m/s, ballistic {:.2f} s, countdown found {}, floored on {} call(s), {}", last.flight_v0, last.flight_total,
                    last.flight_found > 8.0f ? std::string("unread") : std::format("{:.2f} s", last.flight_found), last.flight_floors,
                    last.flight_stop == 1 ? "time up" : last.flight_stop == 2 ? "let go on contact" : last.flight_stop == 4 ? "launch too weak" : "left 201 first"),
            last.route ? std::format("; guard {} tick(s), package cleared on {}; keep speed {}", last.route_guard, last.guard_clears,
                !last.keep_speed ? std::string("off") : std::format("landing {:.1f} m/s, {:.1f} m/s added back to {} bodies, re-asserted on {} guard tick(s)", last.landing_speed, last.added_speed, last.speed_bodies, last.speed_reasserts)) : std::string(),
            last.hold && last.hold_hooked ? std::format("; hold active on {} tick(s), mode-2 requests {}, releases so far {}", last.hold_ticks, last.attach_requests, last.releases) : std::string(),
            !last.hold_flight ? std::string("off") : last.hold_blocks ? std::format("cleared contacts on {} call(s) (up to {} at once)", last.hold_blocks, last.hold_contacts_max) : std::string("nothing to clear"),
            !last.stay_physical ? std::string("off") : std::format("{} tick(s), jump-fired bit hidden on {} call(s){}", last.stay_physical, last.stay_clears,
                !last.early_jump ? std::string() : last.reaim ? std::string(" (the jump fired early: re-aimed and handed to the animation)") :
                last.early_physics ? std::string(" (the jump fired early: a physics flight, landed from 200)") : std::string(" (the jump fired early; physics flight off)")),
            !last.reaim ? std::string("off") : !last.early_jump ? std::string("not needed (the jump fired late, a clean pop)") :
                last.reaim_writes ? std::format("wrote the curve on {} call(s) from tick {}: start ({:.1f}, {:.2f}, {:.1f}), velocity ({:.2f}, {:.2f}, {:.2f}) (body y {:.2f}, context y {:.2f}), {} frame(s) left, request time {:.3f} s, landed from {}",
                    last.reaim_writes, last.reaim_tick, last.reaim_pos[0], last.reaim_pos[1], last.reaim_pos[2], last.reaim_vel[0], last.reaim_vel[1], last.reaim_vel[2], last.reaim_body_vy, last.reaim_context_vy, last.reaim_frames, last.reaim_time, last.reaim_from) :
                std::format("nothing written ({})", last.reaim_skip));
        if (last.feet && last.feet_ready)
            logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop feet-only hold through the pop: attach requests cleared on {} tick(s), two-tick attaches requested on {} tick(s)", last.feet_skips, last.feet_requests);
    }
}
}
// ---- Dark pop probe -----------------------------------------------------------
// Off unless switched on. While on it writes logs\dark-pop-probe.csv beside the game: one row per tick
// with the physics state, the controller buttons, 40 words of the skater's request block (where the game
// keeps "mount", "dismount" and similar requests), 64 floats of the board's first body and the skater's
// transform. Doing a few natural dark pops (and failures) with it on shows which of those values the
// game changes at the moment a pop happens, which is what a faithful Dark Pop needs to know.
namespace {
std::ofstream& probe_file() { static auto* file = new std::ofstream; return *file; }
std::ofstream& fast_file() { static auto* file = new std::ofstream; return *file; }
std::int64_t g_fast_base = 0;
// Writes what the selector hook queued: logs\dark-pop-fast.csv, one row per physics-state choice with its time
// in microseconds, the state before, the state the game wanted, the state returned, the buttons and 18 words.
void fast_probe_flush() {
    auto& file = fast_file();
    if (!file.is_open()) return;
    static FastProbeRow rows[2048];
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0) return;
    char text[24];
    for (int pass = 0; pass < 8; ++pass) {
        const auto count = fast_probe_drain(rows, 2048);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& row = rows[i];
            if (!g_fast_base) g_fast_base = row.ticks;
            file << (row.ticks - g_fast_base) * 1000000 / frequency.QuadPart << ',' << row.current << ',' << row.wanted << ','
                 << row.chosen << ',' << row.buttons;
            for (const auto word : row.words) { std::snprintf(text, sizeof text, ",%x", word); file << text; }
            std::snprintf(text, sizeof text, ",%.3f,%u", static_cast<double>(row.board_up), row.dark_pop);
            file << text;
            for (const auto axis : row.axes) { std::snprintf(text, sizeof text, ",%.3f", static_cast<double>(axis)); file << text; }
            for (const auto extra : row.extra) { std::snprintf(text, sizeof text, ",%.4g", static_cast<double>(extra)); file << text; }
            for (const auto up : row.stage_up) { std::snprintf(text, sizeof text, ",%.3f", static_cast<double>(up)); file << text; }
            for (const auto v : row.board_vel) { std::snprintf(text, sizeof text, ",%.3f", static_cast<double>(v)); file << text; }
            std::snprintf(text, sizeof text, ",%.4g,%.4g,%x,%u,%x", static_cast<double>(row.gates[0]), static_cast<double>(row.gates[1]), row.word_13f8, row.contacts, row.phys_flags);
            file << text;
            file << '\n';
        }
        if (count < 2048) break;
    }
}
void probe_tick(InteractiveDebug& debug, const std::array<float, 16>& transform, const NoclipBodies& bodies, bool ready) {
    auto& file = probe_file();
    if (!debug.probe || !ready) {
        if (file.is_open()) file.close();
        if (fast_file().is_open()) { fast_probe_enable(false); fast_probe_flush(); fast_file().close(); }
        return;
    }
    if (!file.is_open()) {
        wchar_t module[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (!length || length >= MAX_PATH) { debug.probe = false; return; }
        // A new pair of files each time the probe is switched on, named by the time, so nothing is overwritten.
        char stamp[32]{};
        const std::time_t now_time = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now_time);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
        const auto path = std::filesystem::path(module).parent_path() / "logs" / (std::string("dark-pop-probe-") + stamp + ".csv");
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        file.open(path, std::ios::out | std::ios::trunc);
        if (!file) { debug.probe = false; return; }
        file << "ms,state,pad";
        for (int i = 0; i < 40; ++i) file << ",c" << i;
        for (int i = 0; i < 64; ++i) file << ",b" << i;
        for (int i = 0; i < 16; ++i) file << ",t" << i;
        file << "\n";
        auto& fast = fast_file();
        fast.open(path.parent_path() / (std::string("dark-pop-fast-") + stamp + ".csv"), std::ios::out | std::ios::trunc);
        if (fast) {
            fast << "us,current,wanted,chosen,pad";
            for (int i = 0; i < 18; ++i) fast << ",w" << i;
            fast << ",board_up,dark_pop,a_x,a_y,a_z,u_x,u_y,u_z,c_x,c_y,c_z";
            fast << ",plant_x,plant_y,plant_z,plant_w,pup_x,pup_y,pup_z,pup_w,stick_x,stick_z,start_x,start_y,start_z,start_w";
            fast << ",up_pre_tick,up_post_tick,up_post_move,bv_x,bv_y,bv_z,bw_x,bw_y,bw_z,gate_18c0,gate_1838,w13f8,contacts,phys\n";
            g_fast_base = 0;
            fast_probe_enable(true);
        }
        logging::write(logging::Level::info, logging::Channel::skater, std::format("Dark pop probe: writing logs\\dark-pop-probe-{0}.csv and logs\\dark-pop-fast-{0}.csv", stamp));
    }
    // Guarded same-process copies of larger blocks (value<T> is limited to 16 bytes).
    SourceReader reader;
    std::uint32_t state{};
    std::array<std::uint32_t, 40> words{};
    std::array<float, 64> board{};
    source_require(reader.raw(bodies.context + 0x1414, &state, sizeof state) &&
                   reader.raw(bodies.context + 0x13a0, words.data(), sizeof words) &&
                   reader.raw(bodies.parts[0], board.data(), sizeof board),
        "Dark pop probe: the skater's data could not be read.");
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    char text[32];
    file << GetTickCount64() << ',' << state << ',' << (pad.available ? pad.buttons : 0u);
    for (const auto word : words) { std::snprintf(text, sizeof text, ",%x", word); file << text; }
    for (const auto value : board) { std::snprintf(text, sizeof text, ",%.4g", static_cast<double>(value)); file << text; }
    for (const auto value : transform) { std::snprintf(text, sizeof text, ",%.4g", static_cast<double>(value)); file << text; }
    file << '\n';
    fast_probe_flush();
    static ULONGLONG last_flush = 0;
    if (const auto now = GetTickCount64(); now - last_flush > 1000) { file.flush(); if (fast_file().is_open()) fast_file().flush(); last_flush = now; }
}
}
// ---- Revert Boost ---------------------------------------------------------------
// Landing with the board still well out of line with the body (the game's auto revert, or a board
// bend) adds forward speed along the direction of travel, through the same velocity write the
// Forward Boost button uses. Clean and short 180s land with the board lined up and get nothing.
//
// The turn is measured in the physics selector (no_bail.cpp, every physics step) and read on the
// step the flight ends, before the game's revert turns anything.
// Physics states: 100-199 riding, 200-299 air.
namespace {
constexpr std::uint32_t revert_air_first = 200, revert_ground_first = 100;
constexpr float revert_min_spin = 90.0f;         // degrees the skater turned in the air
constexpr float revert_min_board_offset = 40.0f; // board out of line with the body (forward or fakie) at touchdown
constexpr float revert_max_speed = 40.0f;        // m/s (144 km/h): no boost above this
// Boost size (before the strength multiplier), from how far the board turned relative to the body:
// - board bends (up to 140 degrees): +2.0 m/s at 40 degrees, rising to about +2.6 m/s at 140.
// - auto reverts (past 140 degrees): a flat +1.5 m/s, less than any bend.
constexpr float revert_ramp_start = 40.0f, revert_ramp_end = 360.0f; // degrees of board rotation
constexpr float revert_ramp_low = 2.0f, revert_ramp_high = 4.0f;     // m/s along the bend line
constexpr float revert_bend_max_rotation = 140.0f;                   // past this it is an auto revert
constexpr float revert_auto_boost = 1.5f;                            // m/s for an auto revert
constexpr std::uint64_t revert_min_air_ms = 250, revert_max_age_ms = 400, revert_cooldown_ms = 500;
bool revert_is_bend(float board_rotation) { return board_rotation <= revert_bend_max_rotation; }
float revert_boost_amount(float board_rotation) {
    if (!revert_is_bend(board_rotation)) return revert_auto_boost;
    const float t = std::clamp((board_rotation - revert_ramp_start) / (revert_ramp_end - revert_ramp_start), 0.0f, 1.0f);
    return revert_ramp_low + (revert_ramp_high - revert_ramp_low) * t;
}
std::uintptr_t revert_transform_address(std::uintptr_t entity) {
    // The same matrix debug_skater reads (the skater's world transform).
    SourceReader reader;
    const auto collection = reader.pointer(entity, 0x70);
    source_require(reader.pointer(collection) == entity, "Skater transform owner changed.");
    const auto count = reader.value<std::uint8_t>(collection, 8);
    const auto first = reader.value<std::uint8_t>(collection, 9), extra = reader.value<std::uint8_t>(collection, 10);
    source_require(count <= 128 && first <= 128 && extra <= 32, "Skater transform layout changed.");
    reader.verify();
    return collection + std::uintptr_t{0x10} + (std::uintptr_t{first} + 2 * std::uintptr_t{extra}) * 0x20;
}
void revert_boost_tick(InteractiveDebug& debug, std::uintptr_t client, std::uintptr_t entity,
    const NoclipBodies& bodies, bool ready) {
    auto& track = debug.revert_track;
    // Like ReSkate's own features: nothing is read or logged until the player turns it on.
    if (!debug.revert_boost) { track = {}; return; }
    static bool reported_unavailable{};
    if (!no_bail_available()) {
        // The turn is measured in the physics hook No Bail installs; without it nothing is boosted.
        if (!reported_unavailable)
            logging::log(logging::Level::warning, logging::Channel::skater, "Revert Boost: the physics hook is unavailable, so no boosts.");
        reported_unavailable = true;
        return;
    }
    SourceReader reader;
    const auto selector = reader.pointer(bodies.core, 0x440);
    source_require(reader.pointer(selector, 8) == bodies.context, "Skater physics selector changed.");
    const auto physics_state = reader.value<std::uint32_t>(bodies.context, 0x1414);
    const auto velocity = reader.value<std::array<float, 3>>(bodies.parts[0], 0x70);
    reader.verify();
    watch_revert_spin(selector, revert_transform_address(entity), bodies.parts[0]);
    const auto landing = last_revert_landing();
    if (!track.primed) { track.primed = true; track.seen = landing.sequence; return; } // older landings are not ours
    if (landing.sequence == track.seen) return;
    track.seen = landing.sequence;
    const auto now = GetTickCount64();
    const float spin_degrees = std::abs(landing.spin_degrees);
    float board_offset = std::fmod(std::abs(landing.board_offset_degrees), 180.0f);
    board_offset = std::min(board_offset, 180.0f - board_offset); // 0 = lined up forward or fakie
    const bool revert = landing.board_valid && spin_degrees >= revert_min_spin && board_offset >= revert_min_board_offset;
    if (!revert) return; // clean landings, short 180s and straight airs: nothing to do or log
    const bool on_ground = landing.to >= revert_ground_first && landing.to < revert_air_first &&
        physics_state >= revert_ground_first && physics_state < revert_air_first;
    const bool fresh = now >= landing.landed_at && now - landing.landed_at <= revert_max_age_ms;
    const float speed = std::hypot(velocity[0], velocity[2]);
    const float board_rotation = std::abs(landing.board_spin_degrees - landing.spin_degrees);
    const bool bend = revert_is_bend(board_rotation);
    const float wanted = revert_boost_amount(board_rotation) * debug.revert_boost_speed;
    const float added = std::isfinite(speed) ? std::min(wanted, std::max(0.0f, revert_max_speed - speed)) : 0.0f;
    const bool allowed = ready && session_boosts_allowed() &&
        source_state().velocity_guard_active.load(std::memory_order_acquire) &&
        !debug.noclip && !debug.park_editor && !bodies.offboard && !debug.forward_velocity.valid;
    const bool fire = allowed && on_ground && fresh && landing.air_ms >= revert_min_air_ms &&
        now >= track.cooldown_until && std::isfinite(speed) && speed >= 1.0f && added > 0.0f;
    if (fire) {
        const float scale = added / speed;
        const std::array<float, 3> delta{velocity[0] * scale, 0.0f, velocity[2] * scale};
        debug.forward_velocity = {client, entity, bodies.core, now + 1000, delta, true};
        track.cooldown_until = now + revert_cooldown_ms;
        ++debug.revert_boost_count;
    }
    const char* outcome = fire ? "" : !allowed ? "not allowed right now" : !on_ground ? "not riding"
        : !fresh ? "late" : landing.air_ms < revert_min_air_ms ? "short flight" : now < track.cooldown_until ? "cooldown" : "speed cap";
    logging::log(logging::Level::info, logging::Channel::skater,
        "Revert Boost: {} after {} ms in the air, spin {:.0f} deg, board {:.0f} deg off, turned {:.0f} deg past the body, {:.1f} m/s, {}",
        bend ? "board bend" : "auto revert", landing.air_ms, spin_degrees, board_offset, board_rotation, speed,
        fire ? std::format("+{:.1f} m/s", added) : std::format("no boost ({})", outcome));
}
}
std::array<float, 16> debug_skater(std::uintptr_t base, std::uintptr_t client, overlay::DebugModel& model) {
    SourceReader reader;
    source_require(reader.pointer(client) == base + addr::engine::client_vtable, "Client changed.");
    const auto context = reader.pointer(client, 8);
    const auto offset = reader.value<std::uint32_t>(base, addr::engine::context_player_manager_offset);
    source_require(offset <= 0x1000000, "Player manager offset changed.");
    const auto manager = reader.pointer(context, offset);
    source_require(reader.pointer(manager) == base + addr::engine::local_player_manager_vtable, "Player manager changed.");
    const auto begin = reader.pointer(manager, 0x4c8), end = reader.pointer(manager, 0x4d0);
    source_require(reader.count(begin, end, 8) == 1, "No unique local skater.");
    const auto player = reader.pointer(begin);
    source_require(reader.pointer(player) == base + addr::engine::local_player_vtable && reader.pointer(player, 0x78) == context &&
        reader.value<std::uint8_t>(player, 0x45) == 1 && !reader.value<std::uint8_t>(player, 0x44), "Local player changed.");
    const auto entity = reader.pointer(player, 0xb8), handle = reader.pointer(player, 0xb0);
    source_require(reader.pointer(entity) == base + addr::engine::skater_entity_vtable && reader.pointer(entity, 0x20) == context &&
        reader.pointer(entity, 0xf8) == player && reader.pointer(handle) == entity + 8,
        "No verified player-bound skater.");
    const auto collection = reader.pointer(entity, 0x70);
    source_require(reader.pointer(collection) == entity, "Skater transform owner changed.");
    const auto count = reader.value<std::uint8_t>(collection, 8);
    const auto first = reader.value<std::uint8_t>(collection, 9), extra = reader.value<std::uint8_t>(collection, 10);
    source_require(count <= 128 && first <= 128 && extra <= 32, "Skater transform layout changed.");
    const auto offset_matrix = std::uintptr_t{0x10} + (std::uintptr_t{first} + 2 * std::uintptr_t{extra}) * 0x20;
    const auto position = debug_position(reader, collection + offset_matrix + 0x30);
    std::array<float, 16> matrix{};
    source_require(reader.raw(collection + offset_matrix, matrix.data(), sizeof(matrix)) && valid_flight_transform(matrix),
        "Skater transform is invalid.");
    reader.verify();
    model.skater_position = position;
    model.skater_identity = entity;
    model.skater_position_valid = true;
    return matrix;
}
namespace {
bool debug_has_lease(const InteractiveDebug& debug) {
    return debug.no_bail || debug.noclip || debug.camera_owned || debug.camera_ambiguous || debug.ui_owned;
}
void debug_restore_ui(SourceTrial& trial) {
    auto& debug = trial.debug;
    if (!debug.ui_owned) return;
    const auto ui = debug_ui(trial.base);
    source_require(ui.object == debug.ui_object, "UI settings were replaced; restoration deferred.");
    if (ui.draw == debug.ui_applied) debug_write(ui.object + 0x4e, debug.ui_original);
    debug.ui_owned = false;
}
void debug_restore_camera(SourceTrial& trial, std::uintptr_t client, bool phase, DWORD error) {
    auto& debug = trial.debug;
    debug_stop_noclip(debug);
    source_require(!debug.camera_ambiguous, "Native camera call had an uncertain result; restart before changing camera mode.");
    if (!debug.camera_owned) { debug.first_person = false; debug.drone_follow = false; debug.first_person_saved_fov = 0; return; }
    source_require(phase, "Waiting for the native camera update phase.");
    auto camera = source_camera_snapshot(trial, client);
    if (debug.camera_owned) {
        source_require(camera.identity == debug.camera_identity, "Camera was replaced; restoration deferred.");
        source_require(camera.mode <= 1, "Another native camera mode is active; it was left unchanged.");
        if (camera.mode == 1) {
            source_require(camera.active == camera.identity.camera, "Free camera selection changed.");
            first_person_disarm();
            if (debug.first_person) first_person_restore_fov(debug);
            free_camera_restore_fov(debug);
            debug.camera_ambiguous = true; // Cleared only after a native return; no retry on an exception.
            SetLastError(error);
            trial.camera_mode(camera.identity.controller, 0);
            debug.camera_ambiguous = false;
            camera = source_camera_snapshot(trial, client);
            source_require(camera.identity == debug.camera_identity && !camera.mode &&
                camera.active != camera.identity.camera && !camera.input_active, "Camera restoration could not be verified.");
        }
        source_require(camera.active != camera.identity.camera && !camera.input_active,
            "Mode zero still has active free-camera state; restoration is unverified.");
        debug.camera_owned = false;
        debug.flight_ready = false;
        debug.first_person = false;
        debug.drone_follow = false;
    }
}
// The game's own collision query (the Park Editor's ray), so the drone hits ground, walls and ramps.
const drone::World& drone_world() {
    static const drone::World world{[](const std::array<float, 3>& from, const std::array<float, 3>& to,
                                      std::array<float, 3>& hit) {
        const auto found = local_surface_hit(from, to);
        if (!found) return false;
        hit = *found;
        return true;
    }};
    return world;
}
// Drone follow: the pilot's tuning from the DRONE page's follow distance and aggression.
drone::follow::Tuning follow_tuning() {
    const auto& values = drone::knobs().value;
    return drone::follow::tuning_for(values[drone::kFollowDistance].load(std::memory_order_relaxed),
                                     values[drone::kFollowAggression].load(std::memory_order_relaxed));
}
// Drone follow: the skater's position and which way it faces, or nothing between lives.
std::optional<std::pair<drone::detail::Vec3, drone::detail::Vec3>> follow_target(SourceTrial& trial, std::uintptr_t client) {
    try {
        overlay::DebugModel unused;
        const auto skater = debug_skater(trial.base, client, unused);
        return std::pair{drone::detail::Vec3{skater[12], skater[13], skater[14]}, drone::detail::row(skater, 2)};
    } catch (...) {
        return std::nullopt;
    }
}
// `taken` receives the camera snapshot this step read, for the tick's model to reuse:
// nothing after it here changes the camera's identity or mode (only its pose and FOV).
void debug_flight_tick(SourceTrial& trial, std::uintptr_t client, bool ready, bool phase,
    const overlay::FlightInput* input, DWORD error, std::optional<SourceCameraSnapshot>& taken) {
    auto& debug = trial.debug;
    const auto now = source_flight_now();
    const auto elapsed = std::clamp(now - debug.flight_time, 0.0, .05);
    debug.flight_time = now;
    if (!debug.first_person || !debug.camera_owned || debug.camera_ambiguous || !ready || !phase) first_person_disarm();
    if ((!debug.camera_owned && !debug.noclip) || debug.camera_ambiguous || !ready || !phase) {
        debug.noclip_velocity.valid = false;
        clear_no_bail_flight();
        return;
    }
    const overlay::FlightInput idle{};
    const auto seconds = static_cast<float>(elapsed);
    const auto camera = source_camera_snapshot(trial, client);
    taken = camera;
    if (debug.noclip) {
        source_require(camera.identity == debug.camera_identity && camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "Player camera changed; flight stopped.");
        const auto bodies = debug_noclip_bodies(trial.base, client, debug.noclip_entity);
        const auto view = debug_view_matrix(trial, camera);
        debug.noclip_velocity = {client, debug.noclip_entity, bodies.core, GetTickCount64() + 250,
            flight_velocity(view, input ? *input : idle, debug.flight_speed), true};
        return;
    }
    if (!debug.flight_ready) return;
    source_require(camera.identity == debug.camera_identity && camera.mode == 1 && camera.active == camera.identity.camera,
        debug.first_person ? "First person lost the camera; view paused." : "Freecam ownership changed; movement paused.");
    auto next = debug.flight_matrix;
    if (debug.first_person) {
        // Respawns, loads and bails can briefly leave no readable head. Hold the
        // last pose instead of dropping the view, and say why once.
        std::uintptr_t component{};
        first_person::Vec3 origin{};
        auto& arm = first_person_arm();
        try {
            component = first_person_component(trial.base, client);
            arm.watched_base.store(trial.base, std::memory_order_release);
            arm.watched.store(component, std::memory_order_release);
            // Newest capture of this skater, if one is recent; otherwise read now.
            std::optional<FirstPersonArm::Snapshot> latest;
            {
                std::lock_guard lock(arm.snapshot_mutex);
                // A capture stays usable while animation pauses: the pose in
                // memory then still has the head hidden and cannot be re-read.
                if (arm.captures && now - arm.latest.time < 2.0 && component == arm.component) {
                    latest = arm.latest;
                }
            }
            if (latest) {
                // The tick runs before this frame's animation, so the newest
                // capture is last frame's head. The render handoff replaces it
                // with this frame's; this only covers frames without one.
                for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) next[i] = latest->head[i];
                origin = latest->origin;
            } else origin = first_person_head_matrix(trial.base, component, next);
        } catch (const SourceGuard& guard) {
            first_person_disarm();
            debug.status = guard.message;
            debug.first_person_waiting = true;
            return;
        }
        if (arm.component != component || arm.camera != camera.identity.camera) arm.spring.reset();
        arm.component = component;
        arm.camera = camera.identity.camera;
        arm.transform = trial.camera_transform;
        arm.fov = debug.first_person_fov;
        arm.settings = debug.first_person_settings;
        arm.spring.begin(now);
        next = arm.spring.sample(next, arm.settings, origin);
        source_require(valid_flight_transform(next), "First-person spring pose is invalid.");
        arm.matrix = next;
        arm.thread.store(GetCurrentThreadId(), std::memory_order_release);
        if (debug.first_person_waiting) {
            debug.first_person_waiting = false;
            debug.status = debug.first_person_settings.stabilize
                ? "First person on. True first person keeps the view level and steady."
                : "First person on. The camera follows the skater's head.";
        }
    } else if (debug.drone_follow) {
        // The autopilot flies the drone behind the skater through the flight model; between
        // lives (a bail, a respawn) it holds on to where the skater was last.
        drone::audio::ensure_started();
        const auto tuning = follow_tuning();
        const auto cfg = drone::follow::flight_settings(drone::live_settings(), tuning);
        const auto& world = drone_world();
        const auto target = follow_target(trial, client);
        const drone::detail::Vec3 at{debug.flight_matrix[12], debug.flight_matrix[13], debug.flight_matrix[14]};
        const drone::detail::Vec3 skater = target ? target->first : debug.follow_pilot.last_target;
        const drone::detail::Vec3 facing = target ? target->second : debug.follow_pilot.heading;
        if (target && drone::detail::length(drone::detail::sub(skater, at)) > 60.0f) {
            // Left far behind (a respawn, a teleport): it is back where it should be.
            const auto spot = drone::follow::spot(skater, {0, 0, 0}, debug.follow_pilot.heading, tuning, 0.0f);
            debug.flight_matrix = drone::follow::pose_facing(spot, skater);
            debug.drone_state.valid = false;
            debug.follow_pilot = {};
        }
        const auto cmd = drone::follow::fly(debug.follow_pilot, debug.drone_state, at, skater, facing, seconds, cfg, tuning, &world);
        next = drone::step_drone_cmd(debug.flight_matrix, debug.drone_state, cmd, seconds, std::max(debug.flight_speed, 8.0f), cfg, &world);
    } else {
        if (drone::is_enabled()) {
            drone::audio::ensure_started();
            const auto& world = drone_world();
            bool flown_by_radio = false;
            if (drone::radio::calibrating()) {
                // The controller setup is moving the sticks and switches: hold the drone where it is.
                next = debug.flight_matrix;
                flown_by_radio = true;
            } else if (drone::radio::config().enabled.load(std::memory_order_acquire) && drone::radio::game_in_foreground() &&
                       drone::gamepad_mode()) {
                // A gamepad, read directly (it is kept from the game while the drone has it).
                overlay::Gamepad pad;
                {
                    drone::radio::detail::InputAccess access;
                    pad = overlay::read_gamepad();
                }
                if (pad.valid) {
                    // While the ReSkate menu is open (no flight input) the pad is the menu's: the drone
                    // holds still in the air and the buttons do nothing.
                    const bool menu_closed = input && input->active;
                    drone::apply_gamepad_buttons(pad.buttons, menu_closed);
                    overlay::Gamepad resting;
                    resting.valid = true;
                    next = drone::step_drone_radio(debug.flight_matrix, debug.drone_state,
                        drone::read_gamepad(menu_closed ? pad : resting), seconds, debug.flight_speed, drone::live_settings(), &world);
                    flown_by_radio = true;
                }
            } else if (drone::radio::config().enabled.load(std::memory_order_acquire) && drone::radio::game_in_foreground()) {
                const auto sample = drone::radio::poll();
                if (sample.valid) {
                    drone::apply_switches(sample.axes, sample.buttons);
                    next = drone::step_drone_radio(debug.flight_matrix, debug.drone_state,
                        drone::read_sticks(sample.axes), seconds, debug.flight_speed,
                        drone::live_settings(), &world);
                    flown_by_radio = true;
                }
            }
            if (!flown_by_radio)
                next = drone::step_drone_flight(debug.flight_matrix, debug.drone_state, input ? *input : idle, seconds, debug.flight_speed,
                    drone::live_settings(), &world);
        } else {
            debug.drone_state.valid = false; // restart from the current view next time drone mode turns on
            next = step_free_flight(debug.flight_matrix, input ? *input : idle, seconds, debug.flight_speed);
        }
    }
    // Runs after the native client update on the same engine thread. Publishing
    // our owned pose each tick also prevents native helper input from drifting it.
    debug.flight_ready = false; // An exceptional setter must not be retried automatically.
    SetLastError(error);
    trial.camera_transform(camera.identity.camera, &next);
    debug.flight_matrix = next;
    debug.flight_ready = true;
    // Written every tick: the camera may refresh its FOV from its own settings.
    if (debug.first_person && debug.first_person_fov > 0)
        (void)first_person_write_fov(camera.identity.camera, debug.first_person_fov);
    else if (!debug.first_person && debug.free_camera_fov > 0) {
        if (!debug.free_camera_saved_fov) {
            float current{};
            debug.free_camera_saved_fov = first_person_read(camera.identity.camera + camera_fov_offset, &current, 4) &&
                std::isfinite(current) && current > 1.0f && current < 175.0f ? current : 0.0f;
        }
        (void)first_person_write_fov(camera.identity.camera, debug.free_camera_fov);
    }
}
// ---- saved choices ("ReSkate.*" in the local profile)
namespace saved {
constexpr const char* no_bail = "NoBail";
constexpr const char* flight_speed = "FlightSpeed";
constexpr const char* forward_velocity = "ForwardVelocity";
constexpr const char* up_velocity = "UpVelocity";
constexpr const char* offboard_up_velocity = "OffboardUpVelocity";
constexpr const char* revert_boost = "RevertBoost";
constexpr const char* revert_boost_strength = "RevertBoost.Strength";
constexpr const char* dark_pop = "DarkPop";
constexpr const char* first_person_fov = "FirstPerson.Fov";
constexpr const char* free_camera_fov = "FreeCamera.Fov";
constexpr const char* spring = "FirstPerson.Spring";
constexpr std::array<const char*, 3> offset{"FirstPerson.OffsetX", "FirstPerson.OffsetY", "FirstPerson.OffsetZ"};
constexpr std::array<const char*, 3> rotation{"FirstPerson.Pitch", "FirstPerson.Yaw", "FirstPerson.Roll"};
constexpr std::array<const char*, 4> strength{"FirstPerson.Up", "FirstPerson.Down", "FirstPerson.Left", "FirstPerson.Right"};
constexpr const char* stabilize = "FirstPerson.Stabilize";
constexpr const char* follow_flips = "FirstPerson.FollowFlips";
constexpr const char* board_only = "FirstPerson.ThirdPersonOnFoot";
constexpr const char* camera_cycle = "FirstPerson.CameraCycle";
constexpr std::array<const char*, 4> steady{"FirstPerson.Smoothing", "FirstPerson.HeadPitch", "FirstPerson.HeadRoll", "FirstPerson.Bob"};
constexpr ULONGLONG delay_ms = 750;

std::optional<float> number(const char* key) {
    const auto value = profile_runtime::local_value(key);
    if (!value || !value->is_number()) return {};
    const auto result = static_cast<float>(value->get<double>());
    return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}
} // namespace saved

void mark_debug_changed(InteractiveDebug& debug) noexcept {
    debug.save_pending = true;
    debug.save_due = GetTickCount64() + saved::delay_ms;
}

void save_debug(InteractiveDebug& debug) noexcept {
    debug.save_pending = false;
    try {
        const auto& arm = debug.first_person_settings;
        std::vector<std::pair<std::string, Json>> values{
            {saved::no_bail, debug.no_bail || debug.no_bail_restore},
            {saved::flight_speed, static_cast<double>(debug.flight_speed)},
            {saved::forward_velocity, static_cast<double>(debug.forward_velocity_speed)},
            {saved::up_velocity, static_cast<double>(debug.up_velocity_speed)},
            {saved::offboard_up_velocity, static_cast<double>(debug.offboard_up_velocity_speed)},
            {saved::revert_boost, debug.revert_boost},
            {saved::revert_boost_strength, static_cast<double>(debug.revert_boost_speed)},
            {saved::dark_pop, debug.dark_pop},
            {saved::first_person_fov, static_cast<double>(debug.first_person_fov)},
            {saved::free_camera_fov, static_cast<double>(debug.free_camera_fov)},
            {saved::spring, arm.enabled}};
        for (std::size_t i = 0; i < 3; ++i) {
            values.emplace_back(saved::offset[i], static_cast<double>(arm.offset[i]));
            values.emplace_back(saved::rotation[i], static_cast<double>(arm.rotation[i]));
        }
        const std::array<float, 4> strengths{arm.up, arm.down, arm.left, arm.right};
        for (std::size_t i = 0; i < 4; ++i) values.emplace_back(saved::strength[i], static_cast<double>(strengths[i]));
        values.emplace_back(saved::stabilize, arm.stabilize);
        values.emplace_back(saved::follow_flips, arm.follow_flips);
        values.emplace_back(saved::board_only, arm.board_only);
        values.emplace_back(saved::camera_cycle, arm.camera_cycle);
        const std::array<float, 4> steady{arm.smoothing, arm.head_pitch, arm.head_roll, arm.bob};
        for (std::size_t i = 0; i < 4; ++i) values.emplace_back(saved::steady[i], static_cast<double>(steady[i]));
        profile_runtime::set_local_values(values);
    } catch (...) { /* Saving is best effort; the choices already apply. */ }
}

// Applies saved choices over the current values; anything missing or out of
// range keeps its default. No Bail is re-applied by the tick once a skater exists.
void load_saved_debug(InteractiveDebug& debug) noexcept {
    try {
        if (const auto value = profile_runtime::local_preference(saved::no_bail); value && *value && !debug.no_bail) {
            debug.no_bail_restore = true;
            debug.no_bail_restore_after = GetTickCount64() + 2000;
        }
        if (const auto speed = saved::number(saved::flight_speed);
            speed && valid_debug_speed(*speed))
            debug.flight_speed = *speed;
        if (const auto speed = saved::number(saved::forward_velocity); speed && *speed >= 0.0f && *speed <= velocity_boost_speed_max)
            debug.forward_velocity_speed = *speed;
        if (const auto speed = saved::number(saved::up_velocity); speed && *speed >= 0.0f && *speed <= velocity_boost_speed_max)
            debug.up_velocity_speed = *speed;
        if (const auto speed = saved::number(saved::offboard_up_velocity); speed && *speed >= 0.0f && *speed <= velocity_boost_speed_max)
            debug.offboard_up_velocity_speed = *speed;
        if (const auto enabled = profile_runtime::local_preference(saved::revert_boost)) debug.revert_boost = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::dark_pop)) debug.dark_pop = *enabled;
        if (const auto strength = saved::number(saved::revert_boost_strength);
            strength && *strength >= revert_boost_strength_min && *strength <= revert_boost_strength_max)
            debug.revert_boost_speed = *strength;
        if (const auto fov = saved::number(saved::first_person_fov);
            fov && *fov >= first_person_fov_min && *fov <= first_person_fov_max) {
            debug.first_person_fov = *fov;
            first_person_arm().fov = *fov;
        }
        if (const auto fov = saved::number(saved::free_camera_fov);
            fov && (*fov == 0.0f || (*fov >= first_person_fov_min && *fov <= first_person_fov_max)))
            debug.free_camera_fov = *fov;
        // Drone tuning saved by `dronetune`.
        for (std::size_t i = 0; i < drone::knob_count; ++i) {
            const auto tuned = saved::number(std::format("Drone.Tune.{}", drone::knob_info[i].name).c_str());
            if (tuned && *tuned >= drone::knob_info[i].lo && *tuned <= drone::knob_info[i].hi) drone::knobs().value[i] = *tuned;
        }
        // Drone radio: on/off and which axis is which (signed 1..6), saved by the console commands.
        if (const auto radio_on = saved::number("Drone.Radio")) drone::radio::config().enabled = *radio_on != 0.0f;
        for (int i = 0; i < 4; ++i) {
            const auto axis = saved::number(std::format("Drone.Axis{}", i).c_str());
            const int signed_axis = axis ? static_cast<int>(std::lround(*axis)) : 0;
            if (signed_axis != 0 && std::abs(signed_axis) <= 6) drone::radio::config().map[i] = signed_axis;
        }
        auto arm = debug.first_person_settings;
        if (const auto enabled = profile_runtime::local_preference(saved::spring)) arm.enabled = *enabled;
        for (std::size_t i = 0; i < 3; ++i) {
            if (const auto value = saved::number(saved::offset[i])) arm.offset[i] = *value;
            if (const auto value = saved::number(saved::rotation[i])) arm.rotation[i] = *value;
        }
        const std::array<float*, 4> strengths{&arm.up, &arm.down, &arm.left, &arm.right};
        for (std::size_t i = 0; i < 4; ++i)
            if (const auto value = saved::number(saved::strength[i])) *strengths[i] = *value;
        if (const auto enabled = profile_runtime::local_preference(saved::stabilize)) arm.stabilize = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::follow_flips)) arm.follow_flips = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::board_only)) arm.board_only = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::camera_cycle)) arm.camera_cycle = *enabled;
        const std::array<float*, 4> steady{&arm.smoothing, &arm.head_pitch, &arm.head_roll, &arm.bob};
        for (std::size_t i = 0; i < 4; ++i)
            if (const auto value = saved::number(saved::steady[i])) *steady[i] = *value;
        if (first_person::valid(arm)) {
            debug.first_person_settings = arm;
            first_person_arm().settings = arm;
        }
    } catch (...) { /* Unreadable choices leave the defaults in place. */ }
}

void debug_action(SourceTrial& trial, std::uintptr_t client, bool can_control, bool phase,
                  const overlay::DebugRequest& request, DWORD error) {
    auto& debug = trial.debug;
    if (request.action == overlay::DebugAction::set_park_editor) {
        source_require(!request.enabled || lobby_object_placement_allowed(),
            "The host has disabled object placement for you in this session.");
        if (request.enabled == debug.park_editor) return;
        source_require(can_control && phase, "Wait for the local camera before changing editor mode.");
        debug.editor_transition = true;
        struct EndTransition { bool& flag; ~EndTransition(){flag=false;} } transition{debug.editor_transition};
        const auto apply = [&](overlay::DebugAction action, bool enabled) {
            debug_action(trial, client, can_control, phase, {action, enabled}, error);
        };
        if (request.enabled) {
            const auto camera = source_camera_snapshot(trial, client);
            debug.editor_previous_camera = camera.mode == 1;
            debug.editor_previous_ui = debug_ui(trial.base).draw == 0;
            debug.editor_previous_noclip = debug.noclip;
            debug.editor_previous_first_person = debug.first_person;
            // Retain the restoration state even if activation fails halfway.
            debug.park_editor = true;
            try {
                apply(overlay::DebugAction::set_free_camera, true);
                apply(overlay::DebugAction::set_game_ui_hidden, true);
            } catch (...) {
                try {
                    apply(overlay::DebugAction::set_game_ui_hidden, debug.editor_previous_ui);
                    if (!debug.editor_previous_camera) apply(overlay::DebugAction::set_free_camera, false);
                    if (debug.editor_previous_noclip && session_noclip_allowed()) apply(overlay::DebugAction::set_noclip, true);
                    if (debug.editor_previous_first_person) apply(overlay::DebugAction::set_first_person, true);
                    debug.park_editor = false;
                } catch (...) {}
                throw;
            }
            debug.status = "Park editor enabled. Hold RMB over the viewport to fly.";
        } else {
            apply(overlay::DebugAction::set_game_ui_hidden, debug.editor_previous_ui);
            if (!debug.editor_previous_camera) apply(overlay::DebugAction::set_free_camera, false);
            if (debug.editor_previous_noclip && session_noclip_allowed()) apply(overlay::DebugAction::set_noclip, true);
            if (debug.editor_previous_first_person) apply(overlay::DebugAction::set_first_person, true);
            debug.park_editor = false;
            debug.status = "Park editor closed. Previous camera and UI settings restored.";
        }
        return;
    }
    source_require(!debug.park_editor || debug.editor_transition || request.action == overlay::DebugAction::restore_debug ||
        request.action == overlay::DebugAction::set_camera_speed || request.action == overlay::DebugAction::set_forward_velocity_speed ||
        request.action == overlay::DebugAction::set_up_velocity_speed ||
        request.action == overlay::DebugAction::set_offboard_up_velocity_speed ||
        request.action == overlay::DebugAction::set_first_person_fov ||
        request.action == overlay::DebugAction::set_free_camera_fov ||
        (request.action >= overlay::DebugAction::set_first_person_spring && request.action <= overlay::DebugAction::reset_first_person_arm) ||
        request.action == overlay::DebugAction::set_no_bail || request.action == overlay::DebugAction::set_dark_pop_unpatch ||
        request.action == overlay::DebugAction::set_dark_pop_hold ||
        request.action == overlay::DebugAction::set_dark_pop_one_press ||
        request.action == overlay::DebugAction::set_dark_pop_route_stay || request.action == overlay::DebugAction::set_dark_pop_route_guard ||
        request.action == overlay::DebugAction::set_dark_pop_keep_speed || request.action == overlay::DebugAction::set_dark_pop_catch ||
        request.action == overlay::DebugAction::set_dark_pop_feet || request.action == overlay::DebugAction::set_dark_pop_adaptive ||
        request.action == overlay::DebugAction::set_dark_pop_timeout || request.action == overlay::DebugAction::set_dark_pop_strength ||
        request.action == overlay::DebugAction::set_dark_pop_hold_landing || request.action == overlay::DebugAction::set_dark_pop_dark_exit ||
        request.action == overlay::DebugAction::set_dark_pop_full_flight || request.action == overlay::DebugAction::set_dark_pop_hold_flight ||
        request.action == overlay::DebugAction::set_dark_pop_stay_physical || request.action == overlay::DebugAction::set_dark_pop_early_physics ||
        request.action == overlay::DebugAction::set_dark_pop_reaim || request.action == overlay::DebugAction::set_dark_pop_hide_dpad ||
        (request.action == overlay::DebugAction::set_dark_pop_enabled && !request.enabled),
        "Close Park Editor before changing camera or HUD modes.");
    if (request.action >= overlay::DebugAction::set_first_person_spring &&
        request.action <= overlay::DebugAction::reset_first_person_arm) {
        auto settings = debug.first_person_settings;
        using Action = overlay::DebugAction;
        switch (request.action) {
        case Action::set_first_person_spring: settings.enabled = request.enabled; break;
        case Action::set_first_person_offset_x: settings.offset[0] = request.value; break;
        case Action::set_first_person_offset_y: settings.offset[1] = request.value; break;
        case Action::set_first_person_offset_z: settings.offset[2] = request.value; break;
        case Action::set_first_person_pitch: settings.rotation[0] = request.value; break;
        case Action::set_first_person_yaw: settings.rotation[1] = request.value; break;
        case Action::set_first_person_roll: settings.rotation[2] = request.value; break;
        case Action::set_first_person_spring_up: settings.up = request.value; break;
        case Action::set_first_person_spring_down: settings.down = request.value; break;
        case Action::set_first_person_spring_left: settings.left = request.value; break;
        case Action::set_first_person_spring_right: settings.right = request.value; break;
        case Action::set_first_person_stabilize: settings.stabilize = request.enabled; break;
        case Action::set_first_person_follow_flips: settings.follow_flips = request.enabled; break;
        case Action::set_first_person_smoothing: settings.smoothing = request.value; break;
        case Action::set_first_person_head_pitch: settings.head_pitch = request.value; break;
        case Action::set_first_person_head_roll: settings.head_roll = request.value; break;
        case Action::set_first_person_bob: settings.bob = request.value; break;
        case Action::set_first_person_board_only: settings.board_only = request.enabled; break;
        case Action::set_first_person_camera_cycle: settings.camera_cycle = request.enabled; break;
        // Resets the view; which steps the camera height button goes through is kept.
        case Action::reset_first_person_arm: settings = {.camera_cycle = settings.camera_cycle}; break;
        default: break;
        }
        source_require(first_person::valid(settings), "First-person arm setting is outside its supported range.");
        auto& arm = first_person_arm();
        if (settings.enabled != debug.first_person_settings.enabled ||
            settings.stabilize != debug.first_person_settings.stabilize || request.action == Action::reset_first_person_arm)
            arm.spring.reset();
        debug.first_person_settings = settings;
        arm.settings = settings;
        mark_debug_changed(debug);
        debug.status = request.action == Action::set_first_person_stabilize
            ? (settings.stabilize ? "True first person on: level horizon, steady view." : "True first person off: the view follows the raw head.")
            : request.action == Action::set_first_person_board_only
            ? (settings.board_only ? "Third person while walking; first person on the board." : "First person while walking too.")
            : request.action == Action::set_first_person_camera_cycle
            ? (settings.camera_cycle ? "Camera height button: high, low, first person." : "Camera height button: high and low only.")
            : "First-person settings updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_free_camera_fov) {
        source_require(request.value == 0.0f || (std::isfinite(request.value) && request.value >= first_person_fov_min &&
            request.value <= first_person_fov_max), "Freecam FOV must be between 40 and 120 degrees.");
        debug.free_camera_fov = request.value;
        if (request.value == 0.0f) free_camera_restore_fov(debug);
        mark_debug_changed(debug);
        debug.status = request.value == 0.0f ? "Freecam uses the game's own FOV." : "Freecam FOV updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_camera_speed) {
        source_require(valid_debug_speed(request.value), "Flight speed must be between 0.6 and 1500.");
        debug.flight_speed = request.value;
        mark_debug_changed(debug);
        debug.status = "Flight speed updated. Mouse sensitivity is unchanged.";
        return;
    }
    if (request.action == overlay::DebugAction::set_forward_velocity_speed ||
        request.action == overlay::DebugAction::set_up_velocity_speed ||
        request.action == overlay::DebugAction::set_offboard_up_velocity_speed) {
        if (request.action == overlay::DebugAction::set_offboard_up_velocity_speed) {
            source_require(std::isfinite(request.value) && request.value >= 0.0f && request.value <= velocity_boost_speed_max,
                "Off-board Up Boost speed must be between 0 (off) and 10000.");
            debug.offboard_up_velocity_speed = request.value;
            debug.status = "Off-board Up Boost speed updated.";
        } else {
            const bool up = request.action == overlay::DebugAction::set_up_velocity_speed;
            source_require(std::isfinite(request.value) && request.value >= 0.0f && request.value <= velocity_boost_speed_max,
                up ? "Up Boost speed must be between 0 (off) and 10000." : "Forward Boost speed must be between 0 (off) and 10000.");
            (up ? debug.up_velocity_speed : debug.forward_velocity_speed) = request.value;
            debug.status = up ? "Up Boost speed updated." : "Forward Boost speed updated.";
        }
        mark_debug_changed(debug);
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_unpatch) {
        debug.dark_pop_unpatch = request.enabled;
        debug.status = request.enabled ? "Dark Pop unpatch on." : "Dark Pop unpatch off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop unpatch on" : "Dark Pop unpatch off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_hold) {
        debug.dark_pop_hold = request.enabled;
        debug.status = request.enabled ? "Dark Pop hold on." : "Dark Pop hold off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop hold on" : "Dark Pop hold off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_one_press) {
        debug.dark_pop_one_press = request.enabled;
        debug.status = request.enabled ? "Dark Pop one press one pop on." : "Dark Pop one press one pop off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop one press one pop on" : "Dark Pop one press one pop off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_catch) {
        debug.dark_pop_catch = request.enabled;
        debug.status = request.enabled ? "Dark Pop catches early dismounts." : "Dark Pop leaves early dismounts to the game.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop early-dismount catch on" : "Dark Pop early-dismount catch off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_dark_exit) {
        debug.dark_pop_dark_exit = request.enabled;
        debug.status = request.enabled ? "Dark Pop dark exit on." : "Dark Pop dark exit off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop dark exit on" : "Dark Pop dark exit off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_full_flight) {
        debug.dark_pop_full_flight = request.enabled;
        debug.status = request.enabled ? "Dark Pop full flight on." : "Dark Pop full flight off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop full flight on" : "Dark Pop full flight off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_hide_dpad) {
        debug.dark_pop_hide_dpad = request.enabled;
        debug.status = request.enabled ? "Dark Pop: the D-pad is hidden from the game in the air (no Quick Drop)." : "Dark Pop D-pad hide off: the game sees the press.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop D-pad hide on" : "Dark Pop D-pad hide off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_reaim) {
        debug.dark_pop_reaim = request.enabled;
        debug.status = request.enabled ? "Dark Pop: an early-jump pop's flight curve is re-aimed at the handover." : "Dark Pop re-aim off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop re-aim on" : "Dark Pop re-aim off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_early_physics) {
        debug.dark_pop_early_physics = request.enabled;
        debug.status = request.enabled ? "Dark Pop: an early-jump pop flies on physics." : "Dark Pop physics flight off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop physics flight on" : "Dark Pop physics flight off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_stay_physical) {
        debug.dark_pop_stay_physical = std::clamp(std::round(request.value), 0.0f, 60.0f);
        debug.status = std::format("Dark Pop stays physical for {} tick(s) after the pop.", static_cast<unsigned>(debug.dark_pop_stay_physical));
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop stay physical {} tick(s)", static_cast<unsigned>(debug.dark_pop_stay_physical));
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_hold_flight) {
        debug.dark_pop_hold_flight = request.enabled;
        debug.status = request.enabled ? "Dark Pop holds the flight: no contact landing while the countdown runs." : "Dark Pop flight hold off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop hold the flight on" : "Dark Pop hold the flight off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_hold_landing) {
        debug.dark_pop_hold_landing = request.enabled;
        debug.status = request.enabled ? "Dark Pop holds the landing for the game to stage it." : "Dark Pop landing hold off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop landing hold on" : "Dark Pop landing hold off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_strength) {
        debug.dark_pop_strength = std::clamp(request.value, 0.0f, 15.0f);
        debug.status = debug.dark_pop_strength > 0.0f ? std::format("Dark Pop launch {:.1f} m/s.", debug.dark_pop_strength) : std::string("Dark Pop launch off (the game's own launch).");
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop launch {:.1f}", debug.dark_pop_strength);
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_timeout) {
        debug.dark_pop_timeout = request.enabled;
        debug.status = request.enabled ? "Dark Pop catches timed-out landings." : "Dark Pop leaves timed-out landings to the game.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop timed-out landing catch on" : "Dark Pop timed-out landing catch off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_adaptive) {
        debug.dark_pop_adaptive = request.enabled;
        debug.status = request.enabled ? "Dark Pop pops when the board starts to roll." : "Dark Pop sinks for the full sink length.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop adaptive sink on" : "Dark Pop adaptive sink off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_feet) {
        debug.dark_pop_feet = request.enabled;
        if (request.enabled && debug.dark_pop_route_stay < 1.0f) {
            debug.dark_pop_route_stay = 11.0f; // a 12-tick sink, the middle of the old 8-19
            debug.status = "Dark Pop feet-only hold on; sink length set to 11 (a 12-tick sink). Change it under Sink length.";
        } else {
            debug.status = request.enabled ? "Dark Pop feet-only hold on." : "Dark Pop feet-only hold off (the game's footplant attaches run in the sink).";
        }
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop feet-only hold {} (sink length {})", request.enabled ? "on" : "off", static_cast<unsigned>(debug.dark_pop_route_stay));
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_keep_speed) {
        debug.dark_pop_keep_speed = request.enabled;
        debug.status = request.enabled ? "Dark Pop keep speed on." : "Dark Pop keep speed off.";
        logging::write(logging::Level::info, logging::Channel::skater, request.enabled ? "Dark Pop keep speed on" : "Dark Pop keep speed off");
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_route_guard) {
        debug.dark_pop_route_guard = std::clamp(request.value, 0.0f, 600.0f);
        debug.status = std::format("Dark Pop pop guard {} tick(s).", static_cast<unsigned>(debug.dark_pop_route_guard));
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop pop guard {} tick(s)", static_cast<unsigned>(debug.dark_pop_route_guard));
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_route_stay) {
        debug.dark_pop_route_stay = std::clamp(request.value, 0.0f, 600.0f);
        debug.status = std::format("Dark Pop sink length {} tick(s).", static_cast<unsigned>(debug.dark_pop_route_stay));
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop sink length {} tick(s)", static_cast<unsigned>(debug.dark_pop_route_stay));
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_enabled && !request.enabled) {
        debug.dark_pop = false;
        dark_pop_update(false, 0);
        // Saved off as well as on, like No Bail (3.1 saved only the on, so a saved "on" came back by itself).
        mark_debug_changed(debug);
        debug.status = "Dark Pop disabled.";
        logging::write(logging::Level::info, logging::Channel::skater, "Dark Pop disabled");
        return;
    }
    if (request.action == overlay::DebugAction::set_probe_enabled) {
        debug.probe = request.enabled;
        debug.status = request.enabled ? "Dark pop probe on: writing a new pair of files in logs, named by the time." : "Dark pop probe off.";
        return;
    }
    if (request.action == overlay::DebugAction::set_revert_boost_enabled) {
        debug.revert_boost = request.enabled;
        debug.revert_track = {};
        mark_debug_changed(debug);
        debug.status = request.enabled ? "Revert Boost on." : "Revert Boost off.";
        // In the log (which crash reports attach) so a crash can be matched to the mod being on.
        logging::log(logging::Level::info, logging::Channel::skater, "Revert Boost {}{}", request.enabled ? "enabled" : "disabled",
            dingosdk::reskate_modified_build ? std::format(" (modified build {} {})", dingosdk::reskate_mod_id, dingosdk::reskate_mod_version) : std::string());
        return;
    }
    if (request.action == overlay::DebugAction::set_revert_boost_speed) {
        source_require(std::isfinite(request.value) && request.value >= revert_boost_strength_min &&
            request.value <= revert_boost_strength_max, "Revert Boost strength must be between x0.5 and x100.");
        debug.revert_boost_speed = request.value;
        mark_debug_changed(debug);
        debug.status = "Revert Boost strength updated.";
        return;
    }
    if (request.action == overlay::DebugAction::restore_debug) {
        if (debug.save_pending) save_debug(debug);
        debug.first_person_settings = {};
        first_person_arm().settings = {};
        first_person_disarm();
        debug.park_editor = false;
        debug.flight_speed = 15.0f;
        debug.forward_velocity_speed = 0.0f;
        debug.forward_velocity.valid = false;
        debug.up_velocity_speed = 0.0f;
        debug.offboard_up_velocity_speed = 0.0f;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
        debug.no_bail = false;
        debug.dark_pop = false;
        dark_pop_update(false, 0);
        debug_stop_noclip(debug);
        clear_no_bail();
        // Level loads drop temporary modes, never the player's saved choices.
        load_saved_debug(debug);
        // Restore UI first even when the camera's native context is unavailable.
        std::string issue;
        try { debug_restore_ui(trial); }
        catch (const SourceGuard& guard) { issue = guard.message; }
        catch (...) { issue = "UI restoration failed."; }
        try {
            debug_restore_camera(trial, client, phase, error);
        } catch (const SourceGuard& guard) { if (!issue.empty()) issue += " "; issue += guard.message; }
        catch (...) { if (!issue.empty()) issue += " "; issue += "Camera restoration failed."; }
        if (!issue.empty()) { debug.status = issue; return; }
        debug.status = "Debug changes restored.";
        return;
    }
    if (request.action == overlay::DebugAction::set_noclip && !request.enabled) {
        debug_stop_noclip(debug);
        debug.status = debug.no_bail ? "Noclip disabled. Manual No Bail remains enabled." : "Noclip disabled. Normal skating resumed.";
        return;
    }
    if (request.action == overlay::DebugAction::set_no_bail && !request.enabled) {
        debug.no_bail = false;
        debug.no_bail_restore = false;
        mark_debug_changed(debug);
        debug.status = debug.noclip ? "No Bail manual toggle off; noclip still protects against wipeouts." : "No Bail disabled.";
        return;
    }
    source_require(can_control, "Wait for an active local level before changing debug settings.");
    if (request.action == overlay::DebugAction::add_forward_velocity || request.action == overlay::DebugAction::add_up_velocity || request.action == overlay::DebugAction::add_offboard_up_velocity) {
        source_require(session_boosts_allowed(), "The host has turned off boosts in this session.");
        const bool offboard_boost = request.action == overlay::DebugAction::add_offboard_up_velocity;
        const bool up = request.action == overlay::DebugAction::add_up_velocity || offboard_boost;
        source_require(!debug.noclip, "Disable Noclip before using velocity boosts.");
        source_require(source_state().velocity_guard_active.load(std::memory_order_acquire),
            "Velocity boosts are unavailable for this game build.");
        overlay::DebugModel skater;
        const auto transform = debug_skater(trial.base, client, skater);
        const auto bodies = debug_noclip_bodies(trial.base, client, skater.skater_identity);
        if (offboard_boost) {
            source_require(bodies.offboard || bodies.wipeout, "Off-board Up Boost requires walking, falling or gliding.");
        } else {
            source_require(!bodies.offboard, "Velocity boosts require the skater to be on the board.");
        }
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(skater.skater_identity, 0x7e0) == 0,
            "Wait for the current teleport before using velocity boosts.");
        const auto board_velocity = up ? std::array<float, 3>{} :
            reader.value<std::array<float, 3>>(bodies.parts[0], 0x70);
        reader.verify();
        const float speed = offboard_boost ? debug.offboard_up_velocity_speed : (up ? debug.up_velocity_speed : debug.forward_velocity_speed);
        source_require(speed > 0.0f, "This boost is 0 (off): set its speed in Skater > Cheats.");
        const auto velocity = velocity_boost_delta(transform, speed,
            up ? VelocityBoostDirection::up : VelocityBoostDirection::forward, board_velocity);
        source_require(velocity.has_value(), "Velocity boost direction or speed is invalid.");
        auto& boost = offboard_boost ? debug.offboard_up_velocity : (up ? debug.up_velocity : debug.forward_velocity);
        source_require(offboard_boost || !boost.valid, "Velocity boost is already queued.");
        boost = {client, skater.skater_identity, bodies.core, GetTickCount64() + 1000, *velocity, true};
        if (offboard_boost) source_state().offboard_boost_core.store(bodies.core, std::memory_order_release);
        if (offboard_boost) {
            // Give the player the impulse on this client tick. The motion hook
            // keeps its upward speed briefly if native off-board drive rewrites it.
            const float current_up = reader.value<float>(bodies.parts[9], 0x74);
            const float target_up = std::max(speed, current_up + speed);
            source_require(std::isfinite(target_up) && target_up <= 100000,
                "Off-board boost produced an invalid upward speed.");
            std::array<std::array<float, 3>, 32> velocities{};
            std::array<std::uint32_t, 32> flags{};
            for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
                velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
                flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
                velocities[i][1] = std::max(velocities[i][1], target_up);
            }
            reader.verify();
            if (bodies.offboard)
                source_require(ensure_offboard_up_velocity(trial.base, bodies.core, bodies.context,
                    bodies.rig_wrapper, target_up), "Off-board boost motion ownership changed.");
            for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
                debug_write(bodies.parts[i] + 0x70, velocities[i]);
                debug_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
            boost.velocity[1] = target_up;
            boost.applied = true;
            boost.expires = GetTickCount64() + 180;
            ++debug.offboard_up_velocity_updates;
            debug.status = "Off-board up velocity added.";
        } else debug.status = up ? "Up Boost queued." : "Forward Boost queued.";
        return;
    }
    if (request.action == overlay::DebugAction::set_dark_pop_enabled) {
        source_require(session_boosts_allowed(), "The host has turned off boosts in this session.");
        debug.dark_pop = true;
        mark_debug_changed(debug);
        debug.status = "Dark Pop enabled. Catch a flip dark, press D-pad Left, Down or Right in the air, land it dark.";
        logging::log(logging::Level::info, logging::Channel::skater, "Dark Pop enabled{}",
            dingosdk::reskate_modified_build ? std::format(" (modified build {} {})", dingosdk::reskate_mod_id, dingosdk::reskate_mod_version) : std::string());
        return;
    }
    if (request.action == overlay::DebugAction::set_no_bail) {
        source_require(session_no_bail_allowed(), "The host has turned off No Bail in this session.");
        overlay::DebugModel skater;
        (void)debug_skater(trial.base, client, skater);
        source_require(update_no_bail(client, skater.skater_identity, true, debug.noclip && debug.noclip_velocity.valid,
            debug.noclip_velocity.expires), "No Bail is unavailable for the current skater.");
        debug.no_bail = true;
        mark_debug_changed(debug);
        debug.status = "No Bail enabled. Prevents new wipeouts; recover first if already bailed.";
        return;
    }
    if (request.action == overlay::DebugAction::set_game_ui_hidden) {
        const auto ui = debug_ui(trial.base);
        const auto desired = static_cast<std::uint8_t>(request.enabled ? 0 : 1);
        if (debug.ui_owned) source_require(ui.object == debug.ui_object, "UI settings owner changed.");
        if (ui.draw != desired) {
            if (!debug.ui_owned) { debug.ui_original = ui.draw; debug.ui_object = ui.object; }
            debug.ui_owned = true; debug.ui_applied = desired;
            debug_write(ui.object + 0x4e, desired);
        }
        if (debug.ui_owned && desired == debug.ui_original) debug.ui_owned = false;
        debug.status = request.enabled ? "Game UI hidden. " + launcher::key_name(launcher::overlay_keys().menu) +
            " still opens this overlay." : "Game UI visible.";
        return;
    }
    source_require(phase, "Waiting for the native camera update phase.");
    source_require(!debug.camera_ambiguous, "Native camera result is uncertain; restart before changing camera mode.");
    if (request.action == overlay::DebugAction::set_noclip) {
        if (debug.noclip) return;
        source_require(session_noclip_allowed(), "The host has turned off noclip in this session.");
        debug.forward_velocity.valid = false;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
        source_require(no_bail_available(), "Noclip requires the native No Bail hooks.");
        source_require(source_state().velocity_guard_active.load(std::memory_order_acquire),
            "Velocity flight is unavailable for this game build.");
        overlay::DebugModel skater;
        (void)debug_skater(trial.base, client, skater);
        (void)debug_noclip_bodies(trial.base, client, skater.skater_identity);
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(skater.skater_identity, 0x7e0) == 0,
            "Wait for the current teleport before enabling flight.");
        reader.verify();
        if (debug.camera_owned) debug_restore_camera(trial, client, phase, error);
        const auto camera = source_camera_snapshot(trial, client);
        source_require(camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "Noclip needs the normal player camera.");
        (void)debug_view_matrix(trial, camera);
        debug.camera_identity = camera.identity;
        debug.noclip_entity = skater.skater_identity;
        debug.flight_time = source_flight_now();
        debug.noclip_velocity_updates = debug.noclip_motion_updates = 0;
        debug.noclip_altitude_valid = false;
        debug.noclip = true;
        debug.status = "Flight enabled with No Bail. Left stick moves; RT / LT raise / lower; L3 boosts.";
        return;
    }
    auto camera = source_camera_snapshot(trial, client);
    if (request.action == overlay::DebugAction::set_first_person_fov) {
        source_require(std::isfinite(request.value) && request.value >= first_person_fov_min &&
            request.value <= first_person_fov_max, "First-person FOV must be between 40 and 120 degrees.");
        debug.first_person_fov = request.value;
        first_person_arm().fov = request.value;
        mark_debug_changed(debug);
        if (debug.first_person) (void)first_person_write_fov(camera.identity.camera, request.value);
        debug.status = "First-person FOV updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_first_person) {
        if (!request.enabled) {
            if (!debug.first_person) return;
            debug_restore_camera(trial, client, phase, error); // Also ends first person.
            debug.status = "First person off. Original camera restored.";
            return;
        }
        if (debug.first_person) return;
        // Resolve the head against the live camera matrix before taking the
        // camera, so an unavailable skeleton never leaves an orphaned mode 1.
        auto pose = debug_view_matrix(trial, camera);
        const auto origin = first_person_head_matrix(trial.base, first_person_component(trial.base, client), pose);
        {
            // Optional: without the animation hook the view trails by one step.
            std::string hook_detail;
            if (multiplayer::install_entity_hooks(trial.base, hook_detail)) {
                multiplayer::set_animation_evaluated_listener(&first_person_on_animation);
                multiplayer::set_render_pose_listener(&first_person_on_render);
            } else
                dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::skater,
                    "First person will trail the head by one animation step: {}", hook_detail);
        }
        // Take mode 1 through the verified Freecam path, then drive it from the head.
        if (!debug.camera_owned)
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_free_camera, true}, error);
        camera = source_camera_snapshot(trial, client);
        source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
            camera.active == camera.identity.camera, "First person could not take the camera.");
        debug.first_person = true;
        debug.drone_follow = false;
        debug.first_person_waiting = false;
        {
            float saved{};
            debug.first_person_saved_fov = first_person_read(camera.identity.camera + camera_fov_offset, &saved, 4) &&
                std::isfinite(saved) && saved > 1.0f && saved < 175.0f ? saved : 0.0f;
        }
        if (debug.first_person_fov > 0) (void)first_person_write_fov(camera.identity.camera, debug.first_person_fov);
        auto& arm = first_person_arm();
        arm.settings = debug.first_person_settings;
        arm.spring.reset();
        arm.spring.begin(source_flight_now());
        pose = arm.spring.sample(pose, arm.settings, origin);
        source_require(valid_flight_transform(pose), "First-person arm pose is invalid.");
        debug.flight_ready = false;
        SetLastError(error);
        trial.camera_transform(camera.identity.camera, &pose);
        debug.flight_matrix = pose;
        debug.flight_time = source_flight_now();
        debug.flight_ready = true;
        debug.status = "First person on. The camera follows the skater's head.";
        return;
    }
    if (request.action == overlay::DebugAction::set_drone_follow) {
        if (!request.enabled) {
            if (!debug.drone_follow) return;
            debug_restore_camera(trial, client, phase, error); // Also ends drone follow.
            debug.status = "Drone follow off. Original camera restored.";
            return;
        }
        if (debug.drone_follow) return;
        // The skater to follow comes first, so nothing is taken without one.
        const auto target = follow_target(trial, client);
        source_require(target.has_value(), "Drone follow needs the local skater.");
        // Take mode 1 through the verified Freecam path, as first person does, then fly it.
        if (!debug.camera_owned)
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_free_camera, true}, error);
        camera = source_camera_snapshot(trial, client);
        source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
            camera.active == camera.identity.camera, "Drone follow could not take the camera.");
        if (debug.first_person) {
            first_person_disarm();
            first_person_restore_fov(debug);
            debug.first_person = false;
        }
        // It takes off from its spot behind the skater, facing them.
        debug.follow_pilot = {};
        drone::detail::Vec3 heading{target->second[0], 0.0f, target->second[2]};
        heading = drone::detail::length(heading) > 0.1f ? drone::detail::normalize(heading) : drone::detail::Vec3{0, 0, -1};
        const auto tuning = follow_tuning();
        const auto spot = drone::follow::spot(target->first, {0, 0, 0}, heading, tuning, 0.0f);
        auto pose = drone::follow::pose_facing(spot, target->first);
        source_require(valid_flight_transform(pose), "Drone follow pose is invalid.");
        debug.flight_ready = false;
        SetLastError(error);
        trial.camera_transform(camera.identity.camera, &pose);
        debug.flight_matrix = pose;
        debug.flight_time = source_flight_now();
        debug.drone_state.valid = false;
        debug.drone_follow = true;
        debug.flight_ready = true;
        debug.status = "Drone follow on. The drone flies itself behind you; skate as normal.";
        return;
    }
    if (request.action == overlay::DebugAction::set_free_camera) {
        debug_stop_noclip(debug);
        if (!request.enabled) {
            source_require(!camera.mode || debug.camera_owned, "This free camera is owned by another experiment.");
            debug_restore_camera(trial, client, phase, error);
            debug.status = "Original camera restored.";
            return;
        }
        if (debug.camera_owned) {
            source_require(camera.identity == debug.camera_identity && camera.mode == 1,
                "The owned free camera changed.");
            if (debug.first_person) {
                // Keep the owned camera and fly on from the last head pose.
                first_person_disarm();
                first_person_restore_fov(debug);
                debug.first_person = false;
                debug.status = "Freecam enabled from the first-person view.";
            }
            if (debug.drone_follow) {
                // Keep the owned camera and fly on from where the drone was.
                debug.drone_follow = false;
                debug.status = "Freecam enabled from drone follow.";
            }
            return;
        }
        source_require(camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "An active local view with no other camera override is required.");
        const auto pose = debug_view_matrix(trial, camera);
        debug.camera_identity = camera.identity;
        debug.camera_owned = true; debug.camera_ambiguous = true;
        SetLastError(error);
        trial.camera_mode(camera.identity.controller, 1);
        debug.camera_ambiguous = false;
        camera = source_camera_snapshot(trial, client);
        source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
            camera.active == camera.identity.camera && camera.input_active,
            "Freecam activation could not be verified; disable Freecam to restore the view.");
        SetLastError(error);
        trial.camera_transform(camera.identity.camera, &pose);
        debug.flight_matrix = pose;
        debug.flight_time = source_flight_now();
        debug.flight_ready = true;
        debug.drone_state.valid = false; // a fresh Freecam starts the drone at rest
        debug.status = drone::is_enabled() ? "Freecam enabled in drone mode. WASD pitch/roll, Q E throttle, hold RMB + mouse to yaw."
            : "Freecam enabled. WASD / Q E move; hold RMB to look; Shift boosts.";
        return;
    }
    source_require(false, "Unknown debug request.");
}
// "Third person on foot": runs only while first person is on or this paused
// it, so players who never use first person are untouched. Switches through
// the same actions as the menu toggle, after the state has held briefly.
void first_person_board_tick(SourceTrial& trial, std::uintptr_t client, bool can_control, bool phase, DWORD error) {
    auto& debug = trial.debug;
    // Choosing Freecam or the Park Editor while handed back ends the hand-back.
    if (debug.first_person_paused && (debug.park_editor || (debug.camera_owned && !debug.first_person)))
        debug.first_person_paused = false;
    // With the option off, a hand-back still in progress resumes below.
    if (!debug.first_person_paused && !(debug.first_person && debug.first_person_settings.board_only)) return;
    if (!can_control || !phase || debug.park_editor || debug.noclip || debug.editor_transition) return;
    const auto now = GetTickCount64();
    const auto on_foot = debug.first_person_settings.board_only ? first_person_on_foot(trial.base, client) : std::optional<bool>(false);
    if (!on_foot) return;
    if (*on_foot != debug.first_person_last_on_foot) {
        debug.first_person_last_on_foot = *on_foot;
        debug.first_person_foot_since = now;
    }
    const auto held = now - debug.first_person_foot_since;
    if (now < debug.first_person_retry_after) return;
    try {
        if (debug.first_person && *on_foot && held >= 250) {
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_first_person, false}, error);
            debug.first_person_paused = true;
            debug.status = "On foot: third person until you are back on the board.";
        } else if (debug.first_person_paused && !*on_foot && held >= 120) {
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_first_person, true}, error);
            debug.first_person_paused = !debug.first_person;
        }
    } catch (const SourceGuard& guard) {
        debug.status = guard.message;
        debug.first_person_retry_after = now + 500;
    } catch (...) {
        debug.first_person_retry_after = now + 500;
    }
}
// "First person in the camera cycle" (Engine/Game/Skater/first_person_cycle.h). The game writes
// its camera height option on the thread that handles the button, so each tick publishes what
// that write needs, and the next tick carries out the step it asked for.
namespace {
enum CycleFlag : std::uint8_t { cycle_enabled = 1, cycle_first_person = 2, cycle_can_enter = 4 };
std::atomic<std::uint8_t> cycle_flags{}, cycle_request{};
// When the flags were published: a tick that stopped running (loading, no local skater) leaves
// the game's own height alone.
std::atomic<ULONGLONG> cycle_published{};
constexpr ULONGLONG cycle_flags_lifetime_ms = 250;
}
bool first_person_camera_height_write(bool high) noexcept {
    const auto published = cycle_published.load(std::memory_order_acquire);
    const auto flags = cycle_flags.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    if (now < published || now - published > cycle_flags_lifetime_ms) return high;
    const auto write = first_person::cycle_height_write(
        {(flags & cycle_enabled) != 0, (flags & cycle_first_person) != 0, (flags & cycle_can_enter) != 0}, high);
    if (write.step != first_person::CycleStep::none)
        cycle_request.store(static_cast<std::uint8_t>(write.step), std::memory_order_release);
    return write.high;
}
void first_person_cycle_tick(SourceTrial& trial, std::uintptr_t client, bool can_control, bool phase, DWORD error) {
    auto& debug = trial.debug;
    const auto now = GetTickCount64();
    const bool enabled = debug.first_person_settings.camera_cycle && can_control && phase &&
        !debug.park_editor && !debug.noclip && !debug.editor_transition;
    const auto step = static_cast<first_person::CycleStep>(cycle_request.exchange(0, std::memory_order_acq_rel));
    if (enabled && step != first_person::CycleStep::none) {
        // The player's own First person choice, so it replaces an on-foot hand-back.
        debug.first_person_paused = false;
        try {
            debug_action(trial, client, can_control, phase,
                {overlay::DebugAction::set_first_person, step == first_person::CycleStep::enter}, error);
        } catch (const SourceGuard& guard) {
            debug.status = guard.message;
            // The press kept the low camera; the next ones switch height as the game does.
            debug.first_person_cycle_retry_after = now + 2000;
        } catch (...) {
            debug.first_person_cycle_retry_after = now + 2000;
        }
    }
    bool can_enter = false;
    if (enabled && !debug.first_person && !debug.camera_owned && now >= debug.first_person_cycle_retry_after) {
        const auto on_foot = first_person_on_foot(trial.base, client);
        can_enter = on_foot && !*on_foot;
    }
    cycle_flags.store(static_cast<std::uint8_t>((enabled ? cycle_enabled : 0) | (debug.first_person ? cycle_first_person : 0) |
        (can_enter ? cycle_can_enter : 0)), std::memory_order_release);
    cycle_published.store(now, std::memory_order_release);
}
}
}

namespace dingosdk {
using namespace client_source::detail;

overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client, bool can_control,
    bool camera_phase_observed, const overlay::DebugRequest* request, const overlay::FlightInput* flight_input) {
    SourceLastError error;
    overlay::DebugModel model;
    auto& state = source_state();
    model.free_camera = state.free_camera_active.load(std::memory_order_acquire);
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return model;
    SourceBusyScope scope{state.busy};
    auto& trial = state.trial;
    auto& debug = trial.debug;
    if (base != trial.base || (debug.thread && debug.thread != GetCurrentThreadId())) return model;
    debug.thread = GetCurrentThreadId();
    if (!debug.saved_loaded) {
        debug.saved_loaded = true;
        load_saved_debug(debug);
        profile_runtime::set_camera_height_write_listener(&first_person_camera_height_write);
    }
    if (debug.save_pending && GetTickCount64() >= debug.save_due) save_debug(debug);
    if (debug.no_bail_restore && can_control && !debug.park_editor && GetTickCount64() >= debug.no_bail_restore_after) {
        debug.no_bail_restore = false;
        debug.no_bail = true; // the tick's update_no_bail applies it to the current skater
    }
    if (!can_control) {
        debug.forward_velocity.valid = false;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
    }
    if (debug.park_editor && !lobby_object_placement_allowed() && can_control && camera_phase_observed) {
        try {
            debug_action(trial, client, can_control, camera_phase_observed,
                {overlay::DebugAction::set_park_editor, false}, error.value);
            debug.status = "The host disabled object placement. Previous camera and UI settings restored.";
        } catch (const SourceGuard& guard) { debug.status = guard.message; }
        catch (...) { debug.status = "Restoring camera and UI after the host disabled object placement."; }
    }
    // The host took noclip away (joining, or changed mid-session): land now. A disallowed No
    // Bail is only held back below, so the player's own saved choice comes back after the session.
    if (debug.noclip && !session_noclip_allowed()) {
        debug_stop_noclip(debug);
        debug.status = "The host turned off noclip in this session.";
    }
    const bool no_bail_allowed = session_no_bail_allowed();
    if (request) {
        // The player's own First person choice replaces any on-foot hand-back.
        if (request->action == overlay::DebugAction::set_first_person || request->action == overlay::DebugAction::restore_debug)
            debug.first_person_paused = false;
        try { debug_action(trial, client, can_control, camera_phase_observed, *request, error.value); }
        catch (const SourceGuard& guard) { debug.status = guard.message; }
        catch (...) { debug.status = "Debug action failed; inspect the current state before retrying."; }
    }
    first_person_board_tick(trial, client, can_control, camera_phase_observed, error.value);
    first_person_cycle_tick(trial, client, can_control, camera_phase_observed, error.value);
    // Camera ownership survives a busy tick or a failed presentation snapshot.
    // Input capture must follow the lease, not those transient model failures.
    // (First person and drone follow own the same camera but leave the skater its controls.)
    state.free_camera_active.store(debug.camera_owned && !debug.first_person && !debug.drone_follow, std::memory_order_release);
    model.free_camera = debug.camera_owned && !debug.first_person && !debug.drone_follow;
    model.drone_follow = debug.camera_owned && debug.drone_follow;
    std::optional<SourceCameraSnapshot> flight_camera;
    try { debug_flight_tick(trial, client, can_control, camera_phase_observed, flight_input, error.value, flight_camera); }
    catch (const SourceGuard& guard) { debug_stop_noclip(debug); debug.status = guard.message; }
    catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a native error. Disable Freecam if its view is still active."; }
    model.available = can_control || debug_has_lease(debug);
    model.park_editor = debug.park_editor;
    model.settings_owned = debug_has_lease(debug);
    model.status = debug.status;
    model.first_person_arm = debug.first_person_settings;
    try {
        const auto ui = debug_ui(base);
        model.ui_available = can_control;
        model.game_ui_hidden = !ui.draw;
    } catch (...) {}
    try {
        const auto camera = flight_camera ? *flight_camera : source_camera_snapshot(trial, client);
        const bool owned_view = camera.mode == 1 && camera.active == camera.identity.camera;
        model.free_camera = owned_view && !debug.first_person && !debug.drone_follow;
        model.drone_follow = owned_view && debug.drone_follow;
        // Handed back on foot still reads as on, so the toggle can switch it off.
        model.first_person = (owned_view && debug.first_person) || debug.first_person_paused;
        model.first_person_fov = debug.first_person_fov;
        model.free_camera_fov = debug.free_camera_fov;
        model.camera_available = can_control && camera_phase_observed && !debug.camera_ambiguous;
        model.camera_unavailable = model.camera_available ? "" : !can_control ? "Waiting for active local controls." :
            !camera_phase_observed ? "Waiting for a verified local camera update." :
            "Camera mode could not be verified; restart to use flight.";
        SourceReader reader;
        model.camera_speed = debug.flight_speed;
        const auto vtable = reader.pointer(camera.active);
        if (vtable == base + spawn::free_camera_vtable || vtable == base + addr::engine::camera_vtable) {
            model.camera_position = debug_position(reader, camera.active + 0x80);
            model.camera_position_valid = true;
            if (debug.park_editor) {
                model.camera_transform = debug_view_matrix(trial, camera);
                // Camera's degree FOV at +ac, initialized by spawn::camera_fov_init.
                model.camera_fov = reader.value<float>(camera.active, 0xac);
                model.camera_transform_valid = std::isfinite(model.camera_fov) && model.camera_fov > 1 && model.camera_fov < 175;
            }
        }
        reader.verify();
    } catch (const SourceGuard& guard) {
        model.camera_position_valid = false;
        model.camera_transform_valid = false;
        if (!model.camera_available) model.camera_unavailable = guard.message;
    } catch (...) {
        model.camera_position_valid = false;
        model.camera_transform_valid = false;
        if (!model.camera_available) model.camera_unavailable = "Local camera could not be read.";
    }
    model.noclip_unavailable = model.camera_unavailable;
    model.forward_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Forward Boost." :
        debug.noclip ? "Disable Noclip before using Forward Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Forward Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    model.up_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Up Boost." :
        debug.noclip ? "Disable Noclip before using Up Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Up Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    model.offboard_up_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Off-board Up Boost." :
        debug.noclip ? "Disable Noclip before using Off-board Up Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Off-board Up Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    try {
        const auto skater_transform = debug_skater(base, client, model);
        const auto bodies = debug_noclip_bodies(base, client, model.skater_identity);
        if (debug.forward_velocity.valid && (GetTickCount64() >= debug.forward_velocity.expires ||
            debug.forward_velocity.entity != model.skater_identity || debug.forward_velocity.core != bodies.core)) {
            debug.forward_velocity.valid = false;
            debug.status = "Forward Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip && !debug.forward_velocity.valid &&
            state.velocity_guard_active.load(std::memory_order_acquire) && !bodies.offboard) {
            model.forward_velocity_available = true;
            model.forward_velocity_unavailable.clear();
        } else if (bodies.offboard) {
            model.forward_velocity_unavailable = "Forward Boost requires the skater to be on the board.";
        } else if (debug.forward_velocity.valid) {
            model.forward_velocity_unavailable = "Forward Boost is being applied.";
        }
        if (debug.up_velocity.valid && (GetTickCount64() >= debug.up_velocity.expires ||
            debug.up_velocity.entity != model.skater_identity || debug.up_velocity.core != bodies.core)) {
            debug.up_velocity.valid = false;
            debug.status = "Up Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip && !debug.up_velocity.valid &&
            state.velocity_guard_active.load(std::memory_order_acquire) && !bodies.offboard) {
            model.up_velocity_available = true;
            model.up_velocity_unavailable.clear();
        } else if (bodies.offboard) {
            model.up_velocity_unavailable = "Up Boost requires the skater to be on the board.";
        } else if (debug.up_velocity.valid) {
            model.up_velocity_unavailable = "Up Boost is being applied.";
        }
        if (debug.offboard_up_velocity.valid && (GetTickCount64() >= debug.offboard_up_velocity.expires ||
            debug.offboard_up_velocity.entity != model.skater_identity || debug.offboard_up_velocity.core != bodies.core ||
            (!debug.offboard_up_velocity.applied && !bodies.offboard && !bodies.wipeout))) {
            const bool applied = debug.offboard_up_velocity.applied;
            debug.offboard_up_velocity.valid = false;
            if (!applied) debug.status = "Off-board Up Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip &&
            state.velocity_guard_active.load(std::memory_order_acquire) && (bodies.offboard || bodies.wipeout)) {
            model.offboard_up_velocity_available = true;
            model.offboard_up_velocity_unavailable.clear();
        } else if (!bodies.offboard && !bodies.wipeout) {
            model.offboard_up_velocity_unavailable = "Off-board Up Boost requires walking, falling or gliding.";
        } else if (debug.offboard_up_velocity.valid) {
            model.offboard_up_velocity_unavailable = "Off-board Up Boost is being applied.";
        }
        try { revert_boost_tick(debug, client, model.skater_identity, bodies, can_control); }
        catch (...) { debug.revert_track = {}; }
        try { dark_pop_tick(debug, bodies, can_control); }
        catch (...) {}
        try { probe_tick(debug, skater_transform, bodies, can_control); }
        catch (...) {}
        model.no_bail_available = can_control && update_no_bail(client, model.skater_identity, debug.no_bail && no_bail_allowed,
            debug.noclip && debug.noclip_velocity.valid, debug.noclip_velocity.expires);
        if (model.camera_available) {
            if (!model.no_bail_available) model.noclip_unavailable = "Waiting for local No Bail protection.";
            else if (!state.velocity_guard_active.load(std::memory_order_acquire))
                model.noclip_unavailable = "Noclip motion hooks are unavailable for this launch.";
            else {
                // `bodies` above already verified the skater's physics this tick.
                model.noclip_available = true;
                model.noclip_unavailable.clear();
            }
        }
    } catch (const SourceGuard& guard) {
        if (model.camera_available) model.noclip_unavailable = guard.message;
        model.forward_velocity_unavailable = guard.message;
        model.up_velocity_unavailable = guard.message;
        model.offboard_up_velocity_unavailable = guard.message;
    } catch (...) {
        if (model.camera_available) model.noclip_unavailable = "Local skater physics could not be read.";
        model.forward_velocity_unavailable = "Local skater physics could not be read.";
        model.up_velocity_unavailable = "Local skater physics could not be read.";
        model.offboard_up_velocity_unavailable = model.up_velocity_unavailable;
    }
    if (!session_boosts_allowed()) {
        model.forward_velocity_available = model.up_velocity_available = model.offboard_up_velocity_available = false;
        model.forward_velocity_unavailable = model.up_velocity_unavailable = model.offboard_up_velocity_unavailable = "The host has turned off boosts in this session.";
    }
    if (!model.no_bail_available) {
        clear_no_bail();
        if (debug.noclip) {
            debug_stop_noclip(debug);
            debug.status = "Flight stopped: local wipeout protection is unavailable.";
        }
    }
    if (!session_noclip_allowed()) {
        model.noclip_available = false;
        model.noclip_unavailable = "The host has turned off noclip in this session.";
    }
    // Noclip's own flight protection stays; only the manual toggle is the host's to refuse.
    if (!no_bail_allowed) model.no_bail_available = false;
    model.no_bail = debug.no_bail && no_bail_allowed;
    model.no_bail_active = model.no_bail_available && ((debug.no_bail && no_bail_allowed) ||
        (debug.noclip && debug.noclip_velocity.valid && GetTickCount64() < debug.noclip_velocity.expires));
    model.settings_owned = debug_has_lease(debug);
    model.status = debug.status;
    model.noclip = debug.noclip;
    model.noclip_velocity_updates = debug.noclip_velocity_updates;
    model.noclip_motion_updates = debug.noclip_motion_updates;
    model.forward_velocity_speed = debug.forward_velocity_speed;
    model.forward_velocity_updates = debug.forward_velocity_updates;
    model.up_velocity_speed = debug.up_velocity_speed;
    model.up_velocity_updates = debug.up_velocity_updates;
    model.offboard_up_velocity_speed = debug.offboard_up_velocity_speed;
    model.offboard_up_velocity_updates = debug.offboard_up_velocity_updates;
    state.offboard_boost_core.store(debug.offboard_up_velocity.valid ? debug.offboard_up_velocity.core : 0,
        std::memory_order_release);
    model.revert_boost = debug.revert_boost;
    model.revert_boost_speed = debug.revert_boost_speed;
    model.revert_boost_count = debug.revert_boost_count;
    model.dark_pop = debug.dark_pop;
    model.dark_pop_unpatch = debug.dark_pop_unpatch;
    model.dark_pop_hold = debug.dark_pop_hold;
    model.dark_pop_one_press = debug.dark_pop_one_press;
    model.dark_pop_route_stay = debug.dark_pop_route_stay;
    model.dark_pop_route_guard = debug.dark_pop_route_guard;
    model.dark_pop_keep_speed = debug.dark_pop_keep_speed;
    model.dark_pop_catch = debug.dark_pop_catch;
    model.dark_pop_feet = debug.dark_pop_feet;
    model.dark_pop_adaptive = debug.dark_pop_adaptive;
    model.dark_pop_timeout = debug.dark_pop_timeout;
    model.dark_pop_strength = debug.dark_pop_strength;
    model.dark_pop_hold_landing = debug.dark_pop_hold_landing;
    model.dark_pop_dark_exit = debug.dark_pop_dark_exit;
    model.dark_pop_full_flight = debug.dark_pop_full_flight;
    model.dark_pop_hold_flight = debug.dark_pop_hold_flight;
    model.dark_pop_stay_physical = debug.dark_pop_stay_physical;
    model.dark_pop_early_physics = debug.dark_pop_early_physics;
    model.dark_pop_reaim = debug.dark_pop_reaim;
    model.dark_pop_hide_dpad = debug.dark_pop_hide_dpad;
    model.dark_pop_count = debug.dark_pop_count;
    model.probe = debug.probe;
    return model;
}
bool client_free_camera_active() noexcept {
    return source_state().free_camera_active.load(std::memory_order_acquire);
}
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed) {
    const overlay::DebugRequest request{overlay::DebugAction::restore_debug};
    const auto model = on_client_debug_tick(base, client, false, camera_phase_observed, &request);
    return model.status == "Debug changes restored." && !model.settings_owned;
}
}
