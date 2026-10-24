#pragma once
// The ReSkate Trainer: live physics tuning, presets, practice markers and a telemetry HUD.
//
// The game thread owns the trainer's state (trainer.cpp). Every change is a `trainer ...`
// console command, so the menu page (presentation thread) only queues commands and reads
// the two snapshots below; nothing here touches the game from the UI.
#include "trainer_gamestate.h"
#include "Extension/Skater/client_source_spawn.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::trainer {
// curve: a FloatCurve the asset points at; graph: a table of points stored in the asset
// itself (X0..X15, Y0..Y15). Both are edited as one multiplier on their outputs.
enum class Kind : std::uint8_t { real, integer, flag, curve, graph };
// The sections of the Tune tab: which one shows a value, a dial or a setting of the trainer's own (0: none, the value is
// only on EVERY VALUE).
inline constexpr std::uint8_t topic_none = 0, topic_game = 1, topic_pop = 2, topic_speed = 3, topic_bails = 4, topic_grinds = 5, topic_trickline = 6,
                              topic_transition = 7, topic_pumping = 8, topic_foot = 9, topic_count = 10;
struct Topic {
    std::string_view key;   // `trainer open <key>`, `trainer reset section <key>`, the fold's id
    std::string_view title; // in the menu
    std::string_view about;
};
inline constexpr std::array<Topic, topic_count> topics{{
    {"everything", "EVERY VALUE", "The whole tuning table, group by group"},
    {"game", "PRESETS AND QUICK SETTINGS", "Pick a preset, then change everything that is set in one place"},
    {"pop", "POP AND TRICKS", "Ollie and trick heights, flip trick speed, body flips and spins"},
    {"speed", "SPEED AND PUSHING", "Push speed, auto push, rolling, powerslides, getting on the board"},
    {"bails", "BAILS AND LANDINGS", "What knocks you off, and how the legs take a landing"},
    {"grinds", "GRINDS", "Grinds feel like, exits and transfers, pops off a grind, lock-on, friction"},
    {"tricklining", "TRICKLINING", "Board bending, reverts, Dark Pop, manuals"},
    {"transition", "TRANSITION", "Coping grinds, coming back in, handplants, lip tricks, vert airs"},
    {"pumping", "PUMPING", "Speed without pushing"},
    {"foot", "ON FOOT", "Jumping, sprinting, gliding and wallruns off the board"},
}};
constexpr std::uint8_t topic_named(std::string_view key) noexcept {
    for (std::uint8_t i = 1; i < topic_count; ++i)
        if (topics[i].key == key) return i;
    return key == "trickline" ? topic_trickline : key == "tricks" ? topic_pop : topic_none;
}

// One editable value of Gameplay/SkatePhysicsTuning. `id` is the name the game's data gives
// it ("PhysicsMode.JumpMaxHeight").
struct Row {
    std::string id, label, group;
    Kind kind{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{}; // a single point or bound of a graph: hidden unless asked for
    std::string friendly; // a plain name, for the handful of values on the Essentials list
    int rank{};           // its place among the named values, from 1; 0: not named
    std::uint8_t topic{}; // the section of the Tune tab it is on (topic_*)
    bool trial{};         // shown there although the game was not seen reading it: named for players to try out
    bool used{};          // the game's code was found to read it
    std::string help;     // what the value does, in a sentence; empty for most
};
struct PresetRow {
    std::string name, note;
    bool builtin{}, active{}; // active: every value it sets holds what it sets, right now
    // A dial: one multiplier that moves everything the preset moves, 1 = the game's own and
    // `amount` = the preset as it ships. `factor` is where its first value stands.
    bool dial{};
    std::string title;
    double factor{1}, amount{1};
    std::uint8_t topic{}; // the section of the Tune tab that shows it (topic_*)
};
// One of the trainer's own settings that is off the game's own (the menu's quick settings change it with `trainer setup`).
inline constexpr std::uint8_t setup_number = 0, setup_switch = 1, setup_mode = 2; // a number / off or on / the game's own, held off, held on
struct SetupRow {
    std::string key, title;
    std::uint8_t topic{}, kind{};
    double value{}, stock{}, low{}, high{};
};
struct Marker {
    bool set{};
    std::array<float, 3> position{};
};
struct Spot {
    std::string name;
    std::array<float, 3> position{};
};
inline constexpr std::size_t marker_slots = 5;

// Changes rarely: rebuilt when a command or a level load changes something.
struct View {
    std::uint64_t revision{};
    bool ready{};        // the game's tuning is read and the running game's copy was found
    std::string status;  // why not
    std::string last;    // what the last command answered
    bool editable{};     // false while a session's host sets everyone's physics (the host's apply)
    std::string blocked; // the reason shown on locked rows
    bool session_enforced{}; // a guest where the host (or the server) sets everyone's physics
    bool boosts_blocked{};   // a session whose host turned boosts off: the trick extras are the game's own
    std::vector<Row> rows;
    std::vector<std::string> groups;
    std::vector<PresetRow> presets;
    std::size_t touched{};
    // Nothing the trainer does is in force: no changed value and none of its own settings (tricks, grinds, transition,
    // bending rules) off the game's own. No Bail, Noclip, Dark Pop and the game speed are ReSkate's: the menu adds those.
    bool stock{true};
    std::size_t setups_changed{}, locks{};           // the trainer's own settings off stock; locked values
    std::vector<SetupRow> setups;                    // those settings, one by one
    std::array<std::uint16_t, topic_count> changed{}; // what is changed in each section of the Tune tab ([0]: values on no section)
    std::array<std::uint16_t, topic_count> settings{}; // of those, the trainer's own settings (not tuning values)
    std::map<std::string, bool, std::less<>> folds;   // sections and cards the player opened or closed
    // Practice
    int slot{};
    std::array<Marker, marker_slots> markers{};
    bool auto_return{};
    float return_delay{1.5f};
    bool pad_shortcuts{};
    // Height of the hippy jump, which the game scripts instead of tuning (x of its own height).
    float hippy_height{1};
    // The same for the no comply and the boneless.
    float nocomply_height{1}, boneless_height{1};
    float manual_height{1}; // the pop out of a manual
    float gravity{1};       // x the game's own gravity
    bool gravity_live{true}; // false: this game build's gravity is not where the trainer knows it
    bool gravity_landing_own{}; // landings are planned with gravity_landing, not with gravity
    float gravity_landing{1};   // x the game's own gravity, for the game's landing plans
    float gravity_timing{1};    // 0: the game's animation timing in the air; 1 (default): the timing at the game's own gravity
    float grind_side{1};    // the sideways part of a pop out of a grind
    float grind_push{};     // Skate 3's pop: m/s sideways at full stick (0: none)
    float grind_range{}; // Skate 3's turn: the left stick turns the board on a grind up to this many degrees (0: off)
    std::array<std::int8_t, grind_settings.size()> grind_setting{}; // the game's own grind settings: -1 its own / 0 off / 1 on
    std::array<std::int8_t, grind_switches.size()> grind_mode{}, grind_live{}; // -1 the game's own / 0 off / 1 on; what each reads (-1: not found)
    // Transition skating: the trainer's own help (0 / false: off) and the game's own settings (-1 its own / 0 off / 1 on).
    float tr_lean{}, tr_carry{}, tr_drop{1}, tr_unstick{}, tr_lockout{}, tr_plant{};
    bool tr_spine{};
    std::array<std::int8_t, transition_settings.size()> tr_setting{};
    std::array<int, transition_options.size()> tr_option{}; // the game's own options, as the game holds them now
    // What the player's own game settings hold for the settings the trainer can force (0 off / 1 on).
    std::array<std::int8_t, grind_settings.size()> grind_own{};
    std::array<std::int8_t, transition_settings.size()> tr_own{};
    float revert_boost{}; // strength of the speed given back on an auto revert; 0: off
    float pump_power{1};  // x what pumping a transition gains
    bool pump_live{};     // the game's pumping states are found: the pump power acts
    bool pump_blocked{};  // set, but this session's rules keep the game's own pumping
    RevertTuning revert;  // what counts as a revert and what it is worth
    float offboard_height{1}; // a jump on foot
    float flip_speed{1};
    float underflip_speed{1}; // while an underflip turns, times the flip speed      // board flip tricks: x of the game's own speed
    bool underflip_split{};   // Underflip speed for the under part only; the underflip's flip at Flip trick speed
    bool flip_advanced{};     // each flip trick also has a speed of its own
    bool flip_pop{};          // a slowed flip trick pops higher
    bool flip_flick{};        // a slow flick turns the board as fast as a full one
    std::array<float, flip_tricks.size()> flip_trick{}; // those, in the order of flip_tricks
    bool flip_advanced_blocked{};       // ticked, but this session's rules keep the game's own flips
    bool flip_live{};                   // the game's flip states are found: the speeds act
    // HUD
    bool hud{}, hud_jump{}, logging{};
    // The loaded map and what its author ships for the trainer (Mods/<mod>/trainer.json).
    std::string map, map_note, map_preset, profile_preset;
    std::vector<Spot> spots;
    // The waypoint placed on the game's pause map (trainer_waypoint.h), if any.
    bool waypoint_set{};
    std::array<float, 3> waypoint{};
    // `trainer open <tab>`: each new serial opens the menu on the trainer page at that tab.
    std::uint64_t open_serial{};
    std::string share_text; // `preset export`: the line to put on the clipboard
    std::uint64_t share_serial{};
    int open_tab{};
};

struct Jump {
    std::uint64_t serial{}; // 0: none yet
    float takeoff_speed{}, takeoff_angle{}, air_time{}, height{}, distance{}, drop{}, landing_speed{};
    float spin{}, spin_rate{}; // degrees turned about the vertical in the air, and the fastest rate (deg/s)
    float flip{};              // degrees the skater's up axis tumbled (body flips and rolls)
    float board_turn{};        // the board's fastest turn rate, degrees per second (flip tricks, shuvits)
    std::uint32_t state{};     // the physics state it took off into (tells trick kinds apart)
    std::array<float, 3> takeoff{}, landing{};
};
// Changes every client tick.
struct Telemetry {
    bool skater{};
    std::array<float, 3> position{};
    float heading{};           // degrees, 0 = +Z, clockwise seen from above
    float speed{}, vertical{}; // metres per second: over the ground, and upward
    bool airborne{};
    float air_time{}, height{}; // of the jump in progress
    std::uint32_t physics_state{};
    Jump last, best;
    float top_speed{};
};

// Thread-safe snapshots (trainer_view.cpp, part of the overlay).
std::shared_ptr<const View> view() noexcept;
Telemetry telemetry() noexcept;
void publish(std::shared_ptr<const View>) noexcept;
void publish(const Telemetry &) noexcept;
// The menu is drawing the list of every value, the game's tuning classes among them. Those
// have to be found in memory before an edit to one does anything, and the search is only run
// for a player who has a use for it: this is one. Any thread; the game thread takes the note.
void note_class_list_shown() noexcept;
bool take_class_list_shown() noexcept;
// The menu hands over text to import (a pasted preset line): written where `preset import`
// reads it. Any thread; touches no trainer state.
bool stage_import(std::string_view text) noexcept;
// The menu is drawing the TELEPORT card, which shows the pause map's waypoint. The map's
// registry is read only while it is. Any thread; the game thread takes the note.
void note_teleport_card_shown() noexcept;
bool take_teleport_card_shown() noexcept;

// Game thread (trainer.cpp).
// Each client tick; `playing` while a local skater can exist, `level` the loaded level asset.
void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept;
// Runs one `trainer` command and returns the line to print. Only from the console dispatcher.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);

// Plugins' tuning (reskate.physics.tuning, find, tune), on the game thread. A plugin's value goes over
// the player's own for as long as the plugin keeps it and is never saved; letting go puts back what
// was there. The player changing that value in the trainer takes it back from the plugin.
struct TuningValue {
    std::string id, label, group;
    Kind kind{};
    double value{}, stock{};
    std::string tuner; // the plugin whose value it is, if any
};
// Empty, and why, while the game's tuning is not read or there is no such value.
std::optional<TuningValue> tuning_value(std::string_view id, std::string *why = nullptr);
// Values whose id has every word of `text` (a word starting with ! must not be in it), at most `most`.
std::vector<TuningValue> find_tuning(std::string_view text, std::size_t most);
// Empty when done, else why not. No value: lets go of this plugin's.
std::string plugin_tune(std::string_view owner, std::string_view id, std::optional<double> value);
void release_plugin_tunes(std::string_view owner);
} // namespace dingosdk::trainer
