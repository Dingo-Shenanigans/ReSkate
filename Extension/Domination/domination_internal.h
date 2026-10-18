#pragma once
// Domination's game side, shared by its files. Not for other code: domination.h is the API.
// - domination.cpp: rounds (the game around domination_sync.h's rules), the console, the menu's
//   snapshot and the overlay's view;
// - domination_files.cpp: park files, the spots' outlines on the ground;
// - domination_hud.cpp: tricks read from skate.'s score HUD;
// - domination_tools.cpp: park-making (scan, tp, survey).
// The rules themselves are apart from the game: domination_model.h (parks, spots, reading
// tricks, the board, a round's results), domination_sync.h (multiplayer rounds) and
// domination_wire.h (the messages).
#include "domination.h"
#include "domination_model.h"
#include "domination_sync.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dingosdk::domination::detail {
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;
using Clock = std::chrono::steady_clock;

constexpr double default_round = 300;  // seconds
constexpr double results_shown = 12;    // seconds after a round (gone sooner once the skater rides off)
constexpr double toast_shown = 4;

// ---- the score HUD ----------------------------------------------------------------------
// ScoringHUDViewModel's fields, by the hashes of their names in the game's own type data
// (ui/features/hud/scoring/ScoringHUDViewModel; HudWidgetActive 0x3aa2f739 and ExtraInfoStyle
// 0x0426783f are the ones hud_corner.h already uses). The round reads these four.
struct Watched {
    const char *name;
    std::uint32_t hash;
};
enum : std::size_t { line_score, is_slam, multiplier, action_active };
constexpr std::array<Watched, 4> watched{{
    {"LineScore", 1086322279u},
    {"IsSlam", 2943226809u},
    {"Multiplier", 1782700230u},
    {"ActionActive", 2058053468u},
}};
constexpr std::size_t value_bytes = 16; // read per field

struct Bound {
    bool present{};
    std::uint16_t offset{};
    unsigned size{}, kind{};
    std::array<unsigned char, value_bytes> now{};
    bool read{};
};
struct Hud {
    Clock::time_point next_look;
    Address manager{};
    menu_data::Value model;
    std::array<Bound, watched.size()> fields{};
    bool warned{};
    bool blind_told{}; // a round has been told that tricks can't be read (this build of the game)
};

// A score field's value: the game's widget logic keeps scores as floats (kind 19, as LineTimer
// and Multiplier); an integer reading is taken if the float one is not sensible.
inline float score_of(const Bound &field) {
    if (!field.present || !field.read || field.size != 4) return 0;
    float f{};
    std::int32_t i{};
    std::memcpy(&f, field.now.data(), 4);
    std::memcpy(&i, field.now.data(), 4);
    if (std::isfinite(f) && (f == 0 || (std::abs(f) > 1e-3f && std::abs(f) < 1e9f))) return f;
    return static_cast<float>(i);
}
inline bool flag_of(const Bound &field) { return field.present && field.read && field.now[0] != 0; }

// ---- the round --------------------------------------------------------------------------
// The rounds themselves (who runs them, joining, the clock, teams, the board, the results) are
// domination_sync.h's, one Node per park. Here is what the game adds: tricks read from the score
// HUD, the skater put at a start, and what the player sees and hears.
using sync::Ms;
struct Toast {
    std::string text;
    int kind{};
    Clock::time_point until;
    Player who{}; // the player it is about (another player's claim): drawn in their colour
};
// A spot's marker flashes this long when its owner changes.
constexpr double flash_seconds = 1.2;
// Edge arrows point at this many of the nearest spots you do not own.
constexpr std::size_t arrows_shown = 6;
inline Ms ms_of(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

// What the game keeps beside the node's round: one per round (by its number).
struct Play {
    std::uint32_t round{};
    TrickWatch watch;
    std::vector<Clock::time_point> flashed; // when each spot's owner last changed
    int start = -1;                         // this player's start (Park::starts), -1 none
    bool place{};                           // the skater is still to be put at it
    bool done{};                            // its end handled: results up
    bool counted{};                         // in this player's round counts (Stats)
    int beat = -1;                          // the countdown's last number heard (3, 2, 1; 0 GO)
};
// The countdown as shown and heard: 3 (the first two seconds, while the skaters are placed), 2,
// 1, then 0 (GO) the moment tricks start to count. -1 after.
inline int countdown_beat(Ms played) {
    if (played >= sync::countdown_ms + 1200) return -1;
    if (played >= sync::countdown_ms) return 0;
    return std::min(3, static_cast<int>((sync::countdown_ms - played + 999) / 1000));
}
// A round asked for on another map: it starts once that map has loaded and the skater has stood
// in it for a moment.
struct Pending {
    std::string level, name;
    double duration{};
    bool solo{};
    Clock::time_point asked, skater_since;
};
constexpr auto pending_patience = std::chrono::minutes(4);
constexpr auto settle_after_load = std::chrono::seconds(3);
// In a session the other players load the map after the host: a round started from another map
// waits this long for them (one still loading joins the round when they arrive).
constexpr auto settle_in_session = std::chrono::seconds(10);

// ---- scan -------------------------------------------------------------------------------
// One downward ray per grid point, x fastest; heights in world metres, NaN where nothing was hit.
// File: "DOMSCAN1", u32 nx, u32 nz, f32 x0, z0, step, top, bottom, then nx*nz f32.
struct Scan {
    float x0{}, z0{}, step{}, top{}, bottom{};
    std::uint32_t nx{}, nz{};
    std::vector<float> heights;
    std::size_t next{}, misses{};
    Clock::time_point started;
};
inline constexpr auto scan_budget = std::chrono::milliseconds(6); // of each client tick
constexpr std::size_t scan_limit = 8'000'000;              // points
inline Scan scan_job(float x0, float z0, float x1, float z1, float step, float top, float bottom) {
    const auto nx = static_cast<std::uint32_t>(std::floor((x1 - x0) / step)) + 1, nz = static_cast<std::uint32_t>(std::floor((z1 - z0) / step)) + 1;
    Scan job{x0, z0, step, top, bottom, nx, nz};
    job.heights.assign(static_cast<std::size_t>(nx) * nz, std::numeric_limits<float>::quiet_NaN());
    job.started = Clock::now();
    return job;
}

// ---- survey -------------------------------------------------------------------------------
// A walk through a list of places, for finding and making parks (`domination survey`). At each
// place the skater is put there and waits for the area to load (the game loads collision only
// around the skater); the ground is found; a height map of the square around it is written (as
// `domination scan`); then the skater faces north (+z), east (+x), south and west in turn, each
// pose logged with its time, for pictures of the game's window taken alongside.
struct Survey {
    std::vector<std::array<float, 3>> places;
    std::size_t next{}, missed{};
    float half{}, step{}; // the height map's half size and step (half 0: none)
    int phase{};          // 0 arrive, 1 look for the ground, 2 the height map, 3-6 the four headings
    float ground{};
    Clock::time_point until, started;
};
constexpr auto survey_settle = std::chrono::seconds(3);
constexpr auto survey_look = std::chrono::milliseconds(1500);

// Every park there is (the files, then the ones built in), whatever map is loaded.
struct KnownPark {
    std::string id; // the file's name without .json
    std::string where;
    Park park;
    std::uint32_t hash{};
};
// Whether a park's map can be played here: the game's own, or a map mod installed and switched on.
using MapState = MapStatus;
constexpr auto map_check_every = std::chrono::seconds(10);
// The rounds this player has played to the end, saved with their profile ("ReSkate.Domination.*"):
// in all, with other players, won with them, and per park (by id). Read once the profile is there.
struct Stats {
    bool loaded{};
    int rounds{}, online{}, won{};
    std::map<std::string, int, std::less<>> parks;
};

struct State {
    std::mutex mutex;
    Address base{};
    std::string level;
    std::optional<Park> park;
    std::string park_id; // the loaded park's (a KnownPark's id)
    std::string park_problem;
    std::uint32_t park_hash{}; // the same park on every player's side: its level and spot ids
    std::vector<KnownPark> parks;
    std::vector<std::pair<std::string, std::string>> park_problems; // park files refused: level, "<id>: why"
    // The park the next round is played in (`domination set park`): empty, this map's first park.
    std::string chosen;
    std::map<std::string, MapState> maps; // per level (lower case), checked every map_check_every
    Clock::time_point next_map_check{};
    // The next round's settings (`domination set`, or a menu through it).
    int next_duration = static_cast<int>(default_round);
    bool next_solo{};
    bool next_teams{}; // free-for-all by default
    std::optional<sync::Node> node; // this map's park's rounds
    std::optional<Play> play;
    std::optional<Pending> pending;
    std::map<Player, std::string> names; // players' names (the session's roster)
    std::vector<std::vector<std::uint8_t>> outgoing; // messages for the session to send
    Clock::time_point next_roster;
    Clock::time_point results_until;
    std::deque<Toast> toasts;
    Clock::time_point last_tick;
    Vec3 position{};
    bool skater{};
    int here = -1;
    Hud hud;
    std::optional<Scan> scan;
    std::string last_scan; // the last finished scan's file, or why it failed
    // `domination tp`: a teleport facing a heading (degrees), done at the next client tick.
    std::optional<std::pair<Vec3, float>> teleport;
    std::optional<Survey> survey;
    // The spots' outlines on the ground: points along their edges about half a metre apart, each one's
    // height found by a ray once the skater is near (collision streams in around them; client
    // thread, a little each tick). A spot's outline is drawn once all its heights are known.
    std::vector<std::vector<Vec3>> outlines;
    std::vector<Clock::time_point> outline_since; // per spot: its latest ground hit, the skater near since
    std::vector<Clock::time_point> outline_retry; // per spot: its misses tried again from then
    std::size_t outline_turn{};                   // the spot measured first on the next tick
    // What the camera can see of the outlines near the skater: per point, whether a ray from the
    // camera reaches it (found a few at a time), and how much of it is drawn (easing to that).
    std::vector<std::vector<std::uint8_t>> outline_seen;
    std::vector<std::vector<float>> outline_shown;
    std::size_t sight_cursor{};
    Vec3 camera{};
    Clock::time_point camera_at{}, shown_at{};
    std::size_t outline_left{};
    Stats stats;
};
// The one state, behind its mutex (domination.cpp).
State &state();

// ---- shared helpers (domination_files.cpp) --------------------------------------------------
std::filesystem::path data_directory();
std::filesystem::path game_directory();
std::optional<Json> read_json(const std::filesystem::path &path);
std::string lower(std::string value);
std::string points(float score);
void say(logging::Level level, const std::string &text);
std::optional<float> number(const std::string &word);
void toast(State &s, std::string text, int kind, Player who = 0);
// Park files.
void scan_parks(State &s);
// The loaded map's park: the chosen one when it is on this map, else the map's first.
void load_park(State &s);
// Makes `park` the loaded one (it must be on the loaded map).
void use_park(State &s, const KnownPark &park);
std::uint32_t park_hash(const Park &park);
const KnownPark *find_park(const State &s, std::string_view id);
// The park a round is played in until another is picked (when its map is here).
inline constexpr std::string_view default_park = "skate3improved-redbowl";
// The park the next round is played in: the chosen one, else default_park, else this map's first,
// else the first.
const KnownPark *next_park(const State &s);
// The park's map as players know it.
std::string map_name(const Park &park);
void check_maps(State &s, Clock::time_point now);
MapState map_state(const State &s, const Park &park);
void tick_outlines(State &s, const Vec3 &skater);
// Outlines are drawn near the skater only, fading out between these (m, seen from above).
inline constexpr float outline_near = 18, outline_far = 32;
void tick_sight(State &s, const Vec3 &skater);
std::string stamp(); // the local time, for file names: 20261017-130012
// ---- park-making (domination_tools.cpp) ----------------------------------------------------
void tick_scan(State &s);
void tick_survey(State &s, Clock::time_point now);
bool face(const Vec3 &p, float heading);
// ---- the score HUD (domination_hud.cpp) ----------------------------------------------------
// Reads every watched field into `now`. False without the model.
bool read_hud(State &s);
} // namespace dingosdk::domination::detail
