#include "trainer.h"
#include "trainer_classes.h"
#include "trainer_gamestate.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "trainer_landing.h"
#include "trainer_motion.h"
#include "trainer_waypoint.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/Skater/skater_push.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Extension/Skater/skater_observer.h"
#include "Extension/UI/Overlay/overlay.h"
#include <mutex>
#include <thread>
#include <windows.h>
#include <shlobj.h>
#include <xinput.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <optional>


// Game thread only. See trainer.h for how the menu reaches this.
extern "C" std::uint64_t DingoSDKOverlayGrindStickMirrored() noexcept;
extern "C" void DingoSDKOverlaySetGrindStickTurn(bool on, float x, float y_add, float y_scale) noexcept;
extern "C" void DingoSDKOverlayGrindStickSent(float *values) noexcept;
extern "C" void DingoSDKOverlaySetGrindStickLean(bool on, float x, float y) noexcept;
extern "C" bool DingoSDKOverlayGrindStickLeanRaw(float *values) noexcept;
extern "C" std::uint64_t DingoSDKOverlayGrindStickLeaned() noexcept;
namespace dingosdk::trainer {
namespace {
namespace tuning = dingosdk::physics_tuning;
// A float the game holds at `at`, or false where that memory is gone (diagnostics only).
bool peek_float(std::uintptr_t at, float &value) noexcept {
    __try {
        value = *reinterpret_cast<const float *>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
namespace kit = dingosdk::throwdowns::kit;
// A plugin match blocks markers and returns, unless it lets teleports through (its own).
bool match_blocks_markers() noexcept {
    const auto m = kit::match();
    return m.markers && !m.teleports;
}
using Vec3 = std::array<float, 3>;
constexpr float max_coordinate = 1.0e6f;
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t xinput_up = 0x1, xinput_down = 0x2, xinput_left = 0x4, xinput_right = 0x8,
                        xinput_lb = 0x100, xinput_rb = 0x200;

struct Entry {
    std::string id, key, label, group; // key: lower-case id
    Kind kind{};
    std::uint16_t offset{}, size{};
    std::size_t curve{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{};
    bool used{}; // the game's code was found to read it (trainer_used.inc), or it is linked
    std::vector<std::size_t> drives; // graph multipliers this value scales (value_links)
    std::vector<std::size_t> members; // graph: its outputs (Y0.., Min_Y, Max_Y)
    int class_field{-1};              // a field of one of the game's data-defined classes (trainer_classes.h), not of the tuning asset
};
struct MapData {
    std::array<Marker, marker_slots> markers{};
    std::string preset;
};
struct MapFile { // Mods/<mod>/trainer.json
    std::string note, preset_name;
    std::vector<std::pair<std::string, double>> preset;
    std::vector<Spot> spots;
};
struct Saved {
    double value{};
    bool frozen{};
};
struct SelfTest {
    int step{}; // 0: idle
    std::uint64_t until{};
    std::size_t entry{};
    double before{}, target{};
    Vec3 origin{};
    int failures{};
};
struct Motion {
    bool valid{};
    Vec3 position{}, velocity{};
    std::int64_t stamp{};
    bool airborne{};
    Vec3 takeoff{}, takeoff_velocity{};
    std::int64_t takeoff_stamp{};
    float apex{};
    bool axes_valid{};
    Vec3 forward{}, up{};
    float spin{}, spin_rate{}, flip{};
    float board_turn{}; // the board's fastest turn rate in this jump, degrees per second
    std::uint32_t air_state{}, ground_state{};
};
// The trick multipliers and auto push in force: x of the game's own, and for auto push the
// speed a rolling skater is carried up to (m/s, 0: off).
struct Boosts {
    float flip{1}, hippy{1}, nocomply{1}, boneless{1}, offboard{1}, cruise{};
};
// How long a new skater alone waits for a memory search: doubled while searches find nothing new.
constexpr std::uint64_t class_skater_wait_low = 20000, class_skater_wait_high = 600000;
struct State {
    bool loaded{}; // the store was read
    std::shared_ptr<const tuning::Model> model;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t, std::less<>> index;
    std::uintptr_t base{}, asset{}, entity{};
    tuning::Values own, want;
    bool write_due{}, view_due{true}, save_due{};
    bool profile_due{}; // the loaded map's preset is still to be applied
    bool refresh_due{}; // values were written while the skater had no cached copy to refresh
    std::uint64_t next_hold{}, next_save{}, next_skater{}, revision{};
    std::string status = "Waiting for the game.", last;
    bool ready{};
    std::map<std::string, Saved, std::less<>> saved;                                    // by key
    // Plugins' values (plugin_tune), by key: whose, and what was there before.
    struct PluginTune {
        std::string owner;
        double before{};
    };
    std::map<std::string, PluginTune, std::less<>> plugin_tunes;
    UserPresets user;                                                                   // presets
    std::vector<std::string> active;                                                    // presets applied since Stock
    std::map<std::string, MapData, std::less<>> maps;
    std::string map;
    MapFile map_file;
    int slot{};
    bool auto_return{}, pad_shortcuts{}, hud{}, hud_jump{}, logging{};
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    float underflip_speed{1}; // the board while the game says an underflip is turning, times the flip speed
    // Off: the whole underflip (its flip too) turns at Underflip speed. On: the flip turns at Flip trick speed and only
    // the under part, from where the board turns back, at Underflip speed.
    bool underflip_split{};
    std::array<float, 3> board_spin{}; // the board's angular velocity at the last tick in the air, deg/s
    // "Advanced trick speed": each flip trick also turns at a speed of its own (flip_tricks, trainer_gamestate.h).
    bool flip_advanced{};
    bool flip_pop{}; // a slowed flip trick pops higher, so it has the air time it needs
    bool flip_flick{}; // a slow flick of the stick turns the board as fast as a full one
    bool flip_limit{true}; // the game's flip landing limit: a flip still turning near the landing is hurried round
    bool air_reach_fixed{}; // the turned-up reach from the air that Sticky Grinds saved before 2026-10-24 is cleared (once)
    bool grind_watch{}; // the grind card is asking what the game's switches read (`trainer grind watch`)
    std::map<std::string, bool, std::less<>> folds; // the menu's sections and cards the player has opened or closed
    std::array<float, flip_tricks.size()> flip_trick = [] {
        std::array<float, flip_tricks.size()> ones{};
        ones.fill(1.0f);
        return ones;
    }();
    // The flip speed in the air now: the game's own number, what the trainer made of it, and whether it can be reached at all.
    struct FlipDrive {
        bool active{};
        float game{}, written{};
        float clamped{}; // the impossible's own speed state as the flight began
        float factor{1}; // the flip speed times the turning trick's own
        std::uint32_t trick{}; // the trick it was last driven for
        std::int8_t underflip_anim{-1}, underflip_state{-1}, underflip_window{-1}; // as last logged
        // UnderflipTimeScale, which the underflip clips play at: the game's own number and what the trainer made of it.
        bool timescale_active{};
        bool window_closed{}; // the flick's underflip window opened and closed in this flight: an underflip may start
        float timescale_game{}, timescale_written{};
        int timescale_sets{}; // how often the game set its own number this flight (logged up to 8)
        // "Underflip part only": the way the board turned in the underflip's flip (summed while it turned), how many
        // ticks went into it, and how many ticks in a row it has turned back against it. Two back and the under part
        // has started.
        float flip_way[3]{};
        int flip_way_ticks{}, turned_back{};
        bool under_part{};
        int underflip_ticks{}; // ticks since the underflip started (for the log)
    } flip_drive;
    int underflip_bands{}; // copies of the underflip's rate controllers found
    float flip_game_seen{}; // the game's own number in the last driven flight (for the flight log)
    float flip_clamped_seen{-1}; // and what the impossible's own speed state read (-1: not found)
    bool flip_live{};
    std::uint32_t flip_trick_seen{}; // the last trick that turned (CurrentFlipTrick)
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    float manual_height{1}; // the pop out of a manual
    float gravity{1};       // x the game's own gravity, in the air on the board
    bool gravity_live{true}; // this build's gravity constants are the known ones
    // Advanced gravity: the gravity the game plans landings with (own off: the same as gravity), and how far the air
    // animations keep the timing they have at the game's gravity (0 the game's, 1 all the way).
    bool gravity_landing_own{};
    float gravity_landing{1}, gravity_timing{1};
    float grind_side{1};    // the sideways part of a pop out of a grind or slide
    float grind_push{};     // Skate 3's pop: this many m/s sideways at full stick, the way the stick points (0: none)
    // Skate 3's turn on a grind: the left stick alone turns the board, up to this many degrees either way (0: off, the
    // game's own, which follows where the right stick points). Negative turns the other way.
    float grind_range{};
    float grind_yaw{};          // Skate 3's eased stick, -0.614..0.614
    float grind_heading{};      // what the game is answered with (radians)
    // The angle the game turns the right stick by before it steers the board (it reads the stick from the camera's side):
    // measured from what it was shown against what its GrindControlLR / UD then read.
    float grind_frame_cos{1}, grind_frame_sin{};
    std::uint64_t grind_yaw_at{};
    // The grind exit switches (grind_switches, trainer_gamestate.h): -1 the game's own, 0 held off, 1 held on; and what each read.
    std::array<std::int8_t, grind_switches.size()> grind_mode = [] { std::array<std::int8_t, grind_switches.size()> a{}; a.fill(-1); return a; }();
    std::array<std::int8_t, grind_switches.size()> grind_live = grind_mode;
    // The game's own grind settings (grind_settings, trainer_gamestate.h): -1 the game's own, 0 forced off, 1 forced on.
    std::array<std::int8_t, grind_settings.size()> grind_setting = [] { std::array<std::int8_t, grind_settings.size()> a{}; a.fill(-1); return a; }();
    // What the game had a held switch at before the trainer first wrote it (-1: not held, or not read yet). The game
    // sets most of them once and never again (measured 2026-10-11: released, a switch stayed where it was held), so
    // "the game's own" has to put this back.
    std::array<std::int8_t, grind_switches.size()> grind_stock = grind_mode;
    // Transition skating (bowls, quarter pipes, vert): the trainer's own help, each 0 / false when off.
    float tr_lean{};         // 0..1: how hard the skater leans into the ramp on a coping grind
    float tr_carry{};        // 0..1: how much of the speed along the coping is kept rolling off it
    float tr_drop{1.0f};     // m/s: the most speed across the coping, into the ramp, while that speed is kept
    float tr_unstick{};      // strength of the push out of a hang-up
    float tr_lockout{};      // seconds the coping cannot be caught again after a coping grind
    bool tr_spine{};         // a flight straight up and over a lip does not reach for the coping
    float tr_plant{};        // 0..1: a handplant leaves the lip the way Skate 3's did
    // The game's own transition settings (transition_settings, trainer_gamestate.h): -1 the game's own, 0 forced off, 1 forced on.
    // The game's own Settings options shown with them (transition_options): what the game holds, read twice a second.
    std::array<int, transition_options.size()> tr_option = [] {
        std::array<int, transition_options.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = transition_options[i].stock;
        return a;
    }();
    std::uint64_t tr_option_at{};
    int grind_assist{-1}; // the game's own Grind Assist (grind_assist_setting), read with the options above
    // What the player's own game settings hold for the settings the trainer can force (read with the options above):
    // a row that is not forced shows this, not the game's default.
    std::array<std::int8_t, grind_settings.size()> grind_own = [] {
        std::array<std::int8_t, grind_settings.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = grind_settings[i].stock;
        return a;
    }();
    std::array<std::int8_t, transition_settings.size()> tr_own = [] {
        std::array<std::int8_t, transition_settings.size()> a{};
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = transition_settings[i].stock;
        return a;
    }();
    std::array<std::int8_t, transition_settings.size()> tr_setting = [] { std::array<std::int8_t, transition_settings.size()> a{}; a.fill(-1); return a; }();
    // The game's own hidden on-foot switches (foot_settings, trainer_gamestate.h): -1 the game's own, 0 forced off, 1 forced on.
    std::array<std::int8_t, foot_settings.size()> foot_setting = [] { std::array<std::int8_t, foot_settings.size()> a{}; a.fill(-1); return a; }();
    // What the trainer knows of the transition the skater is on, from one tick to the next.
    struct TransitionTrack {
        std::uint64_t steep_at{};          // when the skater last rode steep ground
        std::array<float, 2> steep_out{};  // level direction out of that wall, into the ramp
        bool coping{};                     // on a coping grind or stall now
        bool grinding{}, flying{};         // at the last tick
        std::array<float, 2> edge{}, ramp{}; // level unit vectors: along the coping, and from it into the ramp
        float in_angle{}, in_speed{};      // how the grind began: degrees between the travel and the coping, m/s
        std::uint64_t left_at{};           // when the last coping grind ended without a pop
        std::uint64_t flight_off_until{};  // a flight up and over a lip: no reaching for edges until then
        bool flight_off{};
        bool air_grinds_off{};             // the air grind reach is held at 0 now
        float lean{};                      // the lean in force, eased
        std::uint64_t reports{};           // physics-step reports told so far
        std::uint64_t log_at{};
    } tr;
    float revert_boost{};            // 0: off
    float pump_power{1};             // x what pumping a transition gains
    bool pump_live{};                // the game's pumping states are found
    RevertTuning revert;
    std::uint64_t revert_boosts_seen{};
    std::uint32_t last_state{};
    float boost_factor{}; // velocity factor of the jump now starting, 0: none
    std::uint64_t boost_until{};
    float return_delay{1.5f};
    std::uint64_t seen_wipeouts{}, return_at{}, open_serial{};
    std::uint64_t class_search_at{}; // when to look for the game's tuning classes (0: not due)
    int class_search_tries{};
    std::uint64_t class_search_seen{};      // the searches that had finished when this level loaded
    bool class_list_wanted{};               // the player has the list of every value open on this level
    float flip_applied{1};                  // the flip speed the catch times were last set for
    float late_flip_applied{1};             // the factor the late flip's speed ends were last set for
    std::uint64_t class_entity{};           // the skater the last search was for
    std::uint64_t class_research_after{};   // no new search before this, however many skaters come and go
    std::uint64_t class_creations{};        // the skaters the game had built at the last look
    bool class_research_due{};              // a new skater came while a search could not start: look when it can
    bool class_skater_due{};                // the same, for a new skater alone (nothing looked lost)
    bool class_skater_judged{true};         // the search a new skater asked for has been compared with what was known
    std::uint64_t class_skater_after{};     // no search for a new skater alone before this
    std::uint64_t class_skater_wait{class_skater_wait_low};
    std::uint64_t class_found{};            // class_search_found() before that search
    std::vector<std::size_t> class_entries; // the entries that are fields of those classes
    // A session whose host sets everyone's physics (session_physics.h): what the host shares
    // beyond its tuning, and what each class field holds for it. The player's own stand down.
    bool enforced{}, competition{};
    std::uint64_t host_revision{};
    SessionExtras host;
    std::array<float, class_field_count> host_classes{};
    Boosts boosts;         // what is in force this tick
    std::string share_text; // the last exported preset, for the menu to put on the clipboard
    std::uint64_t share_serial{};
    bool extras_due{true}; // the player's own extras are to be published for a session again
    bool wipeouts_known{}; // the wipeout count the session started with was read
    int open_tab{};
    std::uint32_t pad_previous{};
    // The pause map's waypoint. The registry is read only while the player has a use for the
    // answer (the TELEPORT card is on screen, or `trainer waypoint`), and the waypoint is
    // forgotten on a new level: it belonged to the old one.
    std::optional<Vec3> waypoint;
    std::string waypoint_detail, waypoint_level;
    std::uint64_t waypoint_wanted_until{}, waypoint_next_read{};
    // A ground-snapped arrival being watched (trainer_landing.h).
    landing::Watch landing;
    Motion motion;
    Telemetry telemetry;
    std::ofstream log;
    std::int64_t log_start{};
    std::map<std::uint32_t, std::uint64_t> states; // physics state -> ticks seen
    SelfTest test;
};
State &state() {
    static auto *value = new State;
    return *value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// "MaxPushSpeed" -> "Max Push Speed"
std::string spaced(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool upper = std::isupper(c) != 0;
        if (i && upper) {
            const auto before = static_cast<unsigned char>(text[i - 1]);
            const bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (std::islower(before) || std::isdigit(before) || (std::isupper(before) && next_lower)) result += ' ';
        }
        result += text[i] == '.' ? ' ' : text[i];
        if (text[i] == '.') result += "/ ";
    }
    return result;
}
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

// ---- storage ---------------------------------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"trainer";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_position(const Json &value) {
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
        if (!std::isfinite(result[i]) || std::abs(result[i]) > max_coordinate) return std::nullopt;
    }
    return result;
}
Json write_position(const Vec3 &p) { return Json::array({Json(p[0]), Json(p[1]), Json(p[2])}); }

// Gravity as a multiple of the game's own.
constexpr float gravity_low = 0.001f, gravity_high = 1000.0f; // a typed number may go far past the slider
struct RevertField {
    std::string_view name;
    float RevertTuning::*member;
    float low, high;
};
constexpr RevertField revert_fields[]{
    {"spin", &RevertTuning::min_spin, 0.0f, 100000.0f},     {"slip", &RevertTuning::min_slip, 0.0f, 360.0f},
    {"twist", &RevertTuning::min_twist, 0.0f, 360.0f},       {"bend", &RevertTuning::bend_boost, -1.0e6f, 1.0e6f},
    {"full", &RevertTuning::full_boost, -1.0e6f, 1.0e6f}, {"auto", &RevertTuning::auto_boost, -1.0e6f, 1.0e6f},
    {"max_speed", &RevertTuning::max_speed, 0.0f, 1.0e6f}, {"cooldown", &RevertTuning::cooldown, 0.0f, 3600.0f},
    {"min_air", &RevertTuning::min_air, 0.0f, 3600.0f},
};
RevertTuning sane_revert(RevertTuning tuning) {
    const RevertTuning stock;
    for (const auto &field : revert_fields) {
        auto &value = tuning.*field.member;
        value = std::isfinite(value) ? std::clamp(value, field.low, field.high) : stock.*field.member;
    }
    return tuning;
}
void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"trainer.json");
    if (!json || !json->is_object()) return;
    try {
        if (json->contains("options") && json->at("options").is_object()) {
            const auto &o = json->at("options");
            s.auto_return = o.value("auto_return", false);
            s.pad_shortcuts = o.value("pad_shortcuts", false);
            s.hud = o.value("hud", false);
            s.hud_jump = o.value("hud_jump", false);
            s.return_delay = std::clamp(o.value("return_delay", 1.5f), 0.0f, 3600.0f);
            s.hippy_height = std::clamp(o.value("hippy_height", 1.0f), height_low, height_high);
            s.nocomply_height = std::clamp(o.value("nocomply_height", 1.0f), height_low, height_high);
            s.manual_height = std::clamp(o.value("manual_height", 1.0f), height_low, height_high);
            s.gravity = std::clamp(o.value("gravity", 1.0f), gravity_low, gravity_high);
            s.gravity_landing_own = o.value("gravity_landing_own", false);
            s.gravity_landing = std::clamp(o.value("gravity_landing", 1.0f), gravity_low, gravity_high);
            // (0.8.5 saved its default 0 as "gravity_timing"; the default is 1 since 0.8.6, under a new key.)
            s.gravity_timing = std::clamp(o.value("gravity_animation_timing", 1.0f), 0.0f, 1.0f);
            s.grind_side = std::clamp(o.value("grind_side", 1.0f), 0.0f, 1.0e6f);
            s.grind_push = std::clamp(o.value("grind_push", 0.0f), -1.0e6f, 1.0e6f);
            // (grind_lean, grind_turn and grind_tip were levers of earlier builds that did nothing: read past.)
            s.grind_range = std::clamp(o.value("grind_range", 0.0f), -1.0e6f, 1.0e6f);
            for (std::size_t i = 0; i < grind_settings.size(); ++i)
                s.grind_setting[i] = static_cast<std::int8_t>(std::clamp(o.value("grind.set." + std::string(grind_settings[i].key), -1), -1, 1));
            for (std::size_t i = 0; i < grind_switches.size(); ++i)
                s.grind_mode[i] = grind_switches[i].read_only ? std::int8_t{-1}
                                                               : static_cast<std::int8_t>(std::clamp(o.value("grind." + std::string(grind_switches[i].key), -1), -1, 1));
            s.tr_lean = std::clamp(o.value("transition.lean", 0.0f), -1000.0f, 1000.0f);
            s.tr_carry = std::clamp(o.value("transition.carry", 0.0f), 0.0f, 1000.0f);
            s.tr_drop = std::clamp(o.value("transition.drop", 1.0f), 0.0f, 1.0e6f);
            s.tr_unstick = std::clamp(o.value("transition.unstick", 0.0f), 0.0f, 1.0e6f);
            s.tr_lockout = std::clamp(o.value("transition.lockout", 0.0f), 0.0f, 3600.0f);
            s.tr_spine = o.value("transition.spine", false);
            s.tr_plant = std::clamp(o.value("transition.plant", 0.0f), 0.0f, 1000.0f);
            for (std::size_t i = 0; i < transition_settings.size(); ++i)
                s.tr_setting[i] = static_cast<std::int8_t>(std::clamp(o.value("transition.set." + std::string(transition_settings[i].key), -1), -1, 1));
            for (std::size_t i = 0; i < foot_settings.size(); ++i)
                s.foot_setting[i] = static_cast<std::int8_t>(std::clamp(o.value("foot.set." + std::string(foot_settings[i].key), -1), -1, 1));
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.underflip_speed = std::clamp(o.value("underflip_speed", 1.0f), flip_low, flip_high);
            s.underflip_split = o.value("underflip_split", false);
            s.flip_advanced = o.value("flip_advanced", false);
            s.flip_pop = o.value("flip_pop", false);
            s.flip_flick = o.value("flip_flick", false);
            s.flip_limit = o.value("flip_landing_limit", true);
            s.air_reach_fixed = o.value("air_reach_fixed", false);
            for (std::size_t i = 0; i < flip_tricks.size(); ++i)
                s.flip_trick[i] = std::clamp(o.value(std::string(flip_tricks[i].key), 1.0f), flip_low, flip_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.revert_boost = std::clamp(o.value("revert_boost", 0.0f), 0.0f, revert_boost_high);
            s.pump_power = std::clamp(o.value("pump_power", 1.0f), 0.0f, pump_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
        if (json->contains("folds") && json->at("folds").is_object())
            for (const auto &[id, open] : json->at("folds").items())
                if (open.is_boolean() && id.size() <= 64) s.folds[id] = open.get<bool>();
        if (json->contains("values") && json->at("values").is_object())
            for (const auto &[key, row] : json->at("values").items())
                if (row.is_object() && row.contains("value") && row.at("value").is_number())
                    s.saved[lower(key)] = {row.at("value").get<double>(), row.value("frozen", false)};
        if (json->contains("presets") && json->at("presets").is_object())
            for (const auto &[name, values] : json->at("presets").items()) {
                if (!values.is_object() || name.empty() || name.size() > max_preset_name) continue;
                auto &preset = s.user[name];
                for (const auto &[key, value] : values.items())
                    if (value.is_number()) preset[lower(key)] = value.get<double>();
            }
        if (json->contains("active") && json->at("active").is_array())
            for (const auto &name : json->at("active"))
                // (Fast Parkour Flips, Pump Power and Deep Landings were presets of earlier versions that changed nothing.)
                if (name.is_string() && name.string() != "Fast Parkour Flips" && name.string() != "Pump Power" && name.string() != "Deep Landings")
                    s.active.push_back(name.string());
        if (json->contains("revert") && json->at("revert").is_object()) {
            for (const auto &field : revert_fields)
                if (const auto name = std::string(field.name); json->at("revert").contains(name) && json->at("revert").at(name).is_number())
                    s.revert.*field.member = json->at("revert").at(name).get<float>();
            // 0.4.1 saved its own default for the off-the-travel rule (12 degrees), which paid ordinary
            // 180s: a file from before the rules were numbered that still holds it gets today's (off).
            if (!json->at("revert").contains("rules") && s.revert.min_slip == 12.0f) s.revert.min_slip = RevertTuning{}.min_slip;
            s.revert = sane_revert(s.revert);
        }
        if (json->contains("maps") && json->at("maps").is_object())
            for (const auto &[level, row] : json->at("maps").items()) {
                if (!row.is_object()) continue;
                auto &map = s.maps[lower(level)];
                map.preset = row.value("preset", "");
                if (row.contains("markers") && row.at("markers").is_array())
                    for (std::size_t i = 0; i < marker_slots && i < row.at("markers").size(); ++i)
                        if (const auto position = read_position(row.at("markers").at(i))) map.markers[i] = {true, *position};
            }
    } catch (...) {
        say(logging::Level::warning, "Trainer: trainer.json is damaged; starting from what could be read.");
    }
    // A preset of the player's own saved before a built-in one took its name would hide the
    // built-in. It keeps its values under a new name, and what named it follows.
    for (const auto &[from, to] : rename_reserved_presets(s.user)) {
        for (auto &name : s.active)
            if (lower(name) == lower(from)) name = to;
        for (auto &entry : s.maps)
            if (lower(entry.second.preset) == lower(from)) entry.second.preset = to;
        s.save_due = true;
        say(logging::Level::info, std::format("Trainer: your preset \"{}\" is now \"{}\": a built-in preset has that name.", from, to));
    }
}
void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json options = Json::object();
        options["auto_return"] = s.auto_return;
        options["pad_shortcuts"] = s.pad_shortcuts;
        options["hud"] = s.hud;
        options["hud_jump"] = s.hud_jump;
        options["return_delay"] = s.return_delay;
        options["hippy_height"] = s.hippy_height;
        options["nocomply_height"] = s.nocomply_height;
        options["manual_height"] = s.manual_height;
        options["gravity"] = s.gravity;
        options["gravity_landing_own"] = s.gravity_landing_own;
        options["gravity_landing"] = s.gravity_landing;
        options["gravity_animation_timing"] = s.gravity_timing;
        options["grind_side"] = s.grind_side;
        options["grind_push"] = s.grind_push;
        options["grind_range"] = s.grind_range;
        for (std::size_t i = 0; i < grind_settings.size(); ++i)
            if (s.grind_setting[i] >= 0) options["grind.set." + std::string(grind_settings[i].key)] = static_cast<int>(s.grind_setting[i]);
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (s.grind_mode[i] >= 0) options["grind." + std::string(grind_switches[i].key)] = static_cast<int>(s.grind_mode[i]);
        options["transition.lean"] = s.tr_lean;
        options["transition.carry"] = s.tr_carry;
        options["transition.drop"] = s.tr_drop;
        options["transition.unstick"] = s.tr_unstick;
        options["transition.lockout"] = s.tr_lockout;
        options["transition.spine"] = s.tr_spine;
        options["transition.plant"] = s.tr_plant;
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
            if (s.tr_setting[i] >= 0) options["transition.set." + std::string(transition_settings[i].key)] = static_cast<int>(s.tr_setting[i]);
        for (std::size_t i = 0; i < foot_settings.size(); ++i)
            if (s.foot_setting[i] >= 0) options["foot.set." + std::string(foot_settings[i].key)] = static_cast<int>(s.foot_setting[i]);
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["underflip_speed"] = s.underflip_speed;
        options["underflip_split"] = s.underflip_split;
        options["flip_advanced"] = s.flip_advanced;
        options["flip_pop"] = s.flip_pop;
        options["flip_flick"] = s.flip_flick;
        options["flip_landing_limit"] = s.flip_limit;
        options["air_reach_fixed"] = s.air_reach_fixed;
        for (std::size_t i = 0; i < flip_tricks.size(); ++i)
            if (s.flip_trick[i] != 1.0f) options[std::string(flip_tricks[i].key)] = s.flip_trick[i];
        options["boneless_height"] = s.boneless_height;
        options["revert_boost"] = s.revert_boost;
        options["pump_power"] = s.pump_power;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
        Json folds = Json::object();
        for (const auto &[id, open] : s.folds) folds[id] = open;
        json["folds"] = std::move(folds);
        Json values = Json::object();
        // Values of fields this build's tuning does not have are kept as they were read.
        for (const auto &[key, saved] : s.saved) {
            Json row = Json::object();
            row["value"] = saved.value;
            row["frozen"] = saved.frozen;
            values[key] = std::move(row);
        }
        json["values"] = std::move(values);
        Json presets = Json::object();
        for (const auto &[name, preset] : s.user) {
            Json row = Json::object();
            for (const auto &[key, value] : preset) row[key] = value;
            presets[name] = std::move(row);
        }
        json["presets"] = std::move(presets);
        Json active = Json::array();
        for (const auto &name : s.active) active.push_back(name);
        json["active"] = std::move(active);
        Json maps = Json::object();
        for (const auto &[level, map] : s.maps) {
            if (level.empty()) continue;
            Json row = Json::object();
            Json markers = Json::array();
            for (const auto &marker : map.markers) markers.push_back(marker.set ? write_position(marker.position) : Json(nullptr));
            row["markers"] = std::move(markers);
            row["preset"] = map.preset;
            maps[level] = std::move(row);
        }
        json["maps"] = std::move(maps);
        Json revert = Json::object();
        for (const auto &field : revert_fields) revert[std::string(field.name)] = s.revert.*field.member;
        revert["rules"] = 2;
        json["revert"] = std::move(revert);
        // Written on a thread of its own (the client tick saves 1.5 s after a change: a folder
        // lookup, a file written and renamed). The writes are kept in order by their own lock.
        std::thread([text = json.dump(2)] {
            static std::mutex writing;
            std::lock_guard lock(writing);
            try {
                const auto directory = data_directory();
                if (directory.empty()) return;
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                const auto target = directory / L"trainer.json", temporary = directory / L"trainer.json.tmp";
                {
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file << text;
                    if (!file) return;
                }
                std::filesystem::rename(temporary, target, error);
                if (error) {
                    std::filesystem::remove(target, error);
                    std::filesystem::rename(temporary, target, error);
                }
            } catch (...) {}
        }).detach();
    } catch (...) {
        say(logging::Level::warning, "Trainer: could not save trainer.json.");
    }
}
void changed(bool save = true) {
    auto &s = state();
    s.view_due = true;
    s.extras_due = true;
    if (save) {
        s.save_due = true;
        s.next_save = GetTickCount64() + 1500;
    }
}

// ---- values ----------------------------------------------------------------------------
double read_field(const std::vector<std::uint8_t> &image, const Entry &e) {
    const auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        float value;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (e.kind == Kind::flag) return *at ? 1 : 0;
    switch (e.size) {
    case 1: { std::int8_t v; std::memcpy(&v, at, 1); return v; }
    case 2: { std::int16_t v; std::memcpy(&v, at, 2); return v; }
    case 4: { std::int32_t v; std::memcpy(&v, at, 4); return v; }
    default: { std::int64_t v; std::memcpy(&v, at, 8); return static_cast<double>(v); }
    }
}
void write_field(std::vector<std::uint8_t> &image, const Entry &e, double value) {
    auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        const auto v = static_cast<float>(value);
        std::memcpy(at, &v, 4);
    } else if (e.kind == Kind::flag) {
        *at = value != 0 ? 1 : 0;
    } else {
        switch (e.size) {
        case 1: { const auto v = static_cast<std::int8_t>(std::clamp(value, -128.0, 127.0)); std::memcpy(at, &v, 1); break; }
        case 2: { const auto v = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)); std::memcpy(at, &v, 2); break; }
        case 4: { const auto v = static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)); std::memcpy(at, &v, 4); break; }
        default: { const auto v = static_cast<std::int64_t>(value); std::memcpy(at, &v, 8); break; }
        }
    }
}
// A curve's outputs times `factor`: a point is 0x1c bytes with its output at +0x14.
tuning::Curve scaled_curve(const tuning::Curve &source, double factor) {
    auto result = source;
    for (std::size_t p = 0; p + addr::physics_tuning::curve_point_size <= result.points.size(); p += addr::physics_tuning::curve_point_size) {
        float y;
        std::memcpy(&y, result.points.data() + p + 0x14, 4);
        y = static_cast<float>(y * factor);
        if (std::isfinite(y)) std::memcpy(result.points.data() + p + 0x14, &y, 4);
    }
    return result;
}
double sane(const Entry &e, double value) {
    if (!std::isfinite(value)) return e.stock;
    if (e.kind == Kind::flag) return value != 0 ? 1 : 0;
    if (e.kind == Kind::integer) return std::round(value);
    // No limits of the trainer's own beyond keeping a number a number: what a value may sensibly
    // be is the player's call (a slider's ends are only where the slider stops).
    if (e.kind == Kind::curve || e.kind == Kind::graph) return std::clamp(value, -1.0e9, 1.0e9);
    return std::clamp(value, -1.0e12, 1.0e12);
}
// The held flip class's catch times: how long each kind of flip may take to come round.
bool extras_blocked();
// While a late flip runs the game sets its speed every update to clamp(the flick's speed, lateflip.MinSpeed, lateflip.MaxSpeed),
// so the flip speed the trainer gives other flips never reaches it (measured 2026-10-15: x0.5, 1 and 2 all turned a late kickflip
// about 260 deg at 1155 deg/s). Those two ends follow the flip speed and the Late flip per-trick speed instead.
bool late_flip_speed(std::string_view key) { return key == "lateflip.maxspeed" || key == "lateflip.minspeed"; }
// The Late flip's own speed sits beside Flip trick speed and always counts; the other tricks' own speeds count with
// Advanced trick speed ticked.
float own_trick_speed(int index) {
    auto &s = state();
    if (index < 0 || (!s.flip_advanced && static_cast<std::size_t>(index) != late_flip_slot)) return 1.0f;
    return s.flip_trick[static_cast<std::size_t>(index)];
}
float late_flip_factor() {
    auto &s = state();
    if (extras_blocked()) return 1.0f;
    return s.boosts.flip * own_trick_speed(flip_trick_index(40)); // LateFlipTrick
}
bool slowed_catch_time(std::string_view key) {
    return key.starts_with("heldflip.") && key.ends_with("catchtime");
}
// What a class field should hold: the player's value, or the host's while a session's host
// sets everyone's physics.
void want_class(const Entry &e) {
    auto &s = state();
    const auto field = static_cast<std::size_t>(e.class_field);
    auto value = s.enforced ? s.host_classes[field] : static_cast<float>(e.touched ? e.value : e.stock);
    // The game hurries any flip that would not come round inside its catch time (measured: at
    // every flip speed below about x0.9 a kickflip turned at 360 degrees per 0.2 s, the stock
    // catch time). A flip slowed by the trick slider is given that much longer.
    if (s.boosts.flip < 1.0f && slowed_catch_time(e.key)) value /= std::max(s.boosts.flip, flip_low);
    if (late_flip_speed(e.key)) value *= late_flip_factor();
    want_class_value(field, value);
}
void apply_entry(Entry &e) {
    auto &s = state();
    if (e.class_field >= 0) {
        want_class(e);
        return;
    }
    if (e.kind == Kind::graph) {
        // A point the player set by hand keeps their value.
        for (const auto index : e.members) {
            const auto &member = s.entries[index];
            if (!member.touched && static_cast<std::size_t>(member.offset) + member.size <= s.want.image.size())
                write_field(s.want.image, member, e.touched ? member.stock * e.value : member.stock);
        }
    } else if (e.kind == Kind::curve) {
        if (e.curve < s.own.curves.size() && s.own.curves[e.curve] && e.curve < s.want.curves.size())
            s.want.curves[e.curve] = e.touched ? scaled_curve(*s.own.curves[e.curve], e.value) : *s.own.curves[e.curve];
    } else if (static_cast<std::size_t>(e.offset) + e.size <= s.want.image.size()) {
        write_field(s.want.image, e, e.touched ? e.value : e.stock);
    }
    s.write_due = true;
}
void remember(const Entry &e) {
    auto &s = state();
    if (e.touched || e.frozen) s.saved[e.key] = {e.value, e.frozen};
    else s.saved.erase(e.key);
}
// `own`: the player's change, remembered and saved, which takes the value back from a plugin.
// Otherwise a plugin's (plugin_tune), neither.
void set_entry(Entry &e, double value, bool own = true) {
    e.value = sane(e, value);
    e.touched = e.value != e.stock;
    apply_entry(e);
    if (own) {
        remember(e);
        state().plugin_tunes.erase(e.key);
    }
    // A linked value scales its graphs by the same ratio (the graphs are what the game reads).
    if (!e.drives.empty() && e.stock != 0) {
        const auto ratio = e.value / e.stock;
        const auto targets = e.drives; // set_entry may not touch `e` again, but keep the list stable
        auto &s = state();
        for (const auto index : targets)
            if (index < s.entries.size() && !s.entries[index].frozen) {
                auto &target = s.entries[index];
                const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
                set_entry(target, multiplier ? ratio : target.stock * ratio, own);
            }
    }
}
// A value a locked linked value drives (its graphs, the push class's speeds) is locked with it:
// resetting it alone would leave the lock showing a number that no longer does anything.
bool is_locked(const Entry &e) {
    if (e.frozen) return true;
    auto &s = state();
    const auto index = static_cast<std::size_t>(&e - s.entries.data());
    return std::ranges::any_of(s.entries, [&](const Entry &other) {
        return other.frozen && std::ranges::find(other.drives, index) != other.drives.end();
    });
}
Entry *find_entry(std::string_view id) {
    auto &s = state();
    const auto found = s.index.find(lower(id));
    return found == s.index.end() ? nullptr : &s.entries[found->second];
}
void build_entries() {
    auto &s = state();
    s.plugin_tunes.clear();
    s.entries.clear();
    s.index.clear();
    s.class_entries.clear();
    const auto &m = *s.model;
    const auto add = [&](std::string name, Kind kind, std::uint16_t offset, std::uint16_t size, std::size_t curve) {
        if (name.empty()) name = std::format("Unnamed.0x{:x}", offset);
        Entry e;
        e.id = kind == Kind::curve || kind == Kind::graph ? name + " x" : name;
        e.key = lower(e.id);
        if (s.index.contains(e.key)) { // arrays of structures repeat names
            e.id += std::format(" @0x{:x}", offset);
            e.key = lower(e.id);
        }
        const auto dot = name.find('.');
        auto group = dot == std::string::npos ? std::string("General") : name.substr(0, dot);
        for (const std::string_view prefix : {"SkatePhysics", "Physics"})
            if (group.size() > prefix.size() && group.starts_with(prefix)) {
                group.erase(0, prefix.size());
                break;
            }
        e.group = spaced(group);
        e.label = spaced(dot == std::string::npos ? name : name.substr(dot + 1));
        if (kind == Kind::curve || kind == Kind::graph) e.label += " (x)";
        e.kind = kind;
        e.offset = offset;
        e.size = size;
        e.curve = curve;
        e.used = value_used(offset);
        s.index.emplace(e.key, s.entries.size());
        s.entries.push_back(std::move(e));
    };
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        const auto &f = m.fields[i];
        add(i < m.field_names.size() ? m.field_names[i] : std::string{}, f.real ? Kind::real : f.flag ? Kind::flag : Kind::integer,
            f.offset, f.size, 0);
    }
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i)
        add(i < m.curve_names.size() ? m.curve_names[i] : std::string{}, Kind::curve, m.curve_slots[i], 8, i);
    // Graphs stored in the asset: sixteen X and Y values, a point count and bounds. Each
    // becomes one multiplier row; its points stay editable but out of the way.
    const auto plain = s.entries.size();
    for (std::size_t i = 0; i < plain; ++i) {
        if (!s.entries[i].id.ends_with(".Y0") || s.entries[i].kind != Kind::real) continue;
        const auto parent = s.entries[i].id.substr(0, s.entries[i].id.size() - 3);
        const auto offset = s.entries[i].offset;
        std::vector<std::size_t> outputs;
        for (std::size_t j = 0; j < plain; ++j) {
            auto &other = s.entries[j];
            if (other.id.size() <= parent.size() + 1 || !other.id.starts_with(parent) || other.id[parent.size()] != '.') continue;
            const std::string_view leaf(other.id.c_str() + parent.size() + 1);
            if (leaf.find('.') != std::string_view::npos) continue;
            other.detail = true;
            const bool output = (leaf.starts_with("Y") && leaf.size() <= 3) || leaf == "Min_Y" || leaf == "Max_Y";
            if (output && other.kind == Kind::real) outputs.push_back(j);
        }
        add(parent, Kind::graph, offset, 0, 0);
        s.entries.back().members = std::move(outputs);
    }
    // The game's data-defined classes: one row per number field, under the class's name.
    for (std::size_t c = 0; c < class_count; ++c)
        for (std::size_t i = 0; i < class_specs[c].count; ++i) {
            const auto &field = class_fields[class_specs[c].first + i];
            // A script block's integer words (Code<n>) only make its copy findable: no row, never written.
            if (std::string_view(field.name).starts_with("Code")) continue;
            Entry e;
            e.id = std::format("{}.{}", class_specs[c].key, field.name);
            e.key = lower(e.id);
            if (s.index.contains(e.key)) continue;
            e.group = class_specs[c].group;
            e.label = spaced(field.name);
            e.kind = Kind::real;
            e.used = field.seen;
            e.class_field = static_cast<int>(class_specs[c].first + i);
            e.stock = e.value = field.stock;
            s.index.emplace(e.key, s.entries.size());
            s.class_entries.push_back(s.entries.size());
            s.entries.push_back(std::move(e));
        }
    for (const auto &link : value_links()) {
        const auto from = s.index.find(link.value), to = s.index.find(link.drives);
        if (from == s.index.end() || to == s.index.end()) continue;
        s.entries[from->second].drives.push_back(to->second);
        s.entries[from->second].used = true;
    }
    // A host's values may have arrived before there were entries to hold them.
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    s.extras_due = true;
    say(logging::Level::info, std::format("Trainer: {} tuning values and {} curves are editable.", m.fields.size(), m.curve_slots.size()));
}
// A new copy of the asset (first sight, or a level load): its values are the stock ones.
void adopt(const tuning::Values &live) {
    auto &s = state();
    s.own = live;
    s.want = live;
    for (auto &e : s.entries) {
        e.stock = e.class_field >= 0 ? class_fields[e.class_field].stock : e.kind == Kind::curve || e.kind == Kind::graph ? 1.0
            : read_field(s.own.image, e);
        const auto found = s.saved.find(e.key);
        if (found != s.saved.end()) {
            e.frozen = found->second.frozen;
            e.value = sane(e, found->second.value);
            e.touched = e.value != e.stock;
        } else if (!e.touched) {
            e.value = e.stock;
        }
        if (e.touched) apply_entry(e);
    }
    // Grind lock-on turned up (0.8.4 to 0.8.6) doubled the reach from the air too, which steered pops near rails onto them.
    // The dial moves only the lock-on distance now; the reach it saved turned up goes back to the game's own, once.
    if (!s.air_reach_fixed) {
        std::size_t cleared{};
        constexpr std::string_view reach[]{"physicstrajectory.scoregrind", "physicsgrindsair.maxdist50_50", "physicsgrindsair.maxdist5_0",
                                           "physicsgrindsair.maxdisttipslide", "physicsgrindsair.maxdistboardslide"};
        for (const auto key : reach) {
            auto *e = find_entry(key);
            if (!e || e->frozen || !(e->value > e->stock)) continue;
            for (auto &[name, preset] : s.user)
                if (const auto held = preset.find(e->key); held != preset.end() && held->second > e->stock) preset.erase(held);
            set_entry(*e, e->stock);
            ++cleared;
        }
        s.air_reach_fixed = true;
        s.save_due = true;
        if (cleared) say(logging::Level::info, std::format("Trainer: {} turned-up grind reach values from the air are the game's own again.", cleared));
    }
    // A linked value saved by itself (before links existed, or edited in the file) still
    // drives its graphs.
    for (auto &e : s.entries) {
        if (e.drives.empty() || !e.touched || e.stock == 0) continue;
        for (const auto index : e.drives) {
            auto &target = s.entries[index];
            if (target.touched || target.frozen) continue;
            const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
            target.value = sane(target, multiplier ? e.value / e.stock : target.stock * e.value / e.stock);
            target.touched = target.value != target.stock;
            if (target.touched) {
                apply_entry(target);
                remember(target);
            }
        }
    }
    s.write_due = true;
    changed(false);
}
std::string apply_preset(std::string_view name);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "You are in a multiplayer game where the host controls physics. This menu will not work in this multiplayer game.";
        return false;
    }
    return true;
}
// Auto push is the trainer's doing: the game's flag reaches only its animation.
float own_cruise() {
    const auto *auto_push = find_entry("physicsmode.autopushenabled"), *auto_speed = find_entry("physicspush.maxspeedforautopush");
    return auto_push && auto_speed && auto_push->touched && auto_push->value != 0 ? static_cast<float>(auto_speed->value) : 0.0f;
}
// The host's where a session's host sets everyone's physics, whatever the boosts switch says
// (that switch is about a guest's own); otherwise the player's, and the game's own in a
// session that has turned boosts off.
// A session where the player's own trick extras do not count: its host sets the physics, or has turned boosts off.
bool extras_blocked() { return state().enforced || (multiplayer_session_active() && !session_boosts_allowed()); }
Boosts boosts_in_force() {
    auto &s = state();
    if (s.enforced)
        return {s.host.flip_speed, s.host.hippy_height, s.host.nocomply_height, s.host.boneless_height, s.host.offboard_height, s.host.cruise};
    if (multiplayer_session_active() && !session_boosts_allowed()) return {};
    return {s.flip_speed, s.hippy_height, s.nocomply_height, s.boneless_height, s.offboard_height, own_cruise()};
}
// Underflip speed as it acts now: the host's in a session where the host sets the physics, 1 where the player's own
// extras do not count, otherwise the player's.
float underflip_in_force() {
    auto &s = state();
    if (s.enforced) return s.host.underflip_speed;
    return extras_blocked() ? 1.0f : s.underflip_speed;
}
// "Underflip part only", the same way.
bool underflip_split_in_force() {
    auto &s = state();
    if (s.enforced) return s.host.underflip_split;
    return !extras_blocked() && s.underflip_split;
}
// The player's own, for a session they host to share with its guests.
void publish_extras() {
    auto &s = state();
    s.extras_due = false;
    SessionExtras own;
    own.flip_speed = s.flip_speed;
    own.hippy_height = s.hippy_height;
    own.nocomply_height = s.nocomply_height;
    own.boneless_height = s.boneless_height;
    own.offboard_height = s.offboard_height;
    own.cruise = own_cruise();
    own.underflip_speed = s.underflip_speed;
    own.underflip_split = s.underflip_split;
    for (const auto index : s.class_entries)
        if (const auto &e = s.entries[index]; e.touched)
            own.classes.emplace_back(static_cast<std::uint16_t>(e.class_field), static_cast<float>(e.value));
    set_local_physics_extras(encode_session_extras(own));
}
// Follows a session into and out of its host's physics, and the host's changes while there.
void sync_session() {
    auto &s = state();
    const bool enforced = session_tuning_enforced();
    const bool competition = competition_stock_physics();
    const bool competition_changed = competition != s.competition;
    std::vector<std::uint8_t> bytes;
    bool fresh = false;
    if (competition) { fresh = competition_changed; s.host_revision = 0; }
    else if (enforced) fresh = host_physics_extras(s.host_revision, bytes);
    else s.host_revision = 0; // read again the next time a host sets the physics
    if (enforced == s.enforced && !fresh && !competition_changed) return;
    if (fresh) {
        auto decoded = decode_session_extras(bytes);
        if (!decoded) say(logging::Level::warning, "Trainer: what the host shares beyond its tuning could not be read; the game's own is used for it.");
        s.host = decoded ? std::move(*decoded) : SessionExtras{};
        for (std::size_t i = 0; i < class_field_count; ++i) s.host_classes[i] = class_fields[i].stock;
        for (const auto &[index, value] : s.host.classes) s.host_classes[index] = value;
        if (!s.host.stock())
            say(logging::Level::info, std::format("Trainer: skating with the host's trick multipliers and {} of its class values.", s.host.classes.size()));
    }
    s.enforced = enforced;
    s.competition = competition;
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    changed(false);
}
void hold(std::uint64_t now) {
    auto &s = state();
    if (!s.base) return;
    if (!s.model) {
        tuning::prepare();
        std::string error;
        s.model = tuning::game_tuning(&error);
        if (!s.model) {
            const auto next = error.empty() ? std::string("Reading the game's physics tuning...") : "Physics tuning is unavailable: " + error + ".";
            if (next != s.status) { s.status = next; changed(false); }
            return;
        }
        build_entries();
    }
    if (!s.write_due && now < s.next_hold) return;
    s.next_hold = now + 500;
    tuning::Values live;
    std::uintptr_t asset{};
    if (!tuning::read_live(s.base, live, &asset)) {
        if (s.ready) { s.ready = false; s.status = "Waiting for a level."; changed(false); }
        return;
    }
    if (asset != s.asset || !s.ready) {
        s.asset = asset;
        adopt(live);
        s.ready = true;
        s.status = "Ready.";
    }
    if (!editable()) return;
    if (s.profile_due) {
        s.profile_due = false;
        if (const auto found = s.maps.find(s.map); found != s.maps.end() && !found->second.preset.empty())
            say(logging::Level::info, "Trainer: this map's preset - " + apply_preset(found->second.preset));
    }
    const bool pending = s.write_due || std::ranges::any_of(s.entries, [](const Entry &e) { return e.touched; });
    s.write_due = false;
    if (!pending) return;
    const auto written = tuning::write_live(s.base, s.entity, s.want);
    if (written.failed) {
        s.status = "The physics tuning could not be written.";
        changed(false);
    }
    // The skater skates from its own copy of part of the tuning: keep asking until it took it.
    if (written.values || written.curves) {
        s.refresh_due = !written.refreshed;
        say(logging::Level::info, std::format("Trainer: wrote {} values and {} curves; skater copy refreshed: {}.", written.values, written.curves,
                                              written.refreshed ? "yes" : "not yet"));
    } else if (s.refresh_due && tuning::refresh_skater(s.base, s.entity)) {
        s.refresh_due = false;
        say(logging::Level::info, "Trainer: skater copy refreshed.");
    }
}

// ---- presets ---------------------------------------------------------------------------
bool matches(const std::string &key, std::string_view pattern) {
    std::size_t start = 0;
    while (start < pattern.size()) {
        auto end = pattern.find(' ', start);
        if (end == std::string_view::npos) end = pattern.size();
        const auto token = pattern.substr(start, end - start);
        if (!token.empty()) {
            const bool exclude = token.front() == '!';
            const auto word = exclude ? token.substr(1) : token;
            if ((key.find(word) != std::string::npos) == exclude) return false;
        }
        start = end + 1;
    }
    return true;
}
// What each built-in preset's rules reach in the running game: (entry, rule) pairs, worked out
// once per table of entries.
// Whether a preset rule is about this value (`scale`: it is a curve or graph multiplier).
bool rule_matches(const Entry &e, bool scale, const PresetRule &rule) {
    if (rule.exact) return !scale && e.key == rule.pattern;
    return !e.detail && scale == rule.curves && matches(e.key, rule.pattern);
}
const std::vector<std::vector<std::pair<std::size_t, std::size_t>>> &preset_targets() {
    static std::vector<std::vector<std::pair<std::size_t, std::size_t>>> cache;
    static const Entry *cached_for{};
    static std::size_t cached_count{};
    auto &s = state();
    if (cached_for == s.entries.data() && cached_count == s.entries.size() && !cache.empty()) return cache;
    cached_for = s.entries.data();
    cached_count = s.entries.size();
    const auto &presets = builtin_presets();
    cache.assign(presets.size(), {});
    for (std::size_t p = 0; p < presets.size(); ++p)
        for (std::size_t r = 0; r < presets[p].rules.size(); ++r) {
            const auto &rule = presets[p].rules[r];
            for (std::size_t i = 0; i < s.entries.size(); ++i) {
                const auto &e = s.entries[i];
                const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                if (!rule_matches(e, scale, rule)) continue;
                if (e.kind == Kind::flag && rule.multiply) continue;
                cache[p].emplace_back(i, r);
            }
        }
    return cache;
}
// A preset as a dial: `factor` is where its first value goes (x of stock); the others follow in
// proportion, so 1 is the game's own and the first rule's amount is the preset as it ships.
std::string apply_dial(std::string_view name, double factor) {
    auto &s = state();
    const auto &presets = builtin_presets();
    const auto wanted = lower(name);
    std::size_t p = 0;
    while (p < presets.size() && lower(presets[p].name) != wanted) ++p;
    if (p == presets.size()) return "error: no built-in preset is called \"" + std::string(name) + "\".";
    const auto &rules = presets[p].rules;
    if (rules.empty() || !rules[0].multiply || rules[0].amount <= 0 || rules[0].amount == 1)
        return "error: " + std::string(presets[p].name) + " is a switch, not a dial.";
    if (!(factor >= 0) || !std::isfinite(factor)) return "error: a dial needs a multiplier of 0 or more; 1 is the game's own.";
    // 0 (t = -infinity): the values that move with the first go to 0; one that moves the other way stays the game's own
    // rather than going to infinity.
    const double t = factor == 0 ? -std::numeric_limits<double>::infinity() : std::log(factor) / std::log(rules[0].amount);
    const bool up = rules[0].amount > 1;
    std::size_t count{}, locked{};
    for (const auto &[index, r] : preset_targets()[p]) {
        auto &e = s.entries[index];
        const auto &rule = rules[r];
        if (is_locked(e)) {
            ++locked;
            continue;
        }
        if (rule.multiply && factor == 0) set_entry(e, rule.amount > 0 && (rule.amount > 1) == up ? 0.0 : e.stock);
        else if (rule.multiply) set_entry(e, rule.amount > 0 ? e.stock * std::pow(rule.amount, t) : e.stock * (1 + (rule.amount - 1) * t));
        else set_entry(e, t > 0.01 ? rule.amount : e.stock); // a switch that goes with the preset's direction
        ++count;
    }
    std::erase(s.active, std::string(presets[p].name));
    if (std::abs(t - 1) < 1e-3) s.active.emplace_back(presets[p].name);
    changed();
    return std::format("{} x{:.2f}: {} values set{}.", presets[p].name, factor, count, locked ? std::format("; {} locked values left alone", locked) : "");
}
float *trick_option(std::string_view name) {
    auto &s = state();
    for (std::size_t i = 0; i < flip_tricks.size(); ++i)
        if (name == flip_tricks[i].key) return &s.flip_trick[i];
    return name == "flip_speed" ? &s.flip_speed : name == "underflip_speed" ? &s.underflip_speed : name == "hippy_height" ? &s.hippy_height : name == "nocomply_height" ? &s.nocomply_height
         : name == "boneless_height" ? &s.boneless_height : name == "offboard_height" ? &s.offboard_height
         : name == "revert_boost" ? &s.revert_boost : name == "pump_power" ? &s.pump_power : name == "manual_height" ? &s.manual_height : nullptr;
}
// Everything of the trainer's that changes how the game plays and is not a tuning value: one number each, under the
// key a preset carries it by. The STOCK badge, every reset and the player's presets all go through this list, so a new
// option cannot be counted by one of them and missed by another.
struct Setup {
    std::string key;
    std::uint8_t topic{}; // the section of the Tune tab that shows it (trainer.h: topic_*)
    double stock{};
    std::function<double()> get;
    std::function<void(double)> set;
    std::function<bool()> in_force; // empty: always. Off stock but not in force (a trick's own speed with Advanced unticked) is not "modified".
    std::string title;      // in the menu's quick settings
    std::uint8_t kind{};    // setup_number / setup_switch / setup_mode (trainer.h)
    double low{}, high{};
};
const std::vector<Setup> &setups() {
    static const auto *list = [] {
        auto *all = new std::vector<Setup>;
        auto &s = state();
        const auto real = [&](std::string key, std::uint8_t topic, float *value, float stock, float low, float high) {
            all->push_back({std::move(key), topic, stock, [value] { return static_cast<double>(*value); },
                            [value, low, high](double v) { *value = std::isfinite(v) ? std::clamp(static_cast<float>(v), low, high) : *value; }, {}});
            all->back().low = low;
            all->back().high = high;
        };
        const auto toggle = [&](std::string key, std::uint8_t topic, bool *value) {
            all->push_back({std::move(key), topic, 0.0, [value] { return *value ? 1.0 : 0.0; }, [value](double v) { *value = v != 0; }, {}});
            all->back().kind = setup_switch;
            all->back().high = 1;
        };
        const auto mode = [&](std::string key, std::uint8_t topic, std::int8_t *value) {
            all->push_back({std::move(key), topic, -1.0, [value] { return static_cast<double>(*value); },
                            [value](double v) { *value = static_cast<std::int8_t>(v < 0 ? -1 : v != 0 ? 1 : 0); }, {}});
            all->back().kind = setup_mode;
            all->back().low = -1;
            all->back().high = 1;
        };
        real("trick.flip_speed", topic_pop, &s.flip_speed, 1.0f, flip_low, flip_high);
        real("trick.underflip_speed", topic_pop, &s.underflip_speed, 1.0f, flip_low, flip_high);
        real("trick.manual_height", topic_pop, &s.manual_height, 1.0f, height_low, height_high);
        real("trick.gravity", topic_pop, &s.gravity, 1.0f, gravity_low, gravity_high);
        real("trick.nocomply_height", topic_pop, &s.nocomply_height, 1.0f, height_low, height_high);
        real("trick.boneless_height", topic_pop, &s.boneless_height, 1.0f, height_low, height_high);
        real("trick.hippy_height", topic_pop, &s.hippy_height, 1.0f, height_low, height_high);
        real("trick.offboard_height", topic_foot, &s.offboard_height, 1.0f, height_low, height_high);
        real("trick.pump_power", topic_pumping, &s.pump_power, 1.0f, 0.0f, pump_high);
        real("trick.revert_boost", topic_trickline, &s.revert_boost, 0.0f, 0.0f, revert_boost_high);
        toggle("trick.flip_advanced", topic_pop, &s.flip_advanced);
        toggle("trick.flip_pop", topic_pop, &s.flip_pop);
        toggle("trick.flip_flick", topic_pop, &s.flip_flick);
        toggle("trick.flip_limit", topic_pop, &s.flip_limit);
        all->back().stock = 1; // the game's own: on
        toggle("trick.underflip_split", topic_pop, &s.underflip_split);
        toggle("trick.gravity_landing_own", topic_pop, &s.gravity_landing_own);
        real("trick.gravity_landing", topic_pop, &s.gravity_landing, 1.0f, gravity_low, gravity_high);
        real("trick.gravity_timing", topic_pop, &s.gravity_timing, 1.0f, 0.0f, 1.0f);
        for (std::size_t i = 0; i < flip_tricks.size(); ++i) {
            real("trick." + std::string(flip_tricks[i].key), topic_pop, &s.flip_trick[i], 1.0f, flip_low, flip_high);
            all->back().title = std::string(flip_tricks[i].label) + (i >= first_underflip_slot ? ": underflip speed" : ": flip speed");
            if (i != late_flip_slot) all->back().in_force = [&s] { return s.flip_advanced; };
        }
        real("grind.side", topic_grinds, &s.grind_side, 1.0f, 0.0f, 1.0e6f);
        real("grind.push", topic_grinds, &s.grind_push, 0.0f, -1.0e6f, 1.0e6f);
        real("grind.range", topic_grinds, &s.grind_range, 0.0f, -1.0e6f, 1.0e6f);
        for (std::size_t i = 0; i < grind_settings.size(); ++i) {
            mode("grind.set." + std::string(grind_settings[i].key), topic_grinds, &s.grind_setting[i]);
            all->back().title = "Grinds: " + std::string(grind_settings[i].label);
        }
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (!grind_switches[i].read_only) {
                mode("grind.hold." + std::string(grind_switches[i].key), topic_grinds, &s.grind_mode[i]);
                all->back().title = "Grinds: " + std::string(grind_switches[i].label);
            }
        real("transition.lean", topic_transition, &s.tr_lean, 0.0f, -1000.0f, 1000.0f);
        real("transition.carry", topic_transition, &s.tr_carry, 0.0f, 0.0f, 1000.0f);
        real("transition.drop", topic_transition, &s.tr_drop, 1.0f, 0.0f, 1.0e6f);
        all->back().in_force = [&s] { return s.tr_carry != 0.0f; }; // the limit on what the row above keeps
        real("transition.unstick", topic_transition, &s.tr_unstick, 0.0f, 0.0f, 1.0e6f);
        real("transition.lockout", topic_transition, &s.tr_lockout, 0.0f, 0.0f, 3600.0f);
        real("transition.plant", topic_transition, &s.tr_plant, 0.0f, 0.0f, 1000.0f);
        toggle("transition.spine", topic_transition, &s.tr_spine);
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
        {
            mode("transition.set." + std::string(transition_settings[i].key), transition_settings[i].card == transition_card_pump ? topic_pumping : topic_transition,
                 &s.tr_setting[i]);
            all->back().title = (transition_settings[i].card == transition_card_pump ? "Pumping: " : "Transition: ") + std::string(transition_settings[i].label);
        }
        for (std::size_t i = 0; i < foot_settings.size(); ++i) {
            if (!foot_settings[i].shown) continue;
            mode("foot.set." + std::string(foot_settings[i].key), foot_settings[i].board ? topic_pop : topic_foot, &s.foot_setting[i]);
            all->back().title = (foot_settings[i].board ? "Wallie: " : "On foot: ") + std::string(foot_settings[i].label);
        }
        for (const auto &field : revert_fields) {
            const auto member = field.member;
            const float low = field.low, high = field.high;
            all->push_back({"revert." + std::string(field.name), topic_trickline, static_cast<double>(RevertTuning{}.*member),
                            [&s, member] { return static_cast<double>(s.revert.*member); },
                            [&s, member, low, high](double v) { if (std::isfinite(v)) s.revert.*member = std::clamp(static_cast<float>(v), low, high); }, {}});
            all->back().low = low;
            all->back().high = high;
        }
        constexpr std::pair<std::string_view, std::string_view> titles[]{
            {"trick.flip_speed", "Flip trick speed"}, {"trick.underflip_speed", "Underflip speed"}, {"trick.manual_height", "Manual pop height"}, {"trick.gravity", "Gravity"}, {"trick.gravity_landing_own", "Gravity: landings planned with their own gravity"}, {"trick.gravity_landing", "Landing gravity"},
            {"trick.gravity_timing", "Animation timing"},
            {"trick.nocomply_height", "No comply height"}, {"trick.boneless_height", "Boneless height"}, {"trick.hippy_height", "Hippy jump height"},
            {"trick.offboard_height", "Off-board jump height"}, {"trick.pump_power", "Pump power"}, {"trick.revert_boost", "Board bending boost"},
            {"trick.flip_advanced", "Advanced trick speed"}, {"trick.flip_pop", "Slow flips pop higher"}, {"trick.flip_flick", "Flick tricks at full speed"}, {"trick.flip_limit", "Flip landing limit"},
            {"trick.underflip_split", "Underflip speed: under part only"},
            {"grind.side", "Grind pop: side force"}, {"grind.push", "Grind pop: push with stick (m/s)"}, {"grind.range", "Grinds: left stick turn range (deg)"},
            {"transition.lean", "Transition: lean into the ramp"}, {"transition.carry", "Transition: roll off at your angle"},
            {"transition.drop", "Transition: roll-off drop speed (m/s)"}, {"transition.unstick", "Transition: no hang-ups"},
            {"transition.lockout", "Transition: coping re-catch delay (s)"}, {"transition.plant", "Transition: Skate 3 plant exit"},
            {"transition.spine", "Transition: airs clear the coping"}, {"revert.spin", "Bending: spin needed (deg)"},
            {"revert.twist", "Bending: board off your body (deg)"}, {"revert.slip", "Bending: board off the travel (deg)"},
            {"revert.bend", "Bending boost: a small bend (m/s)"}, {"revert.full", "Bending boost: a full bend (m/s)"},
            {"revert.auto", "Bending boost: an auto revert (m/s)"}, {"revert.max_speed", "Bending: no boost above (m/s)"},
            {"revert.cooldown", "Bending: time between boosts (s)"}, {"revert.min_air", "Bending: shortest flight (s)"}};
        for (auto &one : *all) {
            for (const auto &[key, title] : titles)
                if (one.key == key) one.title = title;
            if (one.title.empty()) one.title = one.key;
        }
        return all;
    }();
    return *list;
}
const Setup *find_setup(std::string_view key) {
    for (const auto &one : setups())
        if (one.key == key) return &one;
    return nullptr;
}
bool off_stock(const Setup &one) { return one.get() != one.stock; }
bool modified(const Setup &one) { return off_stock(one) && (!one.in_force || one.in_force()); }
// Puts back everything on that list (`topic` 0), or what one section of the Tune tab shows.
std::size_t reset_setups(std::uint8_t topic = 0) {
    std::size_t count{};
    for (const auto &one : setups())
        if ((!topic || one.topic == topic) && off_stock(one)) {
            one.set(one.stock);
            ++count;
        }
    return count;
}
// Sets one of the setups named by a preset key; false when the key is not one (it is a tuning value's, then).
// (trick.flip_gate, trick.catch_at and trick.catch_percent come in presets of earlier versions and are read past.)
bool set_setup_key(std::string_view key, double value) {
    if (key == "trick.flip_gate" || key == "trick.catch_at" || key == "trick.catch_percent") return true;
    const auto *one = find_setup(key);
    if (!one) return false;
    one->set(value);
    return true;
}
// Whether a built-in preset is on: the values say so, whatever switched them (a slider, a dial or another preset).
bool builtin_on(std::size_t p);
// `matching`: only the values that still hold what the built-in preset set go back (a feel taken off must not wipe a
// value the player has since set themselves).
std::string remove_preset(std::string_view name, bool matching = false) {
    auto &s = state();
    const auto wanted = lower(name);
    std::size_t count{};
    std::string removed;
    const auto put_back = [&](Entry &e) {
        if (is_locked(e)) return;
        if (e.touched) {
            set_entry(e, e.stock);
            ++count;
            return;
        }
        // A linked value at stock whose graphs were set directly (by an older version's preset).
        for (const auto index : e.drives)
            if (auto &target = s.entries[index]; target.touched && !is_locked(target)) {
                set_entry(target, target.stock);
                ++count;
            }
    };
    for (const auto &[user_name, values] : s.user) {
        if (lower(user_name) != wanted) continue;
        for (const auto &saved : values) {
            if (const auto *one = find_setup(saved.first)) {
                one->set(one->stock);
                ++count;
            }
            else if (auto *e = find_entry(saved.first)) put_back(*e);
        }
        removed = user_name;
        break;
    }
    if (removed.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    if (matching && std::abs(e.value - sane(e, rule.multiply ? e.stock * rule.amount : rule.amount)) > 1e-6 * std::max(1.0, std::abs(e.value))) continue;
                    put_back(e);
                }
            removed = preset.name;
            break;
        }
    if (removed.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    std::erase(s.active, removed);
    // Presets that are still on may share values with the one switched off: set theirs again.
    const auto still_on = s.active;
    for (const auto &other : still_on) (void)apply_preset(other);
    changed();
    return std::format("{} off: {} values put back.", removed, count);
}
std::size_t reset_all() {
    auto &s = state();
    std::size_t count{};
    for (auto &e : s.entries) {
        if (!e.touched || is_locked(e)) continue;
        set_entry(e, e.stock);
        ++count;
    }
    s.active.clear();
    return count;
}
std::string apply_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    if (wanted == "stock") {
        const auto count = reset_all();
        changed();
        return std::format("Stock: {} values put back (frozen values kept).", count);
    }
    std::size_t count{}, locked{};
    std::string applied;
    if (wanted == "map") {
        if (s.map_file.preset.empty()) return "error: this map ships no trainer preset.";
        for (const auto &[id, value] : s.map_file.preset)
            if (auto *e = find_entry(id); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
        applied = s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    }
    if (applied.empty())
        for (const auto &[user_name, values] : s.user) {
            if (lower(user_name) != wanted) continue;
            for (const auto &[key, value] : values) {
                if (set_setup_key(key, value)) ++count;
                else if (auto *e = find_entry(key); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
            }
            applied = user_name;
            break;
        }
    if (applied.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (!rule_matches(e, scale, rule)) continue;
                    if (e.kind == Kind::flag && rule.multiply) continue;
                    if (is_locked(e)) {
                        ++locked;
                        continue;
                    }
                    set_entry(e, rule.multiply ? e.stock * rule.amount : rule.amount);
                    ++count;
                }
            applied = preset.name;
            break;
        }
    if (applied.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    if (std::ranges::find(s.active, applied) == s.active.end()) s.active.push_back(applied);
    changed();
    return std::format("{}: {} values set{}.", applied, count,
                       locked ? std::format("; {} locked values left alone (untick their boxes in Tune to let presets change them)", locked) : "");
}

// ---- maps ------------------------------------------------------------------------------
MapFile read_map_file(const std::string &level) {
    MapFile result;
    const auto mods = game_directory() / L"Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"trainer.json");
        if (!json || !json->is_object()) continue;
        try {
            // The file names its level, or it stands for every level its mod adds.
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto &row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine) continue;
            result.note = json->value("note", "");
            if (json->contains("preset") && json->at("preset").is_object()) {
                const auto &preset = json->at("preset");
                result.preset_name = preset.value("name", "");
                if (preset.contains("values") && preset.at("values").is_object())
                    for (const auto &[id, value] : preset.at("values").items())
                        if (value.is_number() && result.preset.size() < 512) result.preset.emplace_back(id, value.get<double>());
            }
            if (json->contains("spots") && json->at("spots").is_array())
                for (const auto &row : json->at("spots")) {
                    if (!row.is_object() || !row.contains("position") || result.spots.size() >= 64) continue;
                    if (const auto position = read_position(row.at("position")))
                        result.spots.push_back({row.value("name", "Spot"), *position});
                }
            return result;
        } catch (...) {}
    }
    return result;
}
// The map's file is read on a thread of its own and taken by the tick when it is there: read
// here, it listed every mod's folder and opened two files in each on the client thread, as the
// player spawned (2026-10-15).
struct MapFileRead {
    std::mutex mutex;
    std::string level;
    std::optional<MapFile> file;
};
MapFileRead &map_file_read() {
    static auto *read = new MapFileRead; // process lifetime, as the threads that fill it
    return *read;
}
void start_map_file_read(const std::string &level) {
    auto &r = map_file_read();
    {
        std::lock_guard lock(r.mutex);
        r.level = level;
        r.file.reset();
    }
    if (level.empty()) return;
    try {
        std::thread([level] {
            MapFile file;
            try { file = read_map_file(level); } catch (...) {}
            auto &r = map_file_read();
            std::lock_guard lock(r.mutex);
            if (r.level == level) r.file = std::move(file);
        }).detach();
    } catch (...) {}
}
void take_map_file() {
    auto &s = state();
    auto &r = map_file_read();
    {
        std::lock_guard lock(r.mutex);
        if (!r.file || r.level != s.map) return;
        s.map_file = std::move(*r.file);
        r.file.reset();
    }
    if (!s.map_file.spots.empty() || !s.map_file.preset.empty())
        say(logging::Level::info, std::format("Trainer: the map ships {} spots and {} preset values.", s.map_file.spots.size(),
                                                s.map_file.preset.size()));
    changed(false);
}
void enter_map(const std::string &level) {
    auto &s = state();
    s.map = level;
    s.map_file = {};
    start_map_file_read(level);
    s.motion = {};
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.landing = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
    s.class_skater_wait = class_skater_wait_low;
    s.class_skater_after = 0;
    s.class_skater_due = false;
    s.class_skater_judged = true;
    s.class_list_wanted = false;
    if (!level.empty()) {
        const auto found = s.maps.find(level);
        s.profile_due = found != s.maps.end() && !found->second.preset.empty();
    }
    changed(false);
}
MapData &current_map() { return state().maps[state().map]; }
bool teleport_allowed(std::string &why) {
    if (match_blocks_markers()) {
        why = "Quit the active match before using markers or teleporting.";
        return false;
    }
    if (multiplayer_session_active() && !session_noclip_allowed() && !kit::match().teleports) {
        why = "The host has turned off teleporting in this session.";
        return false;
    }
    return true;
}
std::string go_to(const Vec3 &position, const std::string &what) {
    std::string why;
    if (!teleport_allowed(why)) return "error: " + why;
    if (!teleport_local_skater(position)) return "error: teleporting is unavailable right now.";
    state().motion.valid = false;
    return "Teleporting to " + what + ".";
}

// ---- ground-safe arrivals -------------------------------------------------------------
// The topmost collision surface at x, z (trainer_landing.h for the ray), from the client
// physics world. Empty while collision there is still streaming in.
std::optional<float> topmost_ground(float x, float z, float hint_y) {
    const auto first = landing::first_ray(hint_y);
    if (const auto hit = local_ground_height(x, z, first.top, first.bottom)) return hit;
    if (const auto retry = landing::retry_ray(first)) return local_ground_height(x, z, retry->top, retry->bottom);
    return std::nullopt;
}

// Teleports onto the surface at x, z rather than to a raw height: a map waypoint's height is
// not the ground, and arriving under it drops the skater through the map.
std::string land_at(float x, float z, float hint_y, const std::string &what) {
    auto &s = state();
    const auto ground = topmost_ground(x, z, hint_y);
    // Collision far away may not be loaded yet: go to the hint's height, and the landing
    // watch puts the skater on the surface once it streams in.
    const Vec3 to{x, ground ? *ground + landing::stand_height : hint_y + 2.0f, z};
    const auto answer = go_to(to, what);
    if (answer.starts_with("error")) return answer;
    s.landing = landing::start(x, z, to[1], GetTickCount64());
    say(logging::Level::info, ground ? std::format("Trainer: landing on the surface at {:.1f}, {:.1f}, {:.1f}.", to[0], to[1], to[2])
                                     : std::format("Trainer: no collision at {:.1f}, {:.1f} yet; going to height {:.1f} and waiting for the ground.", x, z, to[1]));
    const auto stem = answer.substr(0, answer.size() - 1);
    return ground ? stem + std::format(" (ground at {:.1f}).", *ground)
                  : stem + " (the ground there is still loading; you'll be put on it when it arrives).";
}

void update_landing(std::uint64_t now) {
    auto &s = state();
    auto &w = s.landing;
    if (!w.active || now < w.next || !s.telemetry.skater) return;
    const auto &p = s.telemetry.position;
    landing::Sample sample{.now = now, .position = p, .vertical = s.telemetry.vertical, .ground = std::nullopt, .may_resend = true};
    const float dx = p[0] - w.x, dz = p[2] - w.z;
    if (dx * dx + dz * dz <= landing::arrived_radius * landing::arrived_radius) sample.ground = topmost_ground(p[0], p[2], p[1]);
    switch (landing::step(w, sample)) {
    case landing::Step::resend: {
        const auto ground = topmost_ground(w.x, w.z, w.y);
        (void)go_to({w.x, ground ? *ground + landing::stand_height : w.y, w.z}, "the same spot");
        say(logging::Level::info, "Trainer: the teleport hadn't arrived; sent it again.");
        break;
    }
    case landing::Step::put_back:
        (void)go_to({p[0], *sample.ground + landing::stand_height, p[2]}, "the surface");
        say(logging::Level::info, std::format("Trainer: sank to {:.1f} under the surface at {:.1f}; put back on it.", p[1], *sample.ground));
        break;
    case landing::Step::landed:
        say(logging::Level::info, std::format("Trainer: landed at {:.1f}, {:.1f}, {:.1f}.", p[0], p[1], p[2]));
        break;
    case landing::Step::ended:
        say(logging::Level::info, std::format("Trainer: landing watch ended at {:.1f}, {:.1f}, {:.1f} after {} correction(s).", p[0], p[1], p[2], w.fixes));
        break;
    case landing::Step::wait: break;
    }
}

void refresh_waypoint() {
    auto &s = state();
    const auto read = read_map_waypoint(s.base);
    s.waypoint_detail = read.detail;
    if (const auto next = landing::remembered(s.waypoint, read.reading); next != s.waypoint) {
        s.waypoint = next;
        s.view_due = true;
    }
}

// ---- telemetry -------------------------------------------------------------------------
std::int64_t clock_now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double clock_seconds(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
float horizontal(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
void finish_jump(const Vec3 &landing, float landing_speed, std::int64_t landed_at) {
    auto &s = state();
    auto &m = s.motion;
    const auto air = static_cast<float>(clock_seconds(landed_at - m.takeoff_stamp));
    m.airborne = false;
    s.telemetry.airborne = false;
    if (air < 0.25f) return;
    Jump jump;
    jump.serial = s.telemetry.last.serial + 1;
    jump.air_time = air;
    jump.takeoff = m.takeoff;
    jump.landing = landing;
    jump.takeoff_speed = std::sqrt(m.takeoff_velocity[0] * m.takeoff_velocity[0] + m.takeoff_velocity[1] * m.takeoff_velocity[1] +
                                   m.takeoff_velocity[2] * m.takeoff_velocity[2]);
    jump.takeoff_angle = std::atan2(m.takeoff_velocity[1], std::max(horizontal(m.takeoff_velocity), 0.01f)) * 180.0f / std::numbers::pi_v<float>;
    jump.height = m.apex - m.takeoff[1];
    jump.distance = horizontal({landing[0] - m.takeoff[0], 0, landing[2] - m.takeoff[2]});
    jump.drop = m.takeoff[1] - landing[1];
    jump.landing_speed = landing_speed;
    jump.spin = std::abs(m.spin);
    jump.spin_rate = m.spin_rate;
    jump.flip = m.flip;
    jump.board_turn = m.board_turn;
    jump.state = m.air_state;
    s.telemetry.last = jump;
    if (jump.distance > s.telemetry.best.distance) s.telemetry.best = jump;
    // In the log for a player who is measuring: the read-out is on, or telemetry is recorded.
    if (!s.hud_jump && !s.logging) return;
    say(logging::Level::info,
        std::format("Trainer jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, "
                    "landing {:.1f} km/h at ({:.1f}, {:.1f}, {:.1f}); spin {:.0f} deg (peak {:.0f} deg/s), flip {:.0f} deg, "
                    "board {:.0f} deg/s, states {}>{}.",
                    jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle, jump.air_time, jump.height, jump.distance, jump.drop,
                    jump.landing_speed * 3.6f, landing[0], landing[1], landing[2], jump.spin, jump.spin_rate, jump.flip, jump.board_turn, m.ground_state,
                    m.air_state));
}
// Board flip tricks turn at the game's speed times the player's. The trick scripts set
// Float.Anim.TrickFlipSpeed when the board is flicked; while the skater is in the air the
// trainer keeps it at that number times the flip speed (and, with "Advanced trick speed", times
// the speed of the trick that is turning). On the ground the game's own number is put back.
// The left stick's sideways position on the controller, -1 (left) to 1 (right), 0 inside a small dead zone: Skate 3's
// grind pop reads it raw (its PhysGrindStabilityNudge is left stick X). The game state of the same name in skate.
// (Float.Intent.GrindStabilityNudge) read 0 at every pop in the playtest of 2026-10-11, so the controller is asked.
bool left_stick_raw(float &x, float &y) noexcept;
// And how far the left stick turns the board at its end. Skate 3's own code reads as about 24 degrees; the owner
// remembers turning all the way to the next grind, so the preset starts at a quarter turn.
constexpr float skate3_grind_range = 90.0f;
float left_stick_x() noexcept {
    float x{}, y{};
    if (!left_stick_raw(x, y)) return 0;
    constexpr float dead = 0.2f;
    return std::abs(x) <= dead ? 0.0f : (x - std::copysign(dead, x)) / (1.0f - dead);
}
// The left stick as the controller reports it, each axis -1 to 1 (up and right are positive). False without a controller.
bool pad_sticks(float &x, float &y, float &right_x, float &right_y) noexcept {
    using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
    static const GetState get_state = []() -> GetState {
        for (const auto name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            const auto module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) continue;
            if (const auto entry = GetProcAddress(module, "XInputGetState")) {
                GetState function{};
                std::memcpy(&function, &entry, sizeof(function));
                return function;
            }
            FreeLibrary(module);
        }
        return nullptr;
    }();
    x = y = right_x = right_y = 0;
    if (!get_state) return false;
    bool any = false;
    float most = -1;
    for (DWORD user = 0; user < 4; ++user) {
        XINPUT_STATE pad{};
        if (get_state(user, &pad) != ERROR_SUCCESS) continue;
        any = true;
        const float px = std::clamp(static_cast<float>(pad.Gamepad.sThumbLX) / 32767.0f, -1.0f, 1.0f);
        const float py = std::clamp(static_cast<float>(pad.Gamepad.sThumbLY) / 32767.0f, -1.0f, 1.0f);
        if (const float size = px * px + py * py; size > most) {
            most = size;
            x = px;
            y = py;
            right_x = std::clamp(static_cast<float>(pad.Gamepad.sThumbRX) / 32767.0f, -1.0f, 1.0f);
            right_y = std::clamp(static_cast<float>(pad.Gamepad.sThumbRY) / 32767.0f, -1.0f, 1.0f);
        }
    }
    // While the trainer leans the skater into a ramp the game's polls see a made-up left stick, and so would this
    // read: the player's own stick is what the last poll had before that.
    if (float own[2]{}; DingoSDKOverlayGrindStickLeanRaw(own)) {
        x = own[0];
        y = own[1];
    }
    return any;
}
bool left_stick_raw(float &x, float &y) noexcept {
    float right_x{}, right_y{};
    return pad_sticks(x, y, right_x, right_y);
}
// Test value (`trainer flipland <seconds>`): this long before the landing a slowed flip goes back to the game's own speed.
float flip_land_window = 0;
// Test value (`trainer flipturn <seconds>`): the turn time below, for every trick.
float flip_turn_test = 0;
// With "Flick tricks at full speed" the game's own flip speed counts as at least this (`trainer flipleast <n>` to
// test). The game sets that speed from how fast the stick was flicked, and every trick's clip turns for the same 20
// ticks, so a trick with a slow flick (the front foot impossible, the nollie impossibles, the 360 pop shuvits: 11 of 11
// still turning at the landing in the playtest of 2026-10-11, at under half a clean impossible's turn rate) needs more
// air than "Slow flips pop higher" allows for. Held up to this, they turn in the time that pop gives every other trick.
float flip_least_game = 1.0f;
// The least upward speed a pop into `trick`, slowed to `speed`, needs to land on time (0: the game's own will do).
// Every flip trick turns for about 0.31 s at the game's speed and 0.31 s / speed slowed (playtest of 2026-10-10: 14
// tricks, speeds 0.1 to 0.5), and scripted kickflips landed clean with 0.2 s more air than that. Shuvits and impossibles
// were still turning when they landed in that playtest (scripted pop shuvits were not), so they get a little more. The air a launch gives on flat ground is
// 0.04 s more than twice its upward speed over 9.8.
float air_gravity_now = 1; // x the game's gravity in the air on the board, as the last tick set it
float slow_flip_up(std::uint32_t trick, float speed) {
    if (!(speed < 1.0f)) return 0;
    const auto name = flip_trick_name(trick);
    float turn = name.find("Shuvit") != name.npos || name.find("Impossible") != name.npos ? 0.38f : 0.34f;
    if (flip_turn_test > 0) turn = flip_turn_test;
    const float air = 0.20f + turn / std::max(speed, 0.05f);
    return std::min(4.9f * air_gravity_now * (air - 0.04f), 16.5f);
}
// CurrentFlipTrick values that are a pop with no flip in it (the nollie, the ollie pops, the boned ollie, the wallies):
// the flip speed leaves them alone, as it does the ollie.
constexpr bool plain_pop(std::uint32_t trick) { return trick <= 1 || trick == 16 || trick == 31 || trick == 32 || (trick >= 33 && trick <= 35) || trick == 42; }
// The ollies among those (ollie, nollie, their pops, the boned ollie). With "Slow flips pop higher" they are slowed and
// popped like a flip trick (the owner's ask of 2026-10-11): the pop gives them the air a slowed clip hangs for.
constexpr bool ollie_pop(std::uint32_t trick) { return trick == 1 || trick == 16 || trick == 31 || trick == 32 || trick == 42; }
void drive_flip_speed(std::uintptr_t base, bool airborne, float to_landing) {
    auto &s = state();
    auto &drive = s.flip_drive;
    const auto now = read_flip_state(base, static_cast<std::uintptr_t>(s.entity));
    s.flip_live = static_cast<bool>(now);
    // The underflip's rate band follows the speed the underflip should turn at: without it the game holds every underflip
    // to 1 to 1.2.
    // Underflip speed times the underflipped trick's own (Advanced trick speed).
    const float underflip = underflip_in_force() * (now && !extras_blocked() ? own_trick_speed(underflip_trick_index(now.trick)) : 1.0f);
    const bool split = underflip_split_in_force();
    // The band is copied into each controller as it is made (0x10d9e40), so with "Underflip part only" it spans both the
    // flip's speed and the under part's from the start.
    const auto band = [&s, split, underflip](float factor) {
        const float low = split ? std::min(factor, underflip) : factor, high = split ? std::max(factor, underflip) : factor;
        if (const int bands = scale_underflip_bands(low, high); bands != s.underflip_bands) {
            s.underflip_bands = bands;
            say(logging::Level::info, std::format("Trainer: {} underflip rate controller(s) found.", bands));
        }
    };
    set_launch_trick(now ? now.trick : 0xffu, now ? now.trick_at : 0); // for the launch hook: the trick the game has in mind this tick
    // A plain ollie (trick 1) plays the same clip and reads the same number: slowed, it hangs in
    // the air (measured: 0.60 s of air at 1, 0.65 s at 0.5, 0.77 to 0.88 s at 0.25). Only a flip
    // trick is driven, and the ollies where "Slow flips pop higher" gives them that air.
    const bool flipping = now && (!plain_pop(now.trick) || (ollie_pop(now.trick) && s.flip_pop && !extras_blocked()));
    if (!now || !airborne || !flipping) {
        if (now && drive.active && now.speed == drive.written && drive.written != drive.game) {
            (void)write_flip_speed(now, drive.game);
            if (now.clamped_at) (void)write_state_number(now.clamped_at, drive.clamped);
        }
        if (now && drive.timescale_active && now.underflip_timescale == drive.timescale_written)
            (void)write_state_number(now.underflip_timescale_at, drive.timescale_game);
        (void)hold_underflip_timescale(0, 1.0f);
        arm_underflip_flip_speed(0, 1.0f);
        scale_under_part(0);
        band(split ? (extras_blocked() ? 1.0f : s.boosts.flip) : underflip);
        if (s.logging && drive.window_closed)
            say(logging::Level::info, std::format("Trainer underflip flight ended: {}.", underflip_timescale_callers()));
        drive = {};
        return;
    }
    if (now.trick) s.flip_trick_seen = now.trick;
    float factor = s.boosts.flip;
    if (!extras_blocked()) factor *= own_trick_speed(flip_trick_index(now.trick));
    if (s.logging && (now.underflip_anim != drive.underflip_anim || now.underflip_state != drive.underflip_state ||
                      now.underflip_window != drive.underflip_window))
        say(logging::Level::info, std::format("Trainer underflip states: trick {} ({}), Is.Underflip anim {} state {}, window {}, game's speed {:.3f}, "
                                              "timescale {:.3f} (game's {:.3f}), factor {:.2f}.",
                                              now.trick, flip_trick_name(now.trick), now.underflip_anim, now.underflip_state, now.underflip_window, now.speed,
                                              now.underflip_timescale_at ? now.underflip_timescale : -1.0f, drive.timescale_game,
                                              factor));
    if (s.logging && drive.underflip_anim == 1 && now.underflip_anim == 0)
        say(logging::Level::info, std::format("Trainer underflip timescale asked by: {}.", underflip_timescale_callers()));
    if (drive.underflip_window == 1 && now.underflip_window == 0) drive.window_closed = true;
    const bool in_underflip = now.underflip_anim == 1 || now.underflip_state == 1;
    // "Underflip part only": the underflip's own flip turns one way, the under part back the other. The way it turned
    // is summed from the third tick of the underflip (the first two read a jolt as the clip changes, measured
    // 2026-10-19), and two ticks in a row against it start the under part.
    if (split && in_underflip && !drive.under_part) {
        const auto &w = s.board_spin;
        const float rate = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (++drive.underflip_ticks > 2 && rate > 200.0f) {
            const float way = std::sqrt(drive.flip_way[0] * drive.flip_way[0] + drive.flip_way[1] * drive.flip_way[1] + drive.flip_way[2] * drive.flip_way[2]);
            const float along = way > 0 ? (w[0] * drive.flip_way[0] + w[1] * drive.flip_way[1] + w[2] * drive.flip_way[2]) / way : 0.0f;
            if (drive.flip_way_ticks >= 4 && along < -150.0f) {
                if (++drive.turned_back >= 2) drive.under_part = true;
            } else {
                drive.turned_back = 0;
                if (along >= 0 || drive.flip_way_ticks < 4) {
                    for (std::size_t k = 0; k < 3; ++k) drive.flip_way[k] += w[k];
                    ++drive.flip_way_ticks;
                }
            }
        }
        if (drive.under_part) {
            // The copy that started the underflip was scaled by the flip's factor: from here the under part's.
            const float to = std::clamp(now.speed / std::max(factor, flip_low) * underflip, 0.0f, 1000.0f);
            if (s.logging)
                say(logging::Level::info, std::format("Trainer underflip: under part from tick {} of the underflip, speed {:.3f} -> {:.3f} (x{:.2f}).",
                                                      drive.underflip_ticks, now.speed, to, underflip));
            (void)write_flip_speed(now, to);
        }
    }
    // The under part plays on rate controllers that took their rate as the underflip started: each one's rate is scaled
    // from here to the landing.
    scale_under_part(split && in_underflip && drive.under_part ? underflip : 0.0f);
    // What the underflip turns at: Underflip speed for all of it, or the flip's own speed until the under part.
    const float underflip_rate = split && !drive.under_part ? factor : underflip;
    band(underflip_rate);
    // The game sets TrickFlipSpeed afresh as the underflip starts, a tick after the window closes: that set is scaled.
    if (drive.window_closed && underflip_rate != 1.0f) arm_underflip_flip_speed(now.speed_at, underflip_rate);
    else arm_underflip_flip_speed(0, 1.0f);
    drive.underflip_anim = now.underflip_anim;
    drive.underflip_state = now.underflip_state;
    drive.underflip_window = now.underflip_window;
    // The underflip itself turns at the game's speed times that factor alone (the game's copy that starts it was scaled
    // as it was made): the trainer leaves it there.
    if (in_underflip) return;
    // "Slow flips pop higher" for a trick the game chose after the pop (seen out of manuals and grinds).
    if (s.flip_pop && !extras_blocked() && (!drive.active || drive.trick != now.trick)) {
        float was{}, time{};
        if (const float up = slow_flip_up(now.trick, factor); up > 0 && raise_flight(up, was, time))
            say(logging::Level::info, std::format("Trainer: slow flip chosen {:.3f} s after the pop: up speed {:.2f} -> {:.2f} m/s.", time, was, up));
    }
    drive.trick = now.trick;
    // A slowed clip also plays its landing slowly, so the skater lands late and sinks (measured: 0.07 s late at x 0.5,
    // 0.25 to 0.57 s at x 0.25). Close to the ground the clip goes back to the game's own speed.
    if (factor < 1 && flip_land_window > 0 && to_landing < flip_land_window) factor = 1;
    // A number that is not the trainer's own is the game's: the flight's first, or a new trick's.
    if (!drive.active || now.speed != drive.written) drive.game = now.speed;
    if (!drive.active) drive.clamped = now.clamped;
    drive.active = true;
    drive.factor = factor;
    const bool held_up = s.flip_flick && !extras_blocked() && !in_underflip; // the floor would undo Underflip speed
    s.flip_game_seen = drive.game;
    s.flip_clamped_seen = now.clamped_at ? now.clamped : -1.0f;
    drive.written = std::clamp((held_up ? std::max(drive.game, flip_least_game) : drive.game) * factor, 0.0f, 1000.0f);
    if (now.speed != drive.written || (now.clamped_at && now.clamped != drive.written)) (void)write_flip_speed(now, drive.written);
    // The underflip does not turn at that speed but at UnderflipTimeScale: Underflip speed (times the flip speed) goes there
    // for the whole flight, so the number is in place whenever the game's underflip reads it, and the flip before it is
    // left alone.
    if (now.underflip_timescale_at) {
        const float timescale = factor * underflip;
        if (hold_underflip_timescale(now.underflip_timescale_at, 1.0f)) { // read and logged only: it is not the underflip's rate
            // Held in the game's lookup: what the tick reads is already the trainer's.
            if (s.logging && now.underflip_anim == 1 && ++drive.timescale_sets <= 12)
                say(logging::Level::info, std::format("Trainer underflip timescale: game's {:.3f}, read {:.3f}, held x{:.2f}.", underflip_timescale_game(),
                                                      now.underflip_timescale, timescale));
            drive.timescale_active = false;
        } else if (!drive.timescale_active || now.underflip_timescale != drive.timescale_written) {
            if (s.logging && drive.timescale_active && ++drive.timescale_sets <= 8)
                say(logging::Level::info, std::format("Trainer underflip timescale: the game set {:.3f} (the trainer had {:.3f}), Is.Underflip {}, window {}.",
                                                      now.underflip_timescale, drive.timescale_written, now.underflip_anim, now.underflip_window));
            drive.timescale_game = now.underflip_timescale;
        }
        drive.timescale_active = true;
        drive.timescale_written = std::clamp(drive.timescale_game * timescale, 0.0f, 1000.0f);
        if (now.underflip_timescale != drive.timescale_written) (void)write_state_number(now.underflip_timescale_at, drive.timescale_written);
    }
}
void observe(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.telemetry;
    auto &m = s.motion;
    overlay::DebugModel skater;
    std::array<float, 16> matrix{};
    bool found = false;
    if (now >= s.next_skater) {
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
            found = skater.skater_position_valid;
        } catch (...) {
            s.next_skater = now + 250; // no skater: menus, loading, a respawn
        }
    }
    if (!found) {
        if (t.skater) { t.skater = false; m.valid = false; s.entity = 0; }
        return;
    }
    s.entity = skater.skater_identity;
    watch_physics_state(client, s.entity);
    const auto watch = watched_physics_state();
    const auto stamp = clock_now();
    const Vec3 position = skater.skater_position;
    t.skater = true;
    t.position = position;
    // Rows: right, up, forward, position.
    t.heading = std::atan2(matrix[8], matrix[10]) * 180.0f / std::numbers::pi_v<float>;
    if (t.heading < 0) t.heading += 360.0f;
    t.physics_state = watch.valid ? watch.state : 0;
    if (watch.valid) ++s.states[watch.state];
    // A hippy jump leaves the board (504) straight from riding (100): measured. A plain
    // dismount does the same but is not rising, so it is left alone.
    if (watch.valid && watch.state != s.last_state) {
        // Off a grind or a slide the board is in a 400s state (401 and 403 seen in a playtest) and the hippy leaves it the same
        // way; that case is built from the riding one and not measured yet.
        const bool from_board = s.last_state == 100 || (s.last_state >= 400 && s.last_state < 500);
        if (watch.state == addr::no_bail::offboard_physics_state && from_board && s.boosts.hippy != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.hippy); // height goes with the square of the speed
            s.boost_until = now + 250;
            s.boost_name = "hippy jump";
        }
        s.last_state = watch.state;
    }
    if (s.boost_factor > 0) {
        const auto result = take_jump_scale_result();
        if (result.outcome > 0) {
            say(logging::Level::info, std::format("Trainer: {} boosted: up speed {:.2f} -> {:.2f} m/s.", s.boost_name, result.up_speed,
                                                  result.up_speed * s.boost_factor));
            s.boost_factor = 0;
        } else if (now >= s.boost_until || result.outcome == -2) {
            s.boost_factor = 0;
        } else {
            // The factor comes from the multipliers in force, so a session's rules are already in it.
            (void)queue_jump_scale(client, s.entity, s.boost_factor); // again each tick until the skater is rising
        }
    }
    const auto seconds = m.valid ? clock_seconds(stamp - m.stamp) : 0.0;
    const Vec3 step{position[0] - m.position[0], position[1] - m.position[1], position[2] - m.position[2]};
    const float distance=std::sqrt(step[0]*step[0]+step[1]*step[1]+step[2]*step[2]);
    if (reset_motion_segment(m.valid,seconds,distance,skater_push::holding() || skater_push::pending() || local_skater_teleport_pending())) {
        // First sample, a hitch, or a teleport: nothing can be measured across it.
        m = {};
        m.valid = true;
        m.position = position;
        m.stamp = stamp;
        t.airborne = false;t.speed=0;t.vertical=0;
        t.air_time = t.height = 0;
        return;
    }
    const auto dt = static_cast<float>(seconds);
    const Vec3 raw{step[0] / dt, step[1] / dt, step[2] / dt};
    const Vec3 velocity{m.velocity[0] * 0.5f + raw[0] * 0.5f, m.velocity[1] * 0.5f + raw[1] * 0.5f, m.velocity[2] * 0.5f + raw[2] * 0.5f};
    // An on-foot jump: off the board, level at the last tick and now rising faster than stairs
    // or a slope can lift a runner.
    if (watch.valid && watch.state == addr::no_bail::offboard_physics_state) {
        if (s.offboard_grounded && raw[1] > 2.2f && s.boost_factor == 0 && s.boosts.offboard != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.offboard);
            s.boost_until = now + 250;
            s.boost_name = "off-board jump";
        }
        s.offboard_grounded = std::abs(raw[1]) < 0.6f;
    } else {
        s.offboard_grounded = false;
    }
    // The skater's physics says when it is in the air: its states 200-299 are the pop and
    // the flight (100s ride the ground, 300 is a wipeout, 500s are off the board).
    const bool flying = watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
    // Rotation in the air: about the vertical (spins) and of the up axis (flips and rolls).
    const Vec3 forward{matrix[8], matrix[9], matrix[10]}, up{matrix[4], matrix[5], matrix[6]};
    if (m.airborne && m.axes_valid) {
        const float yaw = std::atan2(m.forward[2] * forward[0] - m.forward[0] * forward[2], m.forward[0] * forward[0] + m.forward[2] * forward[2]);
        const float degrees = yaw * 180.0f / std::numbers::pi_v<float>;
        // A ragdoll or a pose snap turns further in one tick than any trick does: leave those out.
        if (std::abs(degrees) < 45.0f) {
            m.spin += degrees;
            m.spin_rate = std::max(m.spin_rate, std::abs(degrees) / dt);
        }
        const float cosine = std::clamp(m.up[0] * up[0] + m.up[1] * up[1] + m.up[2] * up[2], -1.0f, 1.0f);
        if (const float tumble = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>; tumble < 45.0f) m.flip += tumble;
    }
    m.forward = forward;
    m.up = up;
    m.axes_valid = true;
    if (!flying && watch.valid) m.ground_state = watch.state;
    if (flying && !m.airborne) {
        m.spin = m.spin_rate = m.flip = m.board_turn = 0;
        m.air_state = watch.state;
        m.airborne = true;
        m.takeoff = m.position;
        m.takeoff_velocity = velocity;
        m.takeoff_stamp = m.stamp;
        m.apex = std::max(m.position[1], position[1]);
    }
    {
        // Seconds until the skater is back at the height they left from, by the flight's own arc.
        float to_landing = 1e9f;
        if (m.airborne) {
            const float rise = velocity[1], above = position[1] - m.takeoff[1], gravity = 9.8f * air_gravity_now, under = rise * rise + 2 * gravity * above;
            to_landing = under > 0 ? (rise + std::sqrt(under)) / gravity : 0.0f;
        }
        drive_flip_speed(base, m.airborne, to_landing);
    }
    if (m.airborne) {
        m.apex = std::max(m.apex, position[1]);
        t.air_time = static_cast<float>(clock_seconds(stamp - m.takeoff_stamp));
        t.height = m.apex - m.takeoff[1];
        if (!flying)
            finish_jump(position, std::sqrt(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1] + m.velocity[2] * m.velocity[2]),
                        stamp);
    }
    t.airborne = m.airborne;
    t.speed = horizontal(velocity);
    t.vertical = velocity[1];
    t.top_speed = std::max(t.top_speed, t.speed);
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    if (turning) {
        try {
            const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
            std::array<float, 3> spin{};
            if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                board_turn = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                if (!std::isfinite(board_turn) || board_turn > 1.0e5f) board_turn = 0;
                else
                    for (std::size_t k = 0; k < 3; ++k) s.board_spin[k] = spin[k] * 180.0f / std::numbers::pi_v<float>;
            }
        } catch (...) {}
        if (m.airborne) m.board_turn = std::max(m.board_turn, board_turn);
    }
    if (!m.airborne) s.board_spin = {};
    if (s.logging && s.log) {
        s.log << std::format("{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{},{},{:.0f}\n", clock_seconds(stamp - s.log_start), position[0],
                             position[1], position[2], t.speed, t.vertical, t.heading, t.airborne ? 1 : 0, t.physics_state, board_turn);
    }
    // A bail: back to the selected marker once the skater has had time to fall.
    if (watch.valid && (!s.wipeouts_known || watch.wipeouts != s.seen_wipeouts)) {
        if (s.wipeouts_known && s.auto_return && !match_blocks_markers()) {
            const bool marked = current_map().markers[static_cast<std::size_t>(s.slot)].set;
            if (marked) s.return_at = now + static_cast<std::uint64_t>(s.return_delay * 1000.0f);
            say(logging::Level::info, marked ? std::format("Trainer: bail seen; back to marker {} in {:.1f} s.", s.slot + 1, s.return_delay)
                                             : std::format("Trainer: bail seen, but marker {} is empty.", s.slot + 1));
        }
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
    }
}
void set_logging(bool on) {
    auto &s = state();
    if (s.log.is_open()) s.log.close();
    s.logging = false;
#if defined(DINGOSDK_LEAN_PLAYTEST)
    (void)on;
#else
    if (!on) return;
    const auto directory = data_directory() / L"telemetry";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string name = s.map.empty() ? "unknown" : s.map.substr(s.map.find_last_of('/') + 1);
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    SYSTEMTIME time;
    GetLocalTime(&time);
    const auto file = directory / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", name, time.wYear, time.wMonth, time.wDay, time.wHour,
                                              time.wMinute, time.wSecond);
    s.log.open(file, std::ios::binary | std::ios::trunc);
    if (!s.log) return;
    s.log << "seconds,x,y,z,speed_mps,vertical_mps,heading_deg,airborne,physics_state,board_turn_dps\n";
    s.log_start = clock_now();
    s.logging = true;
#endif
}
void shortcuts() {
    auto &s = state();
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input);
    const auto buttons = input.available ? input.buttons : 0;
    const auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    if (!s.pad_shortcuts || (buttons & (xinput_lb | xinput_rb)) != (xinput_lb | xinput_rb)) return;
    std::string result;
    if (pressed & xinput_up) result = command("marker", {"save"});
    else if (pressed & xinput_down) result = command("marker", {"go"});
    else if (pressed & xinput_left) result = command("slot", {std::to_string((s.slot + static_cast<int>(marker_slots) - 1) % static_cast<int>(marker_slots) + 1)});
    else if (pressed & xinput_right) result = command("slot", {std::to_string((s.slot + 1) % static_cast<int>(marker_slots) + 1)});
    if (!result.empty()) overlay::notify(overlay::NoticeLevel::info, "TRAINER", result);
}

// ---- self test -------------------------------------------------------------------------
void test_line(bool ok, const std::string &text) {
    auto &s = state();
    if (!ok) ++s.test.failures;
    say(ok ? logging::Level::info : logging::Level::error, std::format("trainer selftest: {} {}", ok ? "OK" : "FAIL", text));
}
void run_selftest(std::uint64_t now) {
    auto &s = state();
    auto &t = s.test;
    if (!t.step || now < t.until) return;
    tuning::Values live;
    switch (t.step) {
    case 1: {
        test_line(s.model != nullptr, std::format("game tuning read ({} values, {} curves)", s.model ? s.model->fields.size() : 0,
                                                  s.model ? s.model->curve_slots.size() : 0));
        std::size_t named{};
        if (s.model) named = static_cast<std::size_t>(std::ranges::count_if(s.model->field_names, [](const std::string &n) { return !n.empty(); }));
        test_line(s.model && named == s.model->fields.size(), std::format("field names ({} of {})", named, s.model ? s.model->fields.size() : 0));
        test_line(s.ready && s.asset, "running tuning asset found");
        test_line(s.telemetry.skater, "local skater found");
        // The first plain real that is not zero and that the player has not touched.
        t.entry = s.entries.size();
        for (std::size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].kind == Kind::real && !s.entries[i].detail && !s.entries[i].touched && std::abs(s.entries[i].stock) > 0.01) {
                t.entry = i;
                break;
            }
        if (!s.ready || t.entry == s.entries.size() || !editable()) {
            test_line(false, "no value to write (tuning not ready or locked by a session)");
            t.step = 4;
            break;
        }
        auto &e = s.entries[t.entry];
        t.before = e.stock;
        t.target = e.stock * 1.01;
        set_entry(e, t.target);
        t.step = 2;
        t.until = now + 1200;
        break;
    }
    case 2: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && std::abs(read_field(live.image, e) - static_cast<float>(t.target)) < 1e-4 * std::max(1.0, std::abs(t.target)),
                  std::format("write and hold {} = {:.5g}", e.id, t.target));
        set_entry(e, t.before);
        t.step = 3;
        t.until = now + 1200;
        break;
    }
    case 3: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && read_field(live.image, e) == static_cast<float>(t.before) && !e.touched, std::format("restore {} = {:.5g}", e.id, t.before));
        t.step = 4;
        break;
    }
    case 4: {
        if (!s.telemetry.skater) {
            test_line(false, "teleport (no skater)");
            t.step = 6;
            break;
        }
        t.origin = s.telemetry.position;
        const Vec3 target{t.origin[0] + 3.0f, t.origin[1] + 1.0f, t.origin[2]};
        const auto result = go_to(target, "a test point");
        test_line(!result.starts_with("error"), "teleport request: " + result);
        t.origin = target;
        t.step = result.starts_with("error") ? 6 : 5;
        t.until = now + 700;
        break;
    }
    case 5: {
        const auto &p = s.telemetry.position;
        // The skater may already be rolling away from where it was put.
        const float away = horizontal({p[0] - t.origin[0], 0, p[2] - t.origin[2]});
        test_line(s.telemetry.skater && away < 25.0f, std::format("teleport put the skater {:.2f} m from the target", away));
        t.step = 6;
        t.until = now + 1500;
        break;
    }
    default:
        say(t.failures ? logging::Level::error : logging::Level::info,
            std::format("trainer selftest: DONE {} failures", t.failures));
        t = {};
        changed(false);
        break;
    }
}

// ---- transition skating ------------------------------------------------------------------------
// Bowls, quarter pipes, vert and spines: the trainer's own help back into the ramp, and Skate 3's numbers for it.
// What is known of skate. here comes from its data, not from play (2026-10-11): the timed lean into the ramp Skate 3
// ended a coping grind with is still in the game but switched off (the exit timers are -1), a grind's exit points
// straight across the coping, and nothing measured says what a hang-up is. So every lever below reports what it saw
// (`Trainer transition:` lines) and each is a first setting.
constexpr std::string_view transition_preset = "Transition: Skate 3";
// "Transition feels like Skate 3": the lean is the share of the stick the game is shown (0.8 at most: past 0.9 the
// game takes the lean as the wish to leave the grind); Skate 3 kept all the speed along the coping and nudged the board
// at most 1 m/s across it; its hang-up push; 70 frames before the same edge could be caught again; its handplant exit.
constexpr float skate3_tr_lean = 0.6f, skate3_tr_carry = 1.0f, skate3_tr_drop = 1.0f, skate3_tr_unstick = 1.0f, skate3_tr_lockout = 1.17f, skate3_tr_plant = 1.0f;
// The sign of each stick axis the lean is sent on (`trainer transition leanframe <x> <y>`): the camera's axes as the
// game reads the stick against them. Not measured yet.
float lean_frame_x = 1.0f, lean_frame_y = 1.0f;
bool transition_stock() {
    auto &s = state();
    return s.tr_lean == 0.0f && s.tr_carry == 0.0f && s.tr_unstick == 0.0f && s.tr_lockout == 0.0f && !s.tr_spine && s.tr_plant == 0.0f &&
           std::ranges::all_of(s.tr_setting, [](std::int8_t mode) { return mode < 0; });
}
void reset_transition() {
    auto &s = state();
    s.tr_lean = s.tr_carry = s.tr_unstick = s.tr_lockout = s.tr_plant = 0.0f;
    s.tr_drop = 1.0f;
    s.tr_spine = false;
    s.tr_setting.fill(-1);
}
// How far the skater reaches for an edge from the air, and what an edge is worth as a landing. Held at 0 for a moment
// (after a coping grind, during a flight up and over a lip) the far coping of a spine or the coping just left cannot
// catch the board; the player's own numbers come back after.
constexpr std::string_view air_grind_values[]{"physicstrajectory.scoregrind", "physicsgrindsair.maxdist50_50", "physicsgrindsair.maxdist5_0",
                                              "physicsgrindsair.maxdisttipslide", "physicsgrindsair.maxdistboardslide"};
void hold_air_grinds(bool off) {
    auto &s = state();
    if (!s.ready || s.enforced) return;
    bool wrote = false;
    for (const auto key : air_grind_values) {
        const auto *e = find_entry(key);
        if (!e || e->frozen || e->class_field >= 0 || e->kind != Kind::real || static_cast<std::size_t>(e->offset) + e->size > s.want.image.size()) continue;
        const double wanted = off ? 0.0 : e->touched ? e->value : e->stock;
        if (read_field(s.want.image, *e) == static_cast<double>(static_cast<float>(wanted))) continue;
        write_field(s.want.image, *e, wanted);
        wrote = true;
    }
    if (wrote) s.write_due = true;
    s.tr.air_grinds_off = off;
}
void drive_transition(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.tr;
    const auto &m = s.motion;
    const bool blocked = extras_blocked();
    for (std::size_t i = 0; i < transition_settings.size(); ++i) force_transition_setting(i, blocked ? -1 : s.tr_setting[i]);
    for (std::size_t i = 0; i < foot_settings.size(); ++i) force_foot_setting(i, blocked ? -1 : s.foot_setting[i]);
    if (now >= s.tr_option_at) {
        // The game's own options: whatever its Settings menu (or the trainer) last set.
        s.tr_option_at = now + 500;
        if (const auto held = profile_runtime::local_game_option(grind_assist_setting); (held ? static_cast<int>(std::lround(*held)) : -1) != s.grind_assist) {
            s.grind_assist = held ? static_cast<int>(std::lround(*held)) : -1;
            s.view_due = true;
        }
        for (std::size_t i = 0; i < transition_options.size(); ++i) {
            const auto held = profile_runtime::local_game_option(transition_options[i].setting);
            const int value = held ? static_cast<int>(std::lround(*held)) : transition_options[i].stock;
            if (value != s.tr_option[i]) {
                s.tr_option[i] = value;
                s.view_due = true;
            }
        }
        const auto own = [&](std::string_view setting, bool stock, std::int8_t &shown) {
            const auto held = profile_runtime::local_game_option(setting);
            const auto value = static_cast<std::int8_t>(held ? *held != 0 : stock);
            if (value != shown) {
                shown = value;
                s.view_due = true;
            }
        };
        for (std::size_t i = 0; i < grind_settings.size(); ++i) own(grind_settings[i].setting, grind_settings[i].stock, s.grind_own[i]);
        for (std::size_t i = 0; i < transition_settings.size(); ++i) own(transition_settings[i].setting, transition_settings[i].stock, s.tr_own[i]);
    }
    const auto watch = watched_physics_state();
    const std::uint32_t physics = watch.valid && s.telemetry.skater && m.valid ? watch.state : 0;
    const bool riding = physics >= 100 && physics < 200, grinding = (physics >= 400 && physics < 500) || physics == 603;
    const bool flying = physics >= 200 && physics < 300;
    const auto &v = m.velocity;
    const float over = std::hypot(v[0], v[2]), speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    // Steep ground: the skater stands out from the wall, so the level part of their up axis points into the ramp. A
    // skater held upright on a wall still climbs or drops steeply: the wall is then ahead going up and behind coming down.
    if (riding && m.axes_valid) {
        float out_x = m.up[0], out_z = m.up[2], length = std::hypot(out_x, out_z);
        bool steep = m.up[1] < 0.8f && length > 0.1f;
        if (!steep && speed > 1.5f && std::abs(v[1]) > 0.6f * speed && over > 0.2f) {
            const float away = v[1] > 0 ? -1.0f : 1.0f;
            out_x = away * v[0];
            out_z = away * v[2];
            length = over;
            steep = true;
        }
        if (steep) {
            t.steep_at = now;
            t.steep_out = {out_x / length, out_z / length};
        }
    }
    const bool on_transition = t.steep_at && now - t.steep_at < 3000;
    // The speed along the lip (level, square to the way out of the wall) and across it (up the wall and over).
    const float out_speed = v[0] * t.steep_out[0] + v[2] * t.steep_out[1];
    const float lip_along = std::hypot(v[0] - out_speed * t.steep_out[0], v[2] - out_speed * t.steep_out[1]);
    if (grinding && !t.grinding) {
        // A grind that begins within a second of steep ground is on a coping.
        t.coping = t.steep_at && now - t.steep_at < 1000;
        if (t.coping) {
            t.ramp = t.steep_out;
            t.edge = {-t.ramp[1], t.ramp[0]};
            t.in_speed = speed;
            t.in_angle = std::atan2(std::hypot(out_speed, v[1]), std::max(lip_along, 0.001f)) * 180.0f / std::numbers::pi_v<float>;
        }
    }
    if (grinding && t.coping) {
        // The coping runs the way the skater travels on it (a bowl's coping curves); the ramp stays on the side it was on.
        if (over > 1.0f) t.edge = {v[0] / over, v[2] / over};
        const float lean_on_edge = t.ramp[0] * t.edge[0] + t.ramp[1] * t.edge[1];
        const float side_x = t.ramp[0] - lean_on_edge * t.edge[0], side_z = t.ramp[1] - lean_on_edge * t.edge[1];
        if (const float length = std::hypot(side_x, side_z); length > 0.2f) t.ramp = {side_x / length, side_z / length};
    } else if (!grinding) {
        if (t.coping && t.grinding && physics != 200 && physics != 300 && !(physics >= 500 && physics < 600)) t.left_at = now;
        t.coping = false;
        // For a handplant, and for a flight: the wall last ridden.
        t.ramp = t.steep_out;
        t.edge = {-t.ramp[1], t.ramp[0]};
    }
    // A flight straight up and over a lip (an air, a spine transfer): little of its speed runs along the lip. A carve
    // into a coping grind leaves the wall along the lip and keeps its reach for the coping.
    if (flying && !t.flying) t.flight_off = s.tr_spine && !blocked && on_transition && v[1] > 2.0f && lip_along < 0.5f * speed;
    if (t.flight_off && !flying) {
        if (t.flying) t.flight_off_until = now + 300;
        else if (now >= t.flight_off_until) t.flight_off = false;
    }
    const bool locked = s.tr_lockout > 0.0f && !blocked && t.left_at && now - t.left_at < static_cast<std::uint64_t>(s.tr_lockout * 1000.0f);
    const bool air_off = locked || (t.flight_off && s.tr_spine && !blocked);
    if (air_off != t.air_grinds_off || air_off) hold_air_grinds(air_off);
    // The lean: the left stick the game is shown, pushed toward the ramp, more the faster the grind runs along the
    // coping. The game reads that stick against the camera, so the ramp's side is turned into the camera's axes.
    const float along_speed = std::abs(v[0] * t.edge[0] + v[2] * t.edge[1]);
    const float lean_wanted = t.coping && !blocked ? s.tr_lean * 0.8f * std::clamp((along_speed - 1.0f) / 4.0f, 0.0f, 1.0f) : 0.0f;
    t.lean += (lean_wanted - t.lean) * 0.15f;
    if (blocked) t.lean = 0.0f;
    float lean_x = 0, lean_y = 0;
    bool leaning = false;
    if (t.coping && std::abs(t.lean) > 0.02f) {
        std::array<float, 16> camera{};
        if (read_local_camera_transform(base, client, camera)) {
            // Rows: right, up, forward; a camera looks back along its forward row.
            const float right = std::hypot(camera[0], camera[2]), ahead = std::hypot(camera[8], camera[10]);
            if (right > 0.1f && ahead > 0.1f) {
                lean_x = (t.ramp[0] * camera[0] + t.ramp[1] * camera[2]) / right * t.lean * lean_frame_x;
                lean_y = -(t.ramp[0] * camera[8] + t.ramp[1] * camera[10]) / ahead * t.lean * lean_frame_y;
                leaning = true;
            }
        }
    }
    DingoSDKOverlaySetGrindStickLean(leaning, lean_x, lean_y);
    TransitionAssist assist;
    if (!blocked) {
        assist.carry = s.tr_carry;
        assist.drop_speed = s.tr_drop;
        assist.unstick = s.tr_unstick;
        assist.plant = s.tr_plant;
    }
    assist.coping = t.coping;
    assist.transition = on_transition;
    assist.edge = t.edge;
    assist.ramp = t.ramp;
    assist.measure = s.hud_jump || s.logging;
    set_transition_assist(client, s.entity, assist);
    if (const auto report = transition_report(); report.sequence != t.reports) {
        t.reports = report.sequence;
        const auto off_coping = [](float along, float across, float down) {
            return std::atan2(std::hypot(across, down), std::max(std::abs(along), 0.001f)) * 180.0f / std::numbers::pi_v<float>;
        };
        if (report.kind == 1)
            say(logging::Level::info, std::format("Trainer transition: coping grind left without a pop (state {} -> {}, then {}). It began at {:.0f} deg to the coping, {:.1f} m/s. "
                                                  "Last step on it: {:.2f} m/s along, {:.2f} into the ramp. A third of a second later the game had {:.2f} along, {:.2f} into the ramp, "
                                                  "{:.2f} down = {:.0f} deg off the coping; the skater left with {:.2f} along, {:.2f} into the ramp = {:.0f} deg. Hung {} steps; "
                                                  "speed put back on {} steps, pushed on {}; {:.2f} m lower after a third of a second, {:.2f} m after one and a half.",
                                                  report.from, report.to, report.to_last, t.in_angle, t.in_speed, report.in_along, report.in_across, report.game_along,
                                                  report.game_across, report.out_down, off_coping(report.game_along, report.game_across, report.out_down), report.out_along,
                                                  report.out_across, off_coping(report.out_along, report.out_across, report.out_down), report.hung, report.carried,
                                                  report.kicked, report.drop, report.fall));
        else if (report.kind == 2)
            say(logging::Level::info, std::format("Trainer transition: handplant (state {} -> {}, then {}): {:.2f} m lost inside the plant; it let go at {:.2f} m/s into the ramp, "
                                                  "{:.2f} along the lip, {:.2f} down; a fifth of a second later {:.2f} into the ramp (the game's own {:.2f}), {:.2f} along, "
                                                  "{:.2f} down; {:.2f} m below the plant's top a second and a half after. Hung {} steps; speed changed on {} steps, pushed on {}.",
                                                  report.from, report.to, report.to_last, report.drop, report.in_across, report.in_along, report.in_down, report.out_across,
                                                  report.game_across, report.out_along, report.out_down, report.fall, report.hung, report.carried, report.kicked));
        else if (report.kind == 3)
            say(logging::Level::info, std::format("Trainer transition: a landing off a transition hung (state {} -> {}, then {}): {} steps slower than 1.1 m/s, pushed on {}; "
                                                  "the flight came in at {:.2f} m/s over the ground, {:.2f} down.",
                                                  report.from, report.to, report.to_last, report.hung, report.kicked, report.in_along, report.in_down));
    }
    t.grinding = grinding;
    t.flying = flying;
}
bool builtin_on(std::size_t p) {
    auto &s = state();
    const auto &preset = builtin_presets()[p];
    bool any = false;
    for (const auto &[index, r] : preset_targets()[p]) {
        const auto &e = s.entries[index];
        const auto &rule = preset.rules[r];
        const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
        any = true;
        // To the two decimals a dial shows: dragging one to the preset's number switches it on.
        if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) return false;
    }
    return any;
}
bool builtin_on(std::string_view name) {
    const auto &presets = builtin_presets();
    for (std::size_t p = 0; p < presets.size(); ++p)
        if (presets[p].name == name) return builtin_on(p);
    return false;
}
void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->session_enforced = s.enforced;
    next->session_stock = s.competition;
    next->boosts_blocked = !s.enforced && multiplayer_session_active() && !session_boosts_allowed();
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t topic{};
        bool trial{};
        const auto friendly = essential_name(e.key, &rank, &topic, &trial);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, topic,
                              trial, e.used});
        next->rows.back().help = essential_help(e.key);
        // Grind Assist (the game's own setting, the first row of GRINDS) works in the game's grind code on top of these: at 0, 50
        // and 100 every tuning and class value read the same (trainer livedump, 2026-10-24), so it changes none of them.
        static constexpr std::string_view assisted[]{"physicsmode.grindlockdist", "physicsgrindsair.maxdist50_50", "physicsgrindsair.maxdist5_0",
                                                     "physicsgrindsair.maxdisttipslide", "physicsgrindsair.maxdistboardslide",
                                                     "physicstrajectory.grindlandingmaxangle", "physicstrajectory.grindmaxspeedsqr",
                                                     "physicstrajectory.grindmaxspeeddownontogrind", "physicstrajectory.scoregrind"};
        if (std::ranges::find(assisted, e.key) != std::end(assisted))
            next->rows.back().help += std::string(next->rows.back().help.empty() ? "" : " ") +
                "Your game's Grind Assist setting (Grind lock-on, the first row of GRINDS) also helps grinds lock on: it works on top of "
                "this value and does not change it.";
        if (e.touched) {
            ++next->touched;
            ++next->changed[topic];
        }
        if (e.frozen) ++next->locks;
        if (std::ranges::find(next->groups, e.group) == next->groups.end()) next->groups.push_back(e.group);
    }
    const auto is_active = [&](std::string_view name) { return std::ranges::find(s.active, name) != s.active.end(); };
    next->presets.push_back({"Stock", "Everything back to how this map loaded (frozen values stay).", true, s.active.empty() && !next->touched});
    {
        // A built-in preset is on when the values say so, whatever switched them: a slider, a
        // dial or another preset.
        const auto &presets = builtin_presets();
        const auto &targets = preset_targets();
        for (std::size_t p = 0; p < presets.size(); ++p) {
            const auto &preset = presets[p];
            PresetRow row{std::string(preset.name), std::string(preset.note), true, false};
            const Entry *headline{};
            for (const auto &[index, r] : targets[p])
                if (!headline && r == 0) headline = &s.entries[index];
            row.active = builtin_on(p);
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.topic = dial.topic;
            if (!preset.rules.empty() && preset.rules[0].multiply && headline && headline->stock != 0 && !dial.title.empty()) {
                row.dial = true;
                row.amount = preset.rules[0].amount;
                row.factor = headline->value / headline->stock;
            }
            next->presets.push_back(std::move(row));
        }
    }
    for (const auto &[name, values] : s.user) next->presets.push_back({name, std::format("{} values", values.size()), false, is_active(name)});
    next->slot = s.slot;
    next->auto_return = s.auto_return;
    next->hippy_height = s.hippy_height;
    next->nocomply_height = s.nocomply_height;
    next->manual_height = s.manual_height;
    next->gravity = s.gravity;
    next->gravity_live = s.gravity_live;
    next->gravity_landing_own = s.gravity_landing_own;
    next->gravity_landing = s.gravity_landing;
    next->gravity_timing = s.gravity_timing;
    next->grind_side = s.grind_side;
    next->grind_push = s.grind_push;
    next->grind_range = s.grind_range;
    next->grind_mode = s.grind_mode;
    next->grind_setting = s.grind_setting;
    next->grind_live = s.grind_live;
    next->tr_lean = s.tr_lean;
    next->tr_carry = s.tr_carry;
    next->tr_drop = s.tr_drop;
    next->tr_unstick = s.tr_unstick;
    next->tr_lockout = s.tr_lockout;
    next->tr_spine = s.tr_spine;
    next->tr_plant = s.tr_plant;
    next->tr_setting = s.tr_setting;
    next->grind_own = s.grind_own;
    next->tr_own = s.tr_own;
    next->tr_option = s.tr_option;
    next->grind_assist = s.grind_assist;
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->underflip_speed = s.underflip_speed;
    next->underflip_split = s.underflip_split;
    next->flip_advanced = s.flip_advanced;
    next->flip_pop = s.flip_pop;
    next->flip_flick = s.flip_flick;
    next->flip_limit = s.flip_limit;
    next->flip_trick = s.flip_trick;
    next->flip_advanced_blocked = s.flip_advanced && extras_blocked();
    next->flip_live = s.flip_live;
    next->boneless_height = s.boneless_height;
    next->revert_boost = s.revert_boost;
    next->pump_power = s.pump_power;
    next->pump_live = s.pump_live;
    next->pump_blocked = s.pump_power != 1.0f && extras_blocked();
    next->revert = s.revert;
    next->return_delay = s.return_delay;
    next->pad_shortcuts = s.pad_shortcuts;
    next->hud = s.hud;
    next->hud_jump = s.hud_jump;
    next->logging = s.logging;
    next->map = s.map;
    next->map_note = s.map_file.note;
    next->map_preset = s.map_file.preset.empty() ? std::string{} : s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    next->spots = s.map_file.spots;
    next->waypoint_set = s.waypoint.has_value();
    if (s.waypoint) next->waypoint = *s.waypoint;
    next->open_serial = s.open_serial;
    for (const auto &one : setups())
        if (modified(one)) {
            ++next->changed[one.topic];
            ++next->settings[one.topic];
            ++next->setups_changed;
            next->setups.push_back({one.key, one.title, one.topic, one.kind, one.get(), one.stock, one.low, one.high});
        }
    // Nothing of the trainer's is in force. (A lock on a value that is the game's own changes nothing: counted apart.)
    next->stock = !next->touched && !next->setups_changed;
    next->folds = s.folds;
    next->share_text = s.share_text;
    next->share_serial = s.share_serial;
    next->open_tab = s.open_tab;
    if (const auto found = s.maps.find(s.map); found != s.maps.end()) {
        next->markers = found->second.markers;
        next->profile_preset = found->second.preset;
    }
    publish(std::move(next));
}
// ---- sharing presets ---------------------------------------------------------------------------
// One line of text, short enough to paste in a chat: "RST1:" and the preset's JSON in base64.
constexpr std::string_view share_tag = "RST1:";
constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk{};
        const auto count = std::min<std::size_t>(3, bytes.size() - i);
        for (std::size_t k = 0; k < 3; ++k) chunk = chunk << 8 | (k < count ? static_cast<unsigned char>(bytes[i + k]) : 0u);
        for (std::size_t k = 0; k < 4; ++k) out += k <= count ? base64_digits[chunk >> (18 - 6 * k) & 63] : '=';
    }
    return out;
}
std::optional<std::string> unbase64(std::string_view text) {
    std::string out;
    std::uint32_t chunk{};
    int bits{};
    for (const char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        const auto digit = base64_digits.find(c);
        if (digit == std::string_view::npos) return std::nullopt;
        chunk = chunk << 6 | static_cast<std::uint32_t>(digit);
        if ((bits += 6) >= 8) out += static_cast<char>(chunk >> (bits -= 8) & 0xff);
    }
    return out;
}
std::filesystem::path shared_directory() {
    const auto directory = data_directory();
    return directory.empty() ? directory : directory / L"shared";
}
// File names from preset names: letters, digits, space, '-' and '_' only.
std::string file_stem(std::string_view name) {
    std::string stem;
    for (const char c : name) stem += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' ? c : '_';
    return stem.empty() ? "preset" : stem;
}
// `preset export <name>`: one of the player's presets, or "current" for what is changed right now.
std::string export_preset(const std::string &name) {
    auto &s = state();
    Json values = Json::object();
    std::string title;
    if (lower(name) == "current") {
        for (const auto &e : s.entries)
            if (e.touched) values[e.key] = e.value;
        for (const auto &one : setups())
            if (off_stock(one)) values[one.key] = one.get();
        title = "My setup";
    } else {
        for (const auto &[user_name, saved] : s.user)
            if (lower(user_name) == lower(name)) {
                for (const auto &[key, value] : saved) values[key] = value;
                title = user_name;
            }
        if (title.empty()) return "error: no preset of yours is called \"" + name + "\". Use \"current\" for what is changed right now.";
    }
    if (values.empty()) return "error: nothing is changed, so there is nothing to share.";
    const auto count = values.size();
    Json json = Json::object();
    json["reskate_trainer"] = 1;
    json["name"] = title;
    json["values"] = std::move(values);
    s.share_text = std::string(share_tag) + base64(json.dump());
    ++s.share_serial;
    std::string where;
    if (const auto directory = shared_directory(); !directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / (file_stem(title) + ".json"), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (file) where = std::format(" Also saved as shared\\{}.json.", file_stem(title));
    }
    changed();
    return std::format("\"{}\" copied to the clipboard as one line ({} values): paste it anywhere.{}", title, count, where);
}
// `preset import [file]`: the text the menu put in shared\clipboard.txt, or a file in shared\.
// Either the one-line form or the JSON itself. The preset joins the player's own; nothing is applied.
std::string import_preset(const std::string &file_name) {
    auto &s = state();
    const auto directory = shared_directory();
    if (directory.empty()) return "error: the trainer's folder could not be found.";
    const auto path = directory / (file_name.empty() ? "clipboard.txt" : file_stem(std::filesystem::path(file_name).stem().string()) + ".json");
    std::string text;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return file_name.empty() ? "error: there is nothing to import." : "error: shared\\" + path.filename().string() + " does not exist.";
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (text.size() > 256 * 1024) return "error: that is too long to be a preset.";
    if (const auto tag = text.find(share_tag); tag != std::string::npos) {
        const auto start = tag + share_tag.size();
        const auto end = text.find_first_of(" \t\r\n`\"'", start);
        const auto decoded = unbase64(std::string_view(text).substr(start, end == std::string::npos ? end : end - start));
        if (!decoded) return "error: that is not a whole preset line: copy all of it, from RST1: to the end.";
        text = *decoded;
    }
    try {
        const auto json = Json::parse(text);
        if (!json.is_object() || !json.contains("values") || !json.at("values").is_object()) return "error: that is not a ReSkate Trainer preset.";
        std::map<std::string, double, std::less<>> values;
        std::size_t unknown{};
        for (const auto &[key, value] : json.at("values").items()) {
            if (!value.is_number() || values.size() >= 512) continue;
            const auto id = lower(key);
            const bool setting = find_setup(id) || id == "trick.flip_gate" || id == "trick.catch_at" || id == "trick.catch_percent";
            if (!setting && !find_entry(id)) ++unknown; // kept: another build of the game may have it
            values[id] = value.get<double>();
        }
        if (values.empty()) return "error: that preset holds no values.";
        auto name = json.value("name", "Imported");
        if (name.empty() || name.size() > 40) name = "Imported";
        const auto taken = [&](const std::string &candidate) {
            if (lower(candidate) == "stock" || lower(candidate) == "map" || s.user.contains(candidate)) return true;
            return std::ranges::any_of(builtin_presets(), [&](const auto &preset) { return lower(preset.name) == lower(candidate); });
        };
        auto unique = name;
        for (int n = 2; taken(unique); ++n) unique = std::format("{} {}", name, n);
        const auto count = values.size();
        s.user[unique] = std::move(values);
        changed();
        return std::format("Imported \"{}\" ({} values{}). Turn it on under Your presets.", unique, count,
                           unknown ? std::format(", {} of them unknown to this version", unknown) : "");
    } catch (...) {
        return "error: that is not a ReSkate Trainer preset.";
    }
}
bool flag(const std::string &text, bool &out) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true") { out = true; return true; }
    if (value == "0" || value == "off" || value == "false") { out = false; return true; }
    return false;
}
std::optional<int> slot_argument(const std::vector<std::string> &arguments, std::size_t index) {
    auto &s = state();
    if (arguments.size() <= index) return s.slot;
    const auto value = number(arguments[index]);
    if (!value || *value < 1 || *value > static_cast<double>(marker_slots) || *value != std::floor(*value)) return std::nullopt;
    return static_cast<int>(*value) - 1;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        const auto now = GetTickCount64();
        if (!s.loaded) load_store();
        s.base = base;
        sync_session();
        s.boosts = boosts_in_force();
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        take_map_file();
        if (const auto key = lower(level); !key.empty() && key != s.waypoint_level) {
            s.waypoint_level = key;
            s.waypoint.reset();
            s.view_due = true;
        }
        if (take_teleport_card_shown()) s.waypoint_wanted_until = now + 1000;
        if (now < s.waypoint_wanted_until && now >= s.waypoint_next_read) {
            s.waypoint_next_read = now + 250;
            refresh_waypoint();
        }
        if (playing) {
            if (match_blocks_markers()) s.return_at = 0;
            observe(base, client, now);
            shortcuts();
            update_landing(now);
            if (s.return_at && now >= s.return_at) {
                s.return_at = 0;
                const auto &marker = current_map().markers[static_cast<std::size_t>(s.slot)];
                if (marker.set) (void)go_to(marker.position, "your marker");
            }
        } else if (s.telemetry.skater) {
            s.telemetry.skater = false;
            s.motion.valid = false;
            s.entity = 0;
        }
        (void)start_trick_heights(base);
        (void)start_state_holds(base);
        if (take_class_list_shown()) s.class_list_wanted = true;
        // The search reads all of the game's writable memory, so it runs only for a player with
        // a use for it: one of the classes' values or the flip speed is not the game's own
        // (theirs, or a host's they skate with), or they have the list of every value open.
        if (s.class_search_at && now >= s.class_search_at && playing && s.telemetry.skater && !finding_classes() &&
            (s.class_list_wanted || classes_wanted())) {
            // A level brings fresh copies, so only a search finished since it loaded counts. The
            // skater's own classes arrive a little after the skater does: a search that came up
            // empty is repeated, twice.
            const bool have = class_searches() > s.class_search_seen && class_copies(0) != 0;
            if (have || s.class_search_tries >= 3) {
                s.class_search_at = 0;
            } else {
                ++s.class_search_tries;
                s.class_search_at = now + 15000;
                find_classes();
            }
        }
        flip_speed_in_use(s.boosts.flip != 1.0f || s.flip_advanced || s.flip_trick[late_flip_slot] != 1.0f || s.flip_flick || !s.flip_limit || underflip_in_force() != 1.0f ||
                          underflip_split_in_force());
        // The speed itself goes straight to the trick in the air (drive_flip_speed); the flip curves
        // are only scaled where that number cannot be reached.
        want_flip_speed(s.flip_live ? 1.0f : s.boosts.flip);
        // The player's own choice, and only where their own trick settings count: not under a
        // host's enforced physics or in a session that has turned boosts off.
        // The game's flip landing limit hurries a flip that is still turning round before the landing. It stays the
        // game's own unless the player unticks it (Flip landing limit): it was once dropped whenever a flip was slowed
        // at all, and a flip speed of 0.99 then left the slow-turning shuvits half way round (reported 2026-10-24).
        want_flip_gate(extras_blocked() || s.flip_limit ? flip_gate_stock : flip_gate_off);
        if (s.boosts.flip != s.flip_applied) {
            // The catch times follow the flip speed (want_class).
            s.flip_applied = s.boosts.flip;
            for (const auto &e : s.entries)
                if (e.class_field >= 0 && slowed_catch_time(e.key)) want_class(e);
        }
        if (const float late = late_flip_factor(); late != s.late_flip_applied) {
            s.late_flip_applied = late;
            for (const auto &e : s.entries)
                if (e.class_field >= 0 && late_flip_speed(e.key)) want_class(e);
        }
        (void)apply_classes();
        // The game builds its tuning objects again with a new skater (a respawn, a teleport, a
        // session change) and they start at its own numbers: look again, but not in a flurry.
        {
            const bool lost = refresh_classes(now);
            const auto entity = static_cast<std::uint64_t>(s.entity);
            bool new_skater = entity && s.class_entity && entity != s.class_entity;
            if (entity) s.class_entity = entity;
            // A respawn can keep the entity and still fill new copies (the push speeds' native
            // one among them): the old ones go on holding what was written, so nothing looks lost.
            if (skater_creations() != s.class_creations) s.grind_stock.fill(-1); // a new skater starts from the game's own switches
            if (const auto creations = skater_creations(); creations != s.class_creations) {
                new_skater = new_skater || s.class_creations != 0;
                s.class_creations = creations;
            }
            // A bail builds a new skater too, and most of those bring nothing new: a search that a
            // new skater asked for and that found only what was known doubles the wait before the
            // next one (a search reads all of the game's writable memory, for seconds).
            if (!s.class_skater_judged && !s.class_search_at && !finding_classes()) {
                s.class_skater_judged = true;
                s.class_skater_wait = class_search_found() == s.class_found ? std::min(s.class_skater_wait * 2, class_skater_wait_high) : class_skater_wait_low;
                s.class_skater_after = now + s.class_skater_wait;
            }
            s.class_research_due = s.class_research_due || lost;
            s.class_skater_due = s.class_skater_due || new_skater;
            if (!classes_wanted()) s.class_research_due = s.class_skater_due = false;
            const bool for_lost = s.class_research_due && now >= s.class_research_after;
            const bool for_skater = s.class_skater_due && now >= s.class_skater_after && now >= s.class_research_after;
            if ((for_lost || for_skater) && !s.class_search_at && !finding_classes()) {
                s.class_research_due = s.class_skater_due = false;
                s.class_search_seen = class_searches();
                s.class_search_tries = 0;
                s.class_search_at = now + 3000;
                s.class_research_after = now + 20000;
                if (!for_lost) {
                    s.class_skater_judged = false;
                    s.class_found = class_search_found();
                    s.class_skater_after = now + s.class_skater_wait;
                }
            }
        }
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The pop out of a manual: the ollie's launch, scaled while Enum.Intent.Manual says the skater is in one.
        // "Slow flips pop higher": the launch of each flip trick, by the speed it will turn at (the game has the trick in
        // mind when it launches: measured, a kickflip reads 2 and an ollie 1 at that call).
        for (std::uint32_t trick = 0; trick < 64; ++trick) {
            float up = 0;
            if (s.flip_pop && !extras_blocked() && (!plain_pop(trick) || ollie_pop(trick))) {
                up = slow_flip_up(trick, s.boosts.flip * own_trick_speed(flip_trick_index(trick)));
            }
            set_flip_launch(trick, up);
        }
        set_manual_pop(s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 1.0f : s.manual_height,
                       read_manual_intent(s.base, static_cast<std::uintptr_t>(s.entity)));
        {
            // The sideways part of a pop out of a grind, against the way the skater was travelling on it.
            const auto watch = watched_physics_state();
            const bool on_grind = watch.valid && watch.state >= 400 && watch.state < 500;
            // The controller is only asked while it matters: on a grind, with the stick push in use or being logged.
            const float stick = on_grind && (s.grind_push != 0.0f || s.hud_jump || s.logging) ? left_stick_x() : 0.0f;
            set_grind_side(extras_blocked() ? 1.0f : s.grind_side, s.motion.velocity[0], s.motion.velocity[2], watch.valid && watch.state >= 400 && watch.state < 500,
                           extras_blocked() ? 0.0f : s.grind_push, stick);
            float along{}, side{}, nudge{};
            if (take_grind_pop(along, side, nudge) && (s.hud_jump || s.logging || s.grind_side != 1.0f || s.grind_push != 0.0f))
                say(logging::Level::info, std::format("Trainer grind pop: the game's launch {:.2f} m/s along the grind, {:.2f} m/s sideways (side force x {:.2f}); "
                                                      "stick {:.2f}, stick push {:.2f} m/s.", along, side, extras_blocked() ? 1.0f : s.grind_side, nudge,
                                                      extras_blocked() ? 0.0f : nudge * s.grind_push));
        }
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        if(const auto push=skater_push::peek())
            (void)queue_skater_push_velocity(client,s.entity,*push);
        // Past the game's own ceiling the push is carried by the trainer (see trainer_push_speed):
        // the player's own setting only, like the other speed the trainer adds.
        const float push_factor = top_speed && top_speed->touched && top_speed->stock > 0 && !extras_blocked()
                                      ? std::max(1.0f, static_cast<float>(top_speed->value / top_speed->stock)) : 1.0f;
        set_push_speed(client, s.entity, push_factor, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
        // Pumping: the game says when (its pumping state), the trainer sizes the gain on the physics
        // step (trainer_pump_power). The player's own setting only, like the push speed.
        {
            const auto pump = read_pump_state(base, static_cast<std::uintptr_t>(s.entity));
            if (pump.found != s.pump_live) {
                s.pump_live = pump.found;
                s.view_due = true;
            }
            bool measure = false, searching = false;
            const bool grind_held = std::ranges::any_of(s.grind_mode, [](std::int8_t mode) { return mode >= 0; });
            keep_holds_alive();
            for (std::size_t i = 0; i < grind_settings.size(); ++i) force_grind_setting(i, extras_blocked() ? -1 : s.grind_setting[i]);
            states_in_use(s.pump_power != 1.0f || s.manual_height != 1.0f || grind_held || s.grind_watch || s.grind_push != 0.0f || s.grind_range != 0.0f || measure || searching);
            {
                const auto watch = watched_physics_state();
                const bool grinding = watch.valid && watch.state >= 400 && watch.state < 500;
                // Skate 3's turn (retail sub_82D89F58): nothing inside a quarter of the stick, the rest stretched, x 0.85,
                // cubed, eased 0.91 / 0.09 a sixtieth of a second; the end of the stick is `grind_range` degrees.
                const bool turning = grinding && s.grind_range != 0.0f && !extras_blocked();
                const auto turn_now = GetTickCount64();
                if (turning) {
                    float x{}, y{};
                    (void)left_stick_raw(x, y);
                    const float past = std::abs(x) <= 0.25f ? 0.0f : (std::abs(x) - 0.25f) * (4.0f / 3.0f) * (x < 0 ? -1.0f : 1.0f);
                    const float wanted = std::pow(std::clamp(past * 0.85f, -0.85f, 0.85f), 3.0f);
                    const float steps = s.grind_yaw_at ? std::clamp(static_cast<float>(turn_now - s.grind_yaw_at) * 0.06f, 0.0f, 6.0f) : 1.0f;
                    s.grind_yaw += (wanted - s.grind_yaw) * (1.0f - std::pow(0.91f, steps));
                    if (std::abs(s.grind_yaw) < 0.0001f && wanted == 0.0f) s.grind_yaw = 0.0f;
                    s.grind_heading = s.grind_yaw / 0.614125f * s.grind_range * std::numbers::pi_v<float> / 180.0f;
                    // The game's scripts cannot be answered (they do not go through the lookups), so its own grind control
                    // is steered: it turns the board to about atan2(LR, |UD|) of the right stick it sees, after turning
                    // that stick by an angle of its own. The stick it is shown: the turn, turned back by that angle; the
                    // player's own up and down (the tip), shortened so the turn and the tip together stay on the rim.
                    // (The sideways sign the game flips while the stick is down is undone where the stick is made up.)
                    // Nothing is ever added to the player's up and down: a made-up move there reads as a pop or a flip.
                    // It is only shortened so the turn can pass 45 degrees: freely while the stick is up (nose grind),
                    // to no less than 0.6 while it is down (5-0), where letting go of the stick is the pop.
                    float sent[3]{};
                    DingoSDKOverlayGrindStickSent(sent);
                    const float own_y = sent[2];
                    float stick_x = std::clamp(s.grind_heading / 1.66f, -1.0f, 1.0f), y_scale = 1.0f; // flat: about 95 degrees at the end
                    if (std::abs(own_y) > 0.3f) {
                        const float heading = std::clamp(s.grind_heading, -1.4f, 1.4f);
                        y_scale = std::max(own_y > 0 ? 0.17f : 0.6f, std::cos(heading));
                        stick_x = std::clamp(std::tan(heading) * y_scale, -1.0f, 1.0f);
                    }
                    DingoSDKOverlaySetGrindStickTurn(true, stick_x, 0.0f, y_scale);
                    s.grind_frame_cos = stick_x; // for the log: the stick's sideways part as asked for
                    s.grind_frame_sin = y_scale;
                } else {
                    s.grind_yaw = s.grind_heading = 0.0f;
                    DingoSDKOverlaySetGrindStickTurn(false, 0.0f, 0.0f, 1.0f);
                }
                s.grind_yaw_at = turning ? turn_now : 0;
            }
            {
                auto modes = s.grind_mode, live = s.grind_live;
                if (extras_blocked()) modes.fill(-1);
                std::array<bool, grind_switches.size()> giving_back{};
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (modes[i] < 0 && s.grind_stock[i] >= 0) {
                        modes[i] = s.grind_stock[i]; // released: the game's own value back, once
                        giving_back[i] = true;
                    }
                hold_grind_switches(base, static_cast<std::uintptr_t>(s.entity), modes.data(), live.data());
                // Given back once the skater's value was there to write (no page yet: tried again next tick).
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (giving_back[i] && live[i] >= 0) s.grind_stock[i] = -1;
                for (std::size_t i = 0; i < modes.size(); ++i)
                    if (s.grind_mode[i] >= 0 && !extras_blocked() && s.grind_stock[i] < 0 && live[i] >= 0) s.grind_stock[i] = live[i]; // read before the first write
                if (live != s.grind_live) {
                    s.grind_live = live;
                    s.view_due = true;
                }
            }
            set_pump_power(client, s.entity, extras_blocked() ? 1.0f : s.pump_power, pump.found && pump.pumping, measure);
        }
        {
            // Gravity: the game's own under a host's physics or with boosts off. Both levers let go by themselves.
            const bool stock_gravity = s.enforced || (multiplayer_session_active() && !session_boosts_allowed());
            float board = stock_gravity ? 1.0f : s.gravity, foot = board;
            // Under a host's physics or with boosts off the advanced levers let go with the gravity.
            const bool own = board != 1.0f || (s.gravity_landing_own && s.gravity_landing != 1.0f);
            set_air_gravity(client, s.entity, board, !own || stock_gravity ? 1.0f : s.gravity_landing_own ? s.gravity_landing : 0.0f,
                            board == 1.0f ? 0.0f : s.gravity_timing);
            air_gravity_now = board;
            if (const bool live = set_foot_gravity(foot); live != s.gravity_live) {
                s.gravity_live = live;
                s.view_due = true;
            }
            // No limit on foot: the game's own 10 m/s cut rises with the sprint speeds asked for, and is its own otherwise.
            const float fastest = std::max(class_value_wanted("locomotion", "SprintSpeed"), class_value_wanted("locomotion", "SprintSpeedBoost"));
            (void)set_foot_speed_cap(s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 10.0f : fastest * 1.05f);
        }
        drive_transition(base, client, now);
        // The revert boost adds speed, so it keeps to the session's rule for boosts and to a host's physics.
        set_revert_boost(client, s.entity, s.enforced || (multiplayer_session_active() && !session_boosts_allowed()) ? 0.0f : s.revert_boost, s.revert);
        if (const auto report = revert_boost_report(); report.sequence != s.revert_boosts_seen) {
            s.revert_boosts_seen = report.sequence;
            // Every landing with some spin in it, boosted or not, so a report can say why not.
            if ((s.hud_jump || s.logging) && report.spin >= 30.0f)
                say(logging::Level::info,
                    std::format("Trainer revert: landed after {} ms with {:.0f} deg of spin, board {} off the body and {:.0f} deg off the travel, board turned {:.0f} deg past the body, state {}, "
                                "{:.1f} m/s: {}{}.",
                                report.air_ms, report.spin, report.board_read ? std::format("{:.0f} deg", report.board_offset) : std::string("unread"), report.slip,
                                report.board_rotation, report.landed_state, report.speed, report.outcome,
                                report.added > 0 ? std::format(" +{:.1f} m/s", report.added) : std::string()));
        }
        for (TrickLaunch launch; take_trick_launch(launch);)
            if (launch.factor != 1.0f)
                say(logging::Level::info, std::format("Trainer trick: {} launched at {:.2f} m/s up, x{:.2f}.",
                                                      launch.trick == Trick::boneless ? "boneless" : "no comply", launch.up_speed, launch.factor));
        hold(now);
        if (s.extras_due) publish_extras();
        run_selftest(now);
        publish(s.telemetry);
        if (s.view_due) build_view();
        if (s.save_due && now >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
// ---- tricklining -------------------------------------------------------------------------------
// trainer revert <field> <number> | reset: what counts as a revert and what it is worth.
std::string revert_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto name = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    if (name == "reset") {
        s.revert = {};
        changed();
        return "Bends are judged the way they shipped: 90 degrees of spin and the board 40 degrees off the body (the off-the-travel rule is off).";
    }
    const auto found = std::ranges::find(revert_fields, name, &RevertField::name);
    const auto value = a.size() > 1 ? number(a[1]) : std::nullopt;
    if (found == std::end(revert_fields) || !value) return "error: usage: trainer revert spin|slip|twist|bend|full|auto|max_speed|cooldown|min_air <number>, or revert reset";
    s.revert.*found->member = static_cast<float>(*value);
    s.revert = sane_revert(s.revert);
    changed();
    return std::format("Revert {} = {:.4g}", name, s.revert.*found->member);
}
// The values that make a trick line: one command sets them the way Skate 3 had them, or puts the
// game's own back. Skate 3's are its tuning for what this game still shares with it (the "Skate 3"
// preset) and speed back out of reverts; its heavier revert and powerslide friction is a first
// guess (x3 and x1.5), to be tuned by playing.
constexpr std::pair<std::string_view, double> trickline_friction[]{
    {"onboard_powerslide.frictionscalar_revert", 3.0}, {"onboard_powerslide.frictionscalar_autorevert", 3.0}, {"onboard_powerslide.frictionscalar_slide", 1.5}};
// trainer feel stock|easy|normal|hardcore: which game the skating plays like. The three Skate 3
// choices are its difficulty settings (the built-in presets of those names); only one is on.
constexpr std::pair<std::string_view, std::string_view> feels[]{{"easy", "Skate 3 Easy"}, {"normal", "Skate 3"}, {"hardcore", "Skate 3 Hardcore"}};
std::string feel_command(const std::vector<std::string> &a) {
    const auto which = a.empty() ? std::string{} : lower(a[0]);
    std::string why;
    if (!editable(&why)) return "error: " + why;
    const auto found = std::ranges::find(feels, which, [](const auto &feel) { return feel.first; });
    if (found == std::end(feels) && which != "stock" && which != "skate") return "error: usage: trainer feel stock|easy|normal|hardcore";
    // Only a feel that is on is taken off, and of that only the values that still hold its numbers: taking off more
    // would put back values the player set themselves that it happens to share (the push speed dial, for one).
    for (const auto &feel : feels)
        if (std::ranges::find(state().active, feel.second) != state().active.end() || builtin_on(feel.second)) (void)remove_preset(feel.second, true);
    if (found != std::end(feels)) (void)apply_preset(found->second);
    changed();
    return found == std::end(feels) ? "Plays like skate.: the game's own tuning." : std::format("Plays like {}: its tuning for everything this game shares with it.", found->second);
}
// trainer trickline extras on|off: what Skate 3's trick lines had beyond its tuning. Speed back
// out of reverts, heavier revert and powerslide friction (x3 and x1.5: a first guess), and higher
// footplants (no comply and boneless x1.5, OVM's ask: a first guess too).
constexpr float trickline_footplant = 1.5f;
std::string trickline_command(const std::vector<std::string> &a) {
    auto &s = state();
    std::string why;
    if (!editable(&why)) return "error: " + why;
    bool on{};
    if (a.size() < 2 || lower(a[0]) != "extras" || !flag(a[1], on)) return "error: usage: trainer trickline extras on|off";
    // Off puts back only what still holds the extras' own numbers: a value the player has since moved is theirs.
    for (const auto &[key, factor] : trickline_friction)
        if (auto *e = find_entry(key); e && !is_locked(*e) && (on || std::abs(e->value - sane(*e, e->stock * factor)) < 1e-6 * std::max(1.0, std::abs(e->value))))
            set_entry(*e, on ? e->stock * factor : e->stock);
    s.revert_boost = on ? std::max(s.revert_boost, 1.0f) : s.revert_boost == 1.0f ? 0.0f : s.revert_boost;
    s.nocomply_height = on ? std::max(s.nocomply_height, trickline_footplant) : s.nocomply_height == trickline_footplant ? 1.0f : s.nocomply_height;
    s.boneless_height = on ? std::max(s.boneless_height, trickline_footplant) : s.boneless_height == trickline_footplant ? 1.0f : s.boneless_height;
    changed();
    return on ? "Tricklining extras on: the board bending boost, heavier revert and powerslide friction, no comply and boneless height x 1.5."
              : "Tricklining extras off.";
}
// trainer transition | transition feel skate|skate3 | transition lean|carry|drop|unstick|lockout|plant <n> |
// transition spine on|off | transition <setting>|all game|off|on
std::string transition_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    const auto what = lower(arg(0)), to = lower(arg(1));
    std::string why;
    if (what.empty()) {
        const auto now = GetTickCount64();
        std::string out = std::format("Lean into the ramp {:.2f}, speed kept along the coping {:.2f} (at most {:.2f} m/s across it), hang-up push {:.2f}, no catching the coping again for "
                                      "{:.2f} s, flights clear the coping {}, handplant exit {:.2f}.",
                                      s.tr_lean, s.tr_carry, s.tr_drop, s.tr_unstick, s.tr_lockout, s.tr_spine ? "on" : "off", s.tr_plant);
        out += std::format("\nsteep ground {} ms ago, way out of the wall {:.2f} {:.2f}; on a coping: {}; coping {:.2f} {:.2f}, ramp {:.2f} {:.2f}; air grind reach held off: {}; "
                           "lean polls given {}",
                           s.tr.steep_at ? now - s.tr.steep_at : 0, s.tr.steep_out[0], s.tr.steep_out[1], s.tr.coping ? "yes" : "no", s.tr.edge[0], s.tr.edge[1], s.tr.ramp[0],
                           s.tr.ramp[1], s.tr.air_grinds_off ? "yes" : "no", DingoSDKOverlayGrindStickLeaned());
        for (std::size_t i = 0; i < transition_settings.size(); ++i)
            out += std::format("\n{} (setting {}, the game's default {}): {}; asked {} times since last listed", transition_settings[i].key, transition_settings[i].setting,
                               transition_settings[i].stock ? "on" : "off", s.tr_setting[i] < 0 ? "game's own" : s.tr_setting[i] ? "forced on" : "forced off",
                               transition_setting_asked(i));
        for (std::size_t i = 0; i < transition_options.size(); ++i)
            out += std::format("\n{} (the game's own option {}, default {}): {}", transition_options[i].key, transition_options[i].setting, transition_options[i].stock, s.tr_option[i]);
        return out;
    }
    if (what == "feel") {
        const bool skate3 = to == "skate3" || to == "3";
        if (!skate3 && to != "skate" && to != "stock") return "error: usage: trainer transition feel skate|skate3";
        if (!editable(&why)) return "error: " + why;
        if (std::ranges::find(s.active, transition_preset) != s.active.end() || builtin_on(transition_preset)) (void)remove_preset(transition_preset, true);
        // Values the preset set in earlier builds of this day and no longer does: back to the game's own where they
        // still hold what it set.
        static constexpr std::pair<std::string_view, double> dropped[]{
            {"physicsreckoning.vertjumpalignmaxgroundnormaly", 0.2}, {"physicsairstates.naturalairmaxdist", 0.12}, {"physicstrajectory.scoremiddlebonus", 40.0},
            {"physicstrajectory.mintimefortrajectorytobevalid", 0.25}, {"physicsmode.pumpeffectfactor", 18.0}, {"physicsmode.pumpmaxacceleration", 10.0},
            {"physicsmode.unintentionalpumpscalar", 0.7}, {"physicspumping.pumpangle", 50.0}, {"physicspumping.pumpminfactor", 0.01}};
        for (const auto &[key, value] : dropped)
            if (auto *e = find_entry(key); e && e->touched && !is_locked(*e) && std::abs(e->value - value) < 1e-4) set_entry(*e, e->stock);
        reset_transition();
        if (skate3) {
            s.tr_lean = skate3_tr_lean;
            s.tr_carry = skate3_tr_carry;
            s.tr_drop = skate3_tr_drop;
            s.tr_unstick = skate3_tr_unstick;
            s.tr_lockout = skate3_tr_lockout;
            // "Airs clear the coping" is not part of it: the one session it was on in, every popped air landed on the deck.
            s.tr_plant = skate3_tr_plant;
            for (std::size_t i = 0; i < transition_settings.size(); ++i) s.tr_setting[i] = transition_settings[i].skate3;
            (void)apply_preset(transition_preset);
        }
        changed();
        return std::string{skate3 ? "Transition like Skate 3: lean into the ramp, roll off the coping at the angle you came in, no hang-ups, Skate 3's handplant exit, its "
                                    "coping exit timers. Airs and pumping stay skate.'s own."
                                  : "Transition like skate.: everything is the game's own."};
    }
    struct Number {
        std::string_view key;
        float *value;
        float low, high;
        std::string_view says;
    };
    const Number numbers[]{
        {"lean", &s.tr_lean, -1000.0f, 1000.0f, "Lean into the ramp on a coping grind"},
        {"carry", &s.tr_carry, 0.0f, 1000.0f, "Speed kept along the coping rolling off it"},
        {"drop", &s.tr_drop, 0.0f, 1.0e6f, "Most speed across the coping while it is kept (m/s)"},
        {"unstick", &s.tr_unstick, 0.0f, 1.0e6f, "Push out of a hang-up"},
        {"lockout", &s.tr_lockout, 0.0f, 3600.0f, "Seconds the coping cannot be caught again"},
        {"plant", &s.tr_plant, 0.0f, 1000.0f, "Handplant exit like Skate 3's"},
    };
    for (const auto &one : numbers) {
        if (what != one.key) continue;
        const auto value = number(arg(1));
        if (!value) return std::format("error: usage: trainer transition {} <number> ({} to {}; 0 = off)", one.key, one.low, one.high);
        if (*value != 0 && !editable(&why)) return "error: " + why;
        *one.value = std::clamp(static_cast<float>(*value), one.low, one.high);
        changed();
        return std::format("{}: {:.2f}.", one.says, *one.value);
    }
    for (std::size_t i = 0; i < transition_options.size(); ++i) {
        // The game's own options: shown, never written. They are the player's, changed in the game's own Settings menu.
        const auto &one = transition_options[i];
        if (what != one.key) continue;
        const auto shown = one.boolean ? std::string(s.tr_option[i] ? "on" : "off") : std::to_string(s.tr_option[i]);
        if (!arg(1).empty())
            return std::format("error: {} is your game's own setting ({}): the trainer does not change it. Change it in the game's Settings menu (now {}).",
                               one.label, one.setting, shown);
        return std::format("{} (the game's own option {}): {}. Change it in the game's Settings menu.", one.label, one.setting, shown);
    }
    if (what == "spine") {
        bool on{};
        if (!flag(arg(1), on)) return "error: usage: trainer transition spine on|off";
        if (on && !editable(&why)) return "error: " + why;
        s.tr_spine = on;
        changed();
        return std::string{on ? "Flights up and over a lip no longer reach for the coping." : "Flights reach for the coping as the game has it."};
    }
    const int mode = to == "on" || to == "1" ? 1 : to == "off" || to == "0" ? 0 : to == "game" || to == "-1" ? -1 : -2;
    if (mode == -2) return "error: usage: trainer transition (lists everything) | feel skate|skate3 | lean|carry|drop|unstick|lockout|plant <n> | spine on|off | <setting>|all game|off|on";
    if (mode >= 0 && !editable(&why)) return "error: " + why;
    bool any = false;
    for (std::size_t i = 0; i < transition_settings.size(); ++i)
        if (what == "all" || what == transition_settings[i].key) {
            s.tr_setting[i] = static_cast<std::int8_t>(mode);
            any = true;
        }
    if (!any) return "error: no transition setting is called \"" + what + "\". `trainer transition` lists them.";
    changed();
    return what + ": " + (mode < 0 ? "the game's own" : mode ? "forced on" : "forced off") + ".";
}
// trainer foot (lists the switches and how often the game asked for each) | foot <setting>|all game|off|on
std::string foot_command(const std::vector<std::string> &a) {
    auto &s = state();
    const auto what = a.empty() ? std::string{} : lower(a[0]), to = a.size() > 1 ? lower(a[1]) : std::string{};
    if (what.empty()) {
        std::string out = "The game's own hidden on-foot switches:";
        for (std::size_t i = 0; i < foot_settings.size(); ++i)
            out += std::format("\n{} (setting {}, the game's default {}): {}; asked {} times since last listed", foot_settings[i].key, foot_settings[i].setting,
                               foot_settings[i].stock ? "on" : "off", s.foot_setting[i] < 0 ? "game's own" : s.foot_setting[i] ? "forced on" : "forced off",
                               foot_setting_asked(i));
        return out;
    }
    const int mode = to == "on" || to == "1" ? 1 : to == "off" || to == "0" ? 0 : to == "game" || to == "-1" ? -1 : -2;
    if (mode == -2) return "error: usage: trainer foot (lists them) | foot <setting>|all game|off|on";
    std::string why;
    if (mode >= 0 && !editable(&why)) return "error: " + why;
    bool any = false;
    for (std::size_t i = 0; i < foot_settings.size(); ++i)
        if (what == "all" || what == foot_settings[i].key) {
            s.foot_setting[i] = static_cast<std::int8_t>(mode);
            any = true;
        }
    if (!any) return "error: no on-foot setting is called \"" + what + "\". `trainer foot` lists them.";
    changed();
    return what + ": " + (mode < 0 ? "the game's own" : mode ? "forced on" : "forced off") + ".";
}
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        std::string own;
        for (const auto &one : setups())
            if (off_stock(one)) own += std::format(" {}={:.4g}{}", one.key, one.get(), modified(one) ? "" : "(not in force)");
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}. The trainer's own settings off the game's:{}.", s.status, s.entries.size(), touched,
                           frozen, s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied", own.empty() ? " none" : own);
    }
    if (v == "set" || v == "freeze" || v == "reset") {
        if (v == "reset" && lower(arg(0)) == "all") {
            const auto count = reset_all();
            std::size_t locked{};
            for (const auto &e : s.entries) locked += e.touched;
            changed();
            return locked ? std::format("{} values put back; {} locked values kept (Reset everything clears those too).", count, locked)
                          : std::format("{} values put back.", count);
        }
        if (v == "reset" && (lower(arg(0)) == "tricks" || lower(arg(0)).starts_with("section"))) {
            // reset tricks: the sliders and switches of POP AND TRICKS. reset section <name>: everything one section of
            // the Tune tab shows, its tuning values too (locked ones stay).
            // (The console hands `reset` its words as one: "section grinds".)
            const auto words = lower(arg(0)) + (a.size() > 1 ? " " + lower(arg(1)) : std::string{});
            const bool section = words.starts_with("section");
            const auto named = section && words.size() > 8 ? words.substr(8) : std::string{};
            const auto topic = section ? topic_named(named) : topic_pop;
            if (!topic) return "error: usage: trainer reset section pop|speed|bails|grinds|tricklining|transition|pumping|foot";
            std::size_t count = reset_setups(topic);
            if (section)
                for (auto &e : s.entries) {
                    if (!e.touched || is_locked(e)) continue;
                    std::uint8_t of{};
                    (void)essential_name(e.key, nullptr, &of);
                    if (of != topic) continue;
                    set_entry(e, e.stock);
                    ++count;
                }
            changed();
            return std::format("{}: {} put back to the game's own.", section ? std::string(topics[topic].title) : std::string("Tricks"), count);
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, nothing of the trainer's stays set.
            // (Under a host's physics the player's own setup is kept for when they leave.)
            if (s.enforced) return "error: the host controls physics in this multiplayer game; your own setup is kept for when you leave.";
            std::size_t count{};
            for (auto &e : s.entries) e.frozen = false;
            for (auto &e : s.entries) {
                if (e.touched) {
                    set_entry(e, e.stock);
                    ++count;
                } else {
                    remember(e);
                }
            }
            s.saved.clear();
            s.active.clear();
            count += reset_setups();
            // A preset this map applies by itself would be back at its next load.
            if (!s.map.empty()) current_map().preset.clear();
            changed();
            return std::format("Everything is the game's own again: {} values and settings put back, locks cleared, presets off.", count);
        }
        auto *e = find_entry(arg(0));
        std::size_t at = 1; // where the number is: a graph's id ends in " x", which arrives as a word of its own
        if (!e && a.size() > 1) {
            e = find_entry(arg(0) + " " + arg(1));
            if (e) at = 2;
        }
        if (!e) return "error: no tuning value is called \"" + arg(0) + "\". Try: trainer find <text>";
        std::string why;
        if (v != "freeze" && !editable(&why)) return "error: " + why;
        if (v == "set") {
            const auto value = number(arg(at));
            if (!value) return "error: " + e->id + " needs a number.";
            set_entry(*e, *value);
        } else if (v == "freeze") {
            if (!flag(arg(at), e->frozen)) return "error: freeze needs 0 or 1.";
            remember(*e);
        } else {
            set_entry(*e, e->stock);
        }
        changed();
        return std::format("{} = {:.6g}{}{}", e->id, e->value, e->touched ? std::format(" (stock {:.6g})", e->stock) : "", e->frozen ? ", frozen" : "");
    }
    if (v == "find") {
        const auto pattern = lower(arg(0));
        std::string result;
        std::size_t count{};
        for (const auto &e : s.entries)
            if (matches(e.key, pattern) && ++count <= 12) result += std::format("{}{} = {:.6g}", result.empty() ? "" : "; ", e.id, e.value);
        return count ? std::format("{} match: {}{}", count, result, count > 12 ? "; ..." : "") : "Nothing matches.";
    }
    if (v == "dial") {
        // trainer dial <multiplier> <built-in preset>
        const auto factor = number(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (!factor || name.empty()) return "error: usage: trainer dial <multiplier> <preset name>";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        return apply_dial(name, *factor);
    }
    if (v == "preset") {
        const auto action = lower(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action == "import") return import_preset(name);
        if (name.empty()) return "error: usage: trainer preset apply|remove|save|delete|export <name>, or preset import [file]";
        if (action == "export") return export_preset(name);
        if (action == "remove") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return remove_preset(name);
        }
        if (action == "apply") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return apply_preset(name);
        }
        if (action == "save") {
            if (name.size() > max_preset_name) return "error: that name is too long.";
            if (reserved_preset_name(name)) return "error: that name belongs to a built-in preset (or Stock, or Map).";
            std::map<std::string, double, std::less<>> values;
            for (const auto &e : s.entries)
                if (e.touched) values[e.key] = e.value;
            for (const auto &one : setups())
                if (off_stock(one)) values[one.key] = one.get();
            if (values.empty()) return "error: nothing is changed, so there is nothing to save.";
            const auto count = values.size();
            s.user[name] = std::move(values);
            changed();
            return std::format("Saved \"{}\" ({} values).", name, count);
        }
        if (action == "delete") {
            if (!s.user.erase(name)) return "error: no preset of yours is called \"" + name + "\".";
            std::erase(s.active, name);
            changed();
            return "Deleted \"" + name + "\".";
        }
        return "error: usage: trainer preset apply|save|delete <name>";
    }
    if (v == "revert") return revert_command(a);
    if (v == "trickline") return trickline_command(a);
    if (v == "feel") return feel_command(a);
    if (v == "slot") {
        const auto slot = slot_argument(a, 0);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        s.slot = *slot;
        changed();
        return std::format("Marker slot {}{}.", s.slot + 1, current_map().markers[static_cast<std::size_t>(s.slot)].set ? "" : " (empty)");
    }
    if (v == "marker") {
        const auto action = lower(arg(0));
        if ((action == "save" || action == "go") && match_blocks_markers())
            return "error: markers are disabled during this match.";
        const auto slot = slot_argument(a, 1);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        if (s.map.empty()) return "error: load a level first.";
        auto &marker = current_map().markers[static_cast<std::size_t>(*slot)];
        if (action == "save") {
            if (!s.telemetry.skater) return "error: no skater to mark.";
            marker = {true, s.telemetry.position};
            changed();
            return std::format("Marker {} saved at ({:.1f}, {:.1f}, {:.1f}).", *slot + 1, marker.position[0], marker.position[1], marker.position[2]);
        }
        if (action == "go") {
            if (!marker.set) return std::format("error: marker {} is empty.", *slot + 1);
            return go_to(marker.position, std::format("marker {}", *slot + 1));
        }
        if (action == "clear") {
            marker = {};
            changed();
            return std::format("Marker {} cleared.", *slot + 1);
        }
        return "error: usage: trainer marker save|go|clear [slot]";
    }
    if (v == "ground") {
        const auto x = number(arg(0)), z = number(arg(1));
        if (!x || !z) return "error: usage: trainer ground <x> <z>  (teleport to the topmost surface there)";
        return land_at(static_cast<float>(*x), static_cast<float>(*z), s.telemetry.position[1], "the ground there");
    }
    if (v == "waypoint") {
        refresh_waypoint();
        if (lower(arg(0)) == "info")
            return "Map waypoint: " + (s.waypoint ? std::format("{:.1f}, {:.1f}, {:.1f}", (*s.waypoint)[0], (*s.waypoint)[1], (*s.waypoint)[2]) : std::string("none")) +
                   " (" + s.waypoint_detail + ").";
        if (!s.waypoint) return "error: no waypoint on the map. Open the pause map and place one first.";
        return land_at((*s.waypoint)[0], (*s.waypoint)[2], (*s.waypoint)[1], "your map waypoint");
    }
    if (v == "transition") return transition_command(a);
    if (v == "foot") return foot_command(a);
    if (v == "grind") {
        // trainer grind | grind watch 0|1 | grind side <x> | grind assist <0-100> | grind <key>|all game|off|on
        const auto what = lower(arg(0)), to = lower(arg(1));
        if (what == "assist") {
            // The player's own game setting, as its Settings menu sets it: in any session, never part of presets or resets.
            const auto value = number(to);
            if (!value || !(*value >= 0 && *value <= 100)) return "error: usage: trainer grind assist <0 to 100> (the game's own Grind Assist)";
            if (!profile_runtime::set_local_game_option(grind_assist_setting, std::round(*value), false)) return "error: the game's settings are not ready yet.";
            s.grind_assist = static_cast<int>(std::lround(*value));
            s.tr_option_at = 0; // read back from the game on the next tick
            s.view_due = true;
            return std::format("Grind Assist (the game's own setting) is {}.", s.grind_assist);
        }
        const auto word = [](std::int8_t value) { return value < 0 ? "-" : value ? "on" : "off"; };
        if (what.empty()) {
            std::string out = std::format("Side force off grinds x {:.2f}.", s.grind_side);
            for (std::size_t i = 0; i < grind_settings.size(); ++i)
                out += std::format("\n{} (setting {}, the game's default {}): {}; asked {} times since last listed", grind_settings[i].key, grind_settings[i].setting,
                                   grind_settings[i].stock ? "on" : "off",
                                   s.grind_setting[i] < 0 ? "game's own" : s.grind_setting[i] ? "forced on" : "forced off", grind_setting_asked(i));
            for (std::size_t i = 0; i < grind_switches.size(); ++i)
            {
                std::uint32_t handle{};
                const int status = grind_switch_status(s.base, static_cast<std::uintptr_t>(s.entity), i, handle);
                std::uint32_t asked{}, answered{};
                grind_switch_asked(i, asked, answered);
                out += std::format("\n{} ({}): {}, the game reads {} [{}, handle {:x}; asked {} times since last listed, {} answered held]", grind_switches[i].key,
                                   grind_switches[i].state, s.grind_mode[i] < 0 ? "game's own" : s.grind_mode[i] ? "held on" : "held off", word(s.grind_live[i]),
                                   status == 0 ? "state not found" : status == 1 ? "no handle" : status == 2 ? "no value for this skater" : "ok", handle, asked, answered);
            }
            return out;
        }
        if (what == "feel") {
            // trainer grind feel skate|skate3: the game's own grinds, or without what skate. added to leaving a grind.
            std::string why;
            const bool skate3 = to == "skate3" || to == "3";
            if (!skate3 && to != "skate" && to != "stock") return "error: usage: trainer grind feel skate|skate3";
            if (skate3 && !editable(&why)) return "error: " + why;
            for (std::size_t i = 0; i < grind_switches.size(); ++i) s.grind_mode[i] = skate3 ? skate3_grind_mode(i) : std::int8_t{-1};
            for (std::size_t i = 0; i < grind_settings.size(); ++i) s.grind_setting[i] = skate3 ? grind_settings[i].skate3 : std::int8_t{-1};
            s.grind_push = skate3 ? 1.8f : 0.0f; // Skate 3's pop goes sideways with the left stick, 1.8 m/s at full stick
            // Skate 3's left stick turns the board; the right stick only tips it (5-0, nose grind) and pops.
            s.grind_range = skate3 ? skate3_grind_range : 0.0f;
            changed();
            return std::string{skate3 ? "Grinds like Skate 3: lean exits and skate.'s own ways off a grind off, the pop goes sideways with the stick."
                                      : "Grinds like skate.: every setting and switch is the game's own."};
        }
        if (what == "watch") {
            s.grind_watch = to == "1" || to == "on";
            return std::string{s.grind_watch ? "Reading the grind switches." : "No longer reading the grind switches."};
        }
        std::string why;
        if (what == "turn" || what == "tip" || what == "lean") return "That lever is gone: it did nothing. `trainer grind range <degrees>` is the left stick's turn.";
        if (what == "range") {
            const auto value = number(arg(1));
            if (!value) return "error: usage: trainer grind range <degrees>, 0 = off (the game's own turn, by the right stick); negative turns the other way.";
            if (*value != 0 && !editable(&why)) return "error: " + why;
            s.grind_range = std::clamp(static_cast<float>(*value), -1.0e6f, 1.0e6f);
            changed();
            return std::format("Left stick turns the board on a grind: up to {:.1f} degrees{}.", s.grind_range, s.grind_range == 0 ? " (off)" : "");
        }
        if (what == "push") {
            const auto value = number(arg(1));
            if (!value) return "error: usage: trainer grind push <m/s at full stick>, 0 = none, 1.8 = Skate 3's; a negative number pushes the other way.";
            if (*value != 0 && !editable(&why)) return "error: " + why;
            s.grind_push = std::clamp(static_cast<float>(*value), -1.0e6f, 1.0e6f);
            changed();
            return std::format("Sideways pop with the stick: {:.2f} m/s at full stick.", s.grind_push);
        }
        if (what == "side") {
            const auto value = number(arg(1));
            if (!value || *value < 0) return "error: usage: trainer grind side <multiplier>, 1 = the game's own.";
            if (!editable(&why)) return "error: " + why;
            s.grind_side = std::clamp(static_cast<float>(*value), 0.0f, 1.0e6f);
            changed();
            return std::format("Side force off grinds x {:.2f}.", s.grind_side);
        }
        const int mode = to == "on" || to == "1" ? 1 : to == "off" || to == "0" ? 0 : to == "game" || to == "-1" ? -1 : -2;
        if (mode == -2) return "error: usage: trainer grind (lists everything) | feel skate|skate3 | side <multiplier> | push <m/s> | range <degrees> | <setting or switch>|all game|off|on";
        if (mode >= 0 && !editable(&why)) return "error: " + why;
        bool any = false;
        for (std::size_t i = 0; i < grind_settings.size(); ++i)
            if (what == "all" || what == grind_settings[i].key) {
                s.grind_setting[i] = static_cast<std::int8_t>(mode);
                any = true;
            }
        for (std::size_t i = 0; i < grind_switches.size(); ++i)
            if (what == "all" || what == grind_switches[i].key) {
                if (grind_switches[i].read_only && mode >= 0) {
                    if (what != "all") return "error: " + what + " can only be watched: held, it leaves the skater in the grind state on the ground.";
                    continue;
                }
                s.grind_mode[i] = static_cast<std::int8_t>(mode);
                any = true;
            }
        if (!any) return "error: no grind switch is called \"" + what + "\". `trainer grind` lists them.";
        changed();
        return what + ": " + (mode < 0 ? "the game's own" : mode ? "held on" : "held off") + ".";
    }
    if (v == "tp") {
        const auto x = number(arg(0)), y = number(arg(1)), z = number(arg(2));
        if (!x || !y || !z) return "error: usage: trainer tp <x> <y> <z>";
        return go_to({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)}, "that point");
    }
    if (v == "spot") {
        const auto index = number(arg(0));
        if (!index || *index < 1 || *index > static_cast<double>(s.map_file.spots.size())) return "error: this map has no such spot.";
        const auto &spot = s.map_file.spots[static_cast<std::size_t>(*index) - 1];
        return go_to(spot.position, spot.name);
    }
    if (v == "option") {
        const auto name = lower(arg(0));
        bool on{};
        std::string why;
        if (name == "flip_speed" || name == "underflip_speed" || name.starts_with("flip.")) {
            const auto value = number(arg(1));
            auto *option = trick_option(name);
            if (!option) return "error: no such flip trick. flip_speed is all of them; flip.kickflip, flip.360flip ... are one each.";
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            *option = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height" || name == "manual_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height
             : name == "manual_height" ? s.manual_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "gravity") {
            const auto value = number(arg(1));
            if (!value) return "error: gravity needs a multiplier, 1 = the game's own (0.5 floats, 2 drops fast).";
            if (!editable(&why)) return "error: " + why;
            s.gravity = std::clamp(static_cast<float>(*value), gravity_low, gravity_high);
        } else if (name == "gravity_landing") {
            const auto value = number(arg(1));
            if (!value) return "error: gravity_landing needs a multiplier: the gravity the game plans landings with (gravity_landing_own 1 uses it).";
            if (!editable(&why)) return "error: " + why;
            s.gravity_landing = std::clamp(static_cast<float>(*value), gravity_low, gravity_high);
        } else if (name == "gravity_timing") {
            const auto value = number(arg(1));
            if (!value) return "error: gravity_timing needs 0 (the game's animation timing) to 1 (the timing at the game's own gravity).";
            if (!editable(&why)) return "error: " + why;
            s.gravity_timing = std::clamp(static_cast<float>(*value), 0.0f, 1.0f);
        } else if (name == "pump_power") {
            const auto value = number(arg(1));
            if (!value) return "error: pump_power needs a multiplier, 1 = what the game's own pump gains.";
            if (!editable(&why)) return "error: " + why;
            s.pump_power = std::clamp(static_cast<float>(*value), 0.0f, pump_high);
        } else if (name == "revert_boost") {
            const auto value = number(arg(1));
            if (!value) return "error: revert_boost needs a strength: 0 is off, 1 gives back about 2 m/s on an auto revert.";
            if (!editable(&why)) return "error: " + why;
            s.revert_boost = std::clamp(static_cast<float>(*value), 0.0f, revert_boost_high);
        } else if (name == "catch_percent") {
            return "That setting is gone with the switch it belonged to.";
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 3600.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|flip_advanced|flip_pop|flip_flick|flip_limit|underflip_split|gravity_landing_own|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "pad_menu") return "That switch is gone: LB + RB + R3 is the game's bail. Open the menu with Insert.";
        else if (name == "flip_gate") return "That switch is now option flip_limit (Flip landing limit).";
        else if (name == "catch_at") return "That switch is gone: it did not work reliably. Use the flip trick speed.";
        else if (name == "flip_pop") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_pop = on;
        } else if (name == "flip_flick") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_flick = on;
        } else if (name == "flip_limit") {
            if (!on && !editable(&why)) return "error: " + why;
            s.flip_limit = on;
        } else if (name == "flip_advanced") {
            if (on && !editable(&why)) return "error: " + why;
            s.flip_advanced = on;
        } else if (name == "underflip_split") {
            if (on && !editable(&why)) return "error: " + why;
            s.underflip_split = on;
        } else if (name == "gravity_landing_own") {
            if (on && !editable(&why)) return "error: " + why;
            s.gravity_landing_own = on;
        }
        else if (name == "log") {
            set_logging(on);
            if (on && !s.logging) return "error: the telemetry file could not be created.";
        } else return "error: no option is called \"" + name + "\".";
        changed();
        return name + " = " + arg(1);
    }
    if (v == "profile") {
        const auto action = lower(arg(0));
        if (s.map.empty()) return "error: load a level first.";
        if (action == "clear") {
            current_map().preset.clear();
            changed();
            return "This map no longer applies a preset on load.";
        }
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action != "set" || name.empty()) return "error: usage: trainer profile set <preset> | clear";
        current_map().preset = name;
        changed();
        return "\"" + name + "\" will be applied whenever this map loads.";
    }
    if (v == "setup") {
        // Any one of the trainer's own settings by its preset key (the menu's quick settings): trainer setup grind.push 1.8
        const auto *one = find_setup(lower(arg(0)));
        const auto value = number(arg(1));
        if (!one || !value) return "error: usage: trainer setup <key> <value> (the keys are the ones presets use: trick.flip_speed, grind.push, transition.lean ...)";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        one->set(*value);
        changed();
        return std::format("{} = {:.6g}", one->key, one->get());
    }
    if (v == "fold") {
        bool open{};
        if (arg(0).empty() || arg(0).size() > 120 || !flag(arg(1), open)) return "error: usage: trainer fold <section or card> 0|1";
        s.folds[arg(0)] = open;
        s.save_due = true;
        s.view_due = true;
        return std::string{};
    }
    if (v == "open") {
        const auto tab = lower(arg(0));
        // After "movement": a section of the Tune tab, opened and scrolled to (realistic, fun: lists of earlier versions).
        const std::array<std::string_view, 19> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything", "movement", "trickline", "transition",
                                                    "pop", "speed", "bails", "grinds", "pumping", "foot", "tricklining", "tricks", "game"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end())
            return "error: usage: trainer open [tune|practice|map|movement|presets|game|pop|speed|bails|grinds|tricklining|transition|pumping|foot|everything]";
        s.open_tab = tab.empty() ? 0 : static_cast<int>(found - tabs.begin());
        ++s.open_serial;
        s.view_due = true;
        return "Opening the trainer.";
    }
    if (v == "jumps") {
        const auto &j = s.telemetry.last;
        if (!j.serial) return "No jump measured yet.";
        return std::format("Jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, landing {:.1f} km/h.",
                           j.serial, j.takeoff_speed * 3.6f, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed * 3.6f);
    }
    if (v == "where") {
        if (!s.telemetry.skater) return "error: no skater.";
        const auto &p = s.telemetry.position;
        return std::format("({:.2f}, {:.2f}, {:.2f}) heading {:.0f}, {:.1f} km/h. Blender: ({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2],
                           s.telemetry.heading, s.telemetry.speed * 3.6f, p[0], -p[2], p[1]);
    }
    if (v == "classes") {
        if (lower(arg(0)) == "find") {
            find_classes();
            return "Looking for the game's tuning classes.";
        }
        return "Copies found: " + classes_summary();
    }
    if (v == "refresh") return tuning::refresh_skater(s.base, s.entity) ? "Skater copy refreshed." : "error: the skater has no cached copy right now.";
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
    if (v == "livedump") {
        // Diagnostic: what the game itself holds right now (its live tuning, and the first copy of each class value), not the
        // trainer's own numbers: `trainer livedump <name>` before and after changing one of the game's settings shows what
        // that setting changes. Writes tuning-live-<name>.txt.
        const auto name = arg(0).empty() ? std::string("now") : arg(0);
        if (std::ranges::any_of(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_'; }))
            return "error: a name of letters, digits, - and _.";
        tuning::Values live;
        if (!tuning::read_live(s.base, live)) return "error: the game's tuning is not loaded.";
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / std::filesystem::path("tuning-live-" + name + ".txt"), std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        std::size_t count{};
        for (const auto &e : s.entries) {
            double now{};
            if (e.class_field >= 0) {
                FieldCopy copy{};
                if (class_field_copies(static_cast<std::size_t>(e.class_field), &copy, 1) == 0 || !copy.address) continue;
                float value{};
                if (!peek_float(copy.address, value)) continue;
                now = value;
            } else if (e.kind == Kind::curve || e.kind == Kind::graph || static_cast<std::size_t>(e.offset) + e.size > live.image.size()) {
                continue;
            } else {
                now = read_field(live.image, e);
            }
            file << std::format("{}\t{:.6g}\n", e.id, now);
            ++count;
        }
        for (const auto key : {grind_assist_setting, std::string_view("egf_intentpumpingassistamount")})
            if (const auto held = profile_runtime::local_game_option(key)) file << std::format("setting:{}\t{:.6g}\n", key, *held);
        return std::format("Wrote {} live values to {}.", count, path_utf8(directory / std::filesystem::path("tuning-live-" + name + ".txt")));
    }
    if (v == "dump") {
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / L"tuning-values.txt", std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        for (const auto &e : s.entries)
            file << std::format("{}\t0x{:x}\t{}\t{:.6g}\t{:.6g}\n", e.id, e.offset,
                                e.kind == Kind::real ? "real" : e.kind == Kind::flag ? "flag" : e.kind == Kind::curve ? "curve" :
                                e.kind == Kind::graph ? "graph" : "int", e.stock, e.value);
        return std::format("Wrote {} values to {}.", s.entries.size(), path_utf8(directory / L"tuning-values.txt"));
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|open|set|freeze|reset|find|preset|slot|marker|tp|ground|waypoint|spot|option|profile|jumps|where|states|dump|selftest";
}
} // namespace

namespace {
TuningValue tuning_of(const Entry &e) {
    const auto &s = state();
    const auto held = s.plugin_tunes.find(e.key);
    return {e.id, e.label, e.group, e.kind, e.value, e.stock, held == s.plugin_tunes.end() ? std::string{} : held->second.owner};
}
} // namespace

std::optional<TuningValue> tuning_value(std::string_view id, std::string *why) {
    if (!state().model) {
        if (why) *why = "the game's physics tuning is not read yet";
        return std::nullopt;
    }
    const auto *e = find_entry(id);
    if (!e) {
        if (why) *why = "no tuning value is called \"" + std::string(id) + "\"";
        return std::nullopt;
    }
    return tuning_of(*e);
}
std::vector<TuningValue> find_tuning(std::string_view text, std::size_t most) {
    std::vector<TuningValue> found;
    const auto pattern = lower(text);
    for (const auto &e : state().entries)
        if (found.size() < most && matches(e.key, pattern)) found.push_back(tuning_of(e));
    return found;
}
std::string plugin_tune(std::string_view owner, std::string_view id, std::optional<double> value) {
    auto &s = state();
    if (!s.model) return "the game's physics tuning is not read yet";
    auto *e = find_entry(id);
    if (!e) return "no tuning value is called \"" + std::string(id) + "\"";
    const auto held = s.plugin_tunes.find(e->key);
    if (held != s.plugin_tunes.end() && held->second.owner != owner) return held->second.owner + " tunes " + e->id;
    if (!value) {
        if (held != s.plugin_tunes.end()) {
            const double before = held->second.before;
            s.plugin_tunes.erase(held);
            set_entry(*e, before, false);
            changed(false);
        }
        return {};
    }
    std::string why;
    if (!editable(&why)) return why;
    if (is_locked(*e)) return "the player has locked " + e->id + " in the trainer";
    if (!std::isfinite(*value)) return "a tuning value is a finite number";
    if (held == s.plugin_tunes.end()) s.plugin_tunes[e->key] = {std::string(owner), e->value};
    set_entry(*e, *value, false);
    changed(false);
    return {};
}
void release_plugin_tunes(std::string_view owner) {
    auto &s = state();
    std::vector<std::string> mine;
    for (const auto &[key, tune] : s.plugin_tunes)
        if (tune.owner == owner) mine.push_back(key);
    for (const auto &key : mine) (void)plugin_tune(owner, key, std::nullopt);
}
bool stage_import(std::string_view text) noexcept {
    try {
        const auto directory = shared_directory();
        if (directory.empty() || text.empty() || text.size() > 256 * 1024) return false;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / "clipboard.txt", std::ios::binary | std::ios::trunc);
        file << text;
        return static_cast<bool>(file);
    } catch (...) {
        return false;
    }
}
std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto result = run(verb, arguments);
    // The menu shows the last answer, except for the ones a dragged slider sends every frame.
    if (verb != "set" || result.starts_with("error")) {
        auto &s = state();
        s.last = result;
        s.view_due = true;
    }
    return result;
}
} // namespace dingosdk::trainer
