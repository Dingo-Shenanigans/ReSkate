#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Domination (skate. 3's spot mode; Extension/Domination/README.md).
// - The park's spots come from a park file for the loaded level: the domination\parks folder
//   under <LOCALAPPDATA>\ReSkate, or Mods\<mod>\domination*.json (domination_model.h).
// - `domination start [seconds] [solo]` runs a round (5 minutes by default). Each landed trick
//   counts for the spot it was done on, and the best single trick on a spot owns it, the moment
//   it lands; a higher one steals it. Tricks, their points and slams are read from skate.'s score
//   HUD view model (ScoringHUDViewModel: ActionActive, LineScore, Multiplier, IsSlam) and the
//   skater's physics state (domination_model.h).
// - In a session the rounds are everyone's (domination_sync.h): the session's host starts and
//   stops them (on a dedicated server, the player with the lowest Steam ID), and every player on
//   the map is taken into each round at once, late too. Every player sends the whole board they
//   know every few seconds (domination_wire.h), so all of them end with the same board and the
//   same results. Every round starts with every spot open, alone too.
// - Teams (`domination set teams on`, off by default): red against blue.
// - At the end: the results (who owns what, the standings, the most steals, the best trick, the
//   longest held).
namespace dingosdk::domination {
// Startup, with the game's image base.
void start(std::uintptr_t base) noexcept;
// Client thread: every tick while a level is loaded, and the level when it changes.
void on_client_tick() noexcept;
void set_level(std::string_view level) noexcept;

// Console verbs; each returns the console's reply. Any thread.
std::string round_start(const std::vector<std::string> &words);
std::string round_stop();
std::string status();
std::string spots();
// Every park there is, with its map and whether that map is installed here.
std::string parks();
// The rounds this player has played to the end: in all, with others and won, per map and per park.
std::string stats();
// Park-making: a height map of an area (one ground ray per grid point, written to a file), a
// teleport facing a heading (degrees from +z towards +x), and a survey of a list of places (at
// each: a height map around it, and the skater facing north, east, south and west).
std::string scan(const std::vector<std::string> &words);
std::string teleport(const std::vector<std::string> &words);
std::string survey(const std::vector<std::string> &words);
// Back into the session's round, for a player who left it (everyone else is in it already).
std::string join();
// The next round's settings: duration <seconds>, players everyone|solo, teams on|off.
std::string settings(const std::vector<std::string> &words);

// Multiplayer (domination_wire.h), on the session's client thread: a Domination message another
// player sent (the session hands these over before the throwdown relay sees its inbox), and the
// messages to send.
void receive(std::uint64_t sender, std::span<const std::uint8_t> bytes) noexcept;
std::vector<std::vector<std::uint8_t>> take_outgoing() noexcept;

// Whether a park's map can be played here (from the Mods folder): the game's own map or a map mod
// installed and on; a map mod installed but switched off; not installed; not known yet.
enum class MapStatus { unknown, installed, off, missing };
struct SnapshotPark {
    std::string id, name, map, package; // map: as players know it; package: a map mod's Thunderstore "Owner-Name"
    int spots = 0;
    MapStatus status{};
    bool here = false; // on the loaded map
    int played = 0;    // rounds this player has played in it to the end
};
// What a menu shows of Domination (any thread): the park the next round is played in, every park
// there is, the next round's settings, who starts rounds, and the round going on.
struct Snapshot {
    bool started = false;   // the mode is in this build
    bool available = false; // the next round's park can be played: on this map, or on one that is installed
    std::string park, problem;
    std::string park_level;  // the park's map (its level asset), to load it
    std::string map, package; // its map as players know it, and a map mod's Thunderstore package
    MapStatus map_status{};
    std::string map_problem; // why its map can't be played here (not installed, switched off)
    std::vector<SnapshotPark> parks; // every park, in the order `domination set park next` goes
    int chosen = -1;                 // the next round's, in `parks`
    bool elsewhere = false;  // the park is on another map: Start loads it first
    bool pending = false;    // a round waits for that map to load
    int spots = 0, starts = 0;
    int duration = 300; // seconds, the next round's
    bool solo = false;
    bool teams = false; // the next round's option
    bool others = false;   // in a session with other players
    bool controls = true;  // this player starts the rounds (alone: always)
    std::string controller; // in a session: who starts the rounds (their name)
    bool running = false, multiplayer = false, starter = false;
    int left = 0, owned = 0; // seconds; spots you own
    bool countdown = false;  // the round is counting down to its start
    float total = 0;
    int team = -1; // a teams round: yours (0 red, 1 blue)
    std::vector<std::string> standings; // "name: spots, held time (points)", most spots first
    int rounds = 0, online = 0, won = 0; // this player's rounds played to the end: in all, with others, won with them
};
Snapshot snapshot();

// What the overlay draws (domination_overlay.cpp), a copy taken on the render thread.
enum class OverlayOwner { none, you, other };
struct OverlaySpot {
    std::array<float, 3> marker{};
    std::string name;
    OverlayOwner owner{};
    std::uint64_t owner_id{};
    std::string owner_name;
    float top{};  // the owner's score
    float mine{}; // your best on it this round
    bool here{};
    bool ours{};   // yours, or your team's
    int team = -1; // a teams round: its owner's team
    int colour = -1; // its owner, another player: their colour (see OverlayStanding::colour)
    float flash{}; // 1 when its owner just changed, down to 0
    bool arrow{};  // one of the nearest spots that are not yours: an arrow at the screen's edge
    float distance{}; // from the skater, metres
    std::vector<std::array<float, 3>> outline; // its edge on the ground, a closed loop (empty until measured)
    std::vector<float> outline_alpha;          // per point: 0 hidden (behind something, or far from the skater) to 1
};
struct OverlayStanding {
    std::string name;
    std::uint64_t id{};
    OverlayOwner kind{};
    int spots{};
    float total{};
    double hold{}; // seconds owning spots
    int steals{};
    int team = -1; // a teams row: the team
    // Another player: their colour, a slot in the overlay's palette given out in the order of the
    // round's other players' ids (no two alike for up to seven); -1 none.
    int colour = -1;
};
// A line of the feed: kind 0 info, 1 taken, 2 short, 3 bail or lost; another player's claim
// names them (owner, id, team, colour as OverlayStanding's) and is drawn in their colour.
struct OverlayToast {
    std::string text;
    int kind{};
    OverlayOwner owner{};
    std::uint64_t id{};
    int team = -1;
    int colour = -1;
    float age{}, left{}; // seconds since it came, and until it goes
};
struct OverlayView {
    bool visible{};
    bool running{}, results{}, multiplayer{};
    double left{};      // seconds to play (the countdown not counted)
    double duration{};
    int countdown{};    // 3, 2, 1 before the round starts; 0 after
    bool go{};          // just started: GO
    bool teams{};
    int my_team = -1;
    std::vector<OverlaySpot> spots;
    std::vector<OverlayStanding> standings; // most spots first; with teams, the teams
    // Results: who won, and the round's awards (empty when nobody earned one).
    std::string winner, most_steals, best_trick, held_longest;
    double results_age{};    // seconds the results have been up
    bool you_won{}, tie{};   // multiplayer: this player (or their team) came first; nobody did
    int taken{};   // spots you own
    float total{}; // their scores
    std::string trick_spot; // the spot the trick going on counts for (empty: none)
    std::vector<OverlayToast> toasts;
    std::uintptr_t base{};
    bool skater{};                     // the skater's root (about 1 m above their feet), when there is one:
    std::array<float, 3> skater_at{};  // what of the outlines lies behind them is cut out round them
};
OverlayView overlay_view();
// Where the camera is (render thread, each frame the outlines are drawn): what it can see of them
// is found from there.
void note_camera(const std::array<float, 3> &position);
}
