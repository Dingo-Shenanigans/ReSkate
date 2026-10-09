#pragma once
// The ReSkate Trainer: live physics tuning, presets, practice markers and a telemetry HUD.
//
// The game thread owns the trainer's state (trainer.cpp). Every change is a `trainer ...`
// console command, so the menu page (presentation thread) only queues commands and reads
// the two snapshots below; nothing here touches the game from the UI.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::trainer {
// curve: a FloatCurve the asset points at; graph: a table of points stored in the asset
// itself (X0..X15, Y0..Y15). Both are edited as one multiplier on their outputs.
enum class Kind : std::uint8_t { real, integer, flag, curve, graph };
// The Tune tab's two short lists: which of them a value is on (Row::modes).
inline constexpr std::uint8_t mode_realistic = 1, mode_fun = 2;

// One editable value of Gameplay/SkatePhysicsTuning. `id` is the name the game's data gives
// it ("PhysicsMode.JumpMaxHeight").
struct Row {
    std::string id, label, group;
    Kind kind{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{}; // a single point or bound of a graph: hidden unless asked for
    std::string friendly; // a plain name, for the handful of values on the Essentials list
    int rank{};           // its place on that list, from 1; 0: not on it
    std::uint8_t modes{}; // which short lists it is on (trainer_presets.h: mode_realistic, mode_fun)
    bool used{};          // the game's code was found to read it
};
struct PresetRow {
    std::string name, note;
    bool builtin{}, active{}; // active: every value it sets holds what it sets, right now
    // A dial: one multiplier that moves everything the preset moves, 1 = the game's own and
    // `amount` = the preset as it ships. `factor` is where its first value stands.
    bool dial{};
    std::string title;
    double factor{1}, amount{1};
    std::uint8_t modes{}; // which of the short lists show it (mode_realistic, mode_fun)
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
    std::vector<Row> rows;
    std::vector<std::string> groups;
    std::vector<PresetRow> presets;
    std::size_t touched{};
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
    float offboard_height{1}; // a jump on foot
    float flip_speed{1};      // board flip tricks: x of the game's own speed
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
// The menu is drawing the TELEPORT card, which shows the pause map's waypoint. The map's
// registry is read only while it is. Any thread; the game thread takes the note.
void note_teleport_card_shown() noexcept;
bool take_teleport_card_shown() noexcept;

// Game thread (trainer.cpp).
// Each client tick; `playing` while a local skater can exist, `level` the loaded level asset.
void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept;
// Runs one `trainer` command and returns the line to print. Only from the console dispatcher.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
} // namespace dingosdk::trainer
