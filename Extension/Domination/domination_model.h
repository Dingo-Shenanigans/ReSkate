#pragma once
#include "Engine/Core/Json/json.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <string>
#include <vector>

// Domination's rules, apart from the game (README.md):
// - a park: spots, each a footprint seen from above (a polygon, or a capsule around a rail) and
//   a height range, in game coordinates (y up);
// - a trick's spot: the spot the skater spent the most time in while the trick went on; else the
//   spot it landed on; else the spot the skater left less than a second before it began (a trick
//   off a ledge or a mound into the open still counts for it);
// - when a trick has landed and what it was worth, read from the score HUD's values;
// - the board: the best single trick on a spot owns it, the moment it lands; a higher one steals
//   it;
// - a round's results: owners, time held, steals, the best trick; teams.
namespace dingosdk::domination {
using Vec2 = std::array<float, 2>; // x, z
using Vec3 = std::array<float, 3>; // x, y, z

struct Spot {
    std::string id, name, kind;
    bool capsule{};
    std::vector<Vec2> points; // polygon
    Vec2 a{}, b{};            // capsule
    float radius{};
    float y_min{}, y_max{};
    Vec3 marker{};
};
// Where a round puts a skater: a position on the ground and the way it faces (degrees about
// world up, 0 facing +z, 90 facing +x).
struct Start {
    Vec3 position{};
    float heading{};
};
struct Park {
    std::string level; // lower case
    std::string name;
    // The map it is on as players know it ("San Vansterdam", "Skate 3 Improved"), and, for a map
    // mod, its Thunderstore package ("Owner-Name"), to tell a player without it what to get.
    // Both may be empty (the game's own map; the level's name is shown).
    std::string map, package;
    std::vector<Spot> spots;
    std::vector<Start> starts; // may be empty: the round then leaves the skaters where they are
};
// The start for the n-th of `players` (0-based): spread over the starts in the file's order.
int start_for(const Park &park, int index);

// A park file (schema 1). Throws std::runtime_error saying what is wrong, overlapping spots
// included (checked on a 0.25 m grid, off the grid lines).
Park parse_park(const Json &json);
bool contains(const Spot &spot, const Vec3 &position);
// The first spot holding the position; -1 none.
int spot_at(const Park &park, const Vec3 &position);
// The ground heights round a spot's edge (a closed loop), evened out: a point or two measured
// more than `jump` above both sides (the top of a wall or a rail) or below them (over a drop)
// takes the height of the nearer side, so an edge along a bowl's lip stays on the deck.
void even_out(std::vector<float> &heights, float jump = 1.0f);
// Heights not found (NaN: the ray started inside a wall or a block) from the found ones on either
// side round the loop, in a straight line between them; all of them `fallback` if none was found.
void fill_in(std::vector<float> &heights, float fallback);

// Time per spot over one trick.
class LineSpots {
public:
    void reset(std::size_t spots);
    void add(int spot, double seconds);
    // The spot with the most time, if it had at least `minimum` seconds; -1 none.
    int winner(double minimum) const;
    double seconds(int spot) const;
    double total() const { return total_; }
private:
    std::vector<double> time_;
    double total_{};
};

// The score HUD's values each tick (ScoringHUDViewModel), whether the skater is in a wipeout
// (physics states 300-399), and the spot the skater is on now (-1 none).
// Measured in play (a recording of the score HUD's values in a play session, 2026-10-14): ActionActive
// is on while a trick goes on; LineScore rises one tick after it goes off. LineScore looks like
// the line's points so far times the Multiplier (a rise in the multiplier also raises earlier
// tricks' share), so a trick's points are taken from the unscaled rise, at the new multiplier
// (not yet checked against the game's own trick list).
struct TrickSignals {
    bool action{};
    float line_score{}, multiplier{};
    bool slam{}, wipeout{};
    int spot = -1;
    double dt{};
};
struct TrickEvent {
    enum class Kind { none, landed, broken } kind{Kind::none};
    int spot = -1;          // landed: the trick's spot (-1 none)
    float points{};         // landed: the trick's points at the multiplier it raised the line to
};
// A trick that began within this long after leaving a spot counts for it when no other spot
// claims it (seconds).
inline constexpr double takeoff_window = 1.0;
class TrickWatch {
public:
    explicit TrickWatch(std::size_t spots = 0, int wait_ticks = 20) : spots_(spots), wait_ticks_(wait_ticks) { trick_.reset(spots); }
    TrickEvent update(const TrickSignals &signals);
    bool in_trick() const { return action_; }
    // The spot the trick going on counts for so far (-1 none).
    int trick_spot() const;
private:
    std::size_t spots_;
    int wait_ticks_;
    bool seen_{}, action_{};
    int waiting_{};      // ticks left to see the score rise after a trick
    int landing_spot_ = -1;
    int recent_spot_ = -1;      // the last spot the skater was on, and how long ago (s)
    double since_spot_ = 1e9;
    int takeoff_spot_ = -1;     // the trick going on's: recent_spot_ when it began, if just left
    LineSpots trick_;
    float score_{}, multiplier_{}; // the line's score and multiplier before the trick
};

// ---- the board: who owns each spot --------------------------------------------------------
// Every player's best claim per spot. A spot is owned by the best claim on it: the higher score,
// then the earlier claim (ms since the round started, as its claimant timed it), then the lower
// player id, so every copy of the board that has seen the same claims agrees, whatever order they
// came in. An equal score never steals. The players are Steam IDs.
using Player = std::uint64_t;
struct Claim {
    float score{};
    std::uint32_t ms{};
};
struct ClaimOutcome {
    int spot = -1;
    Player player{};
    float score{};
    bool best{};              // it raised the player's own best on the spot
    bool kept{};              // it went into the spot's history (best, or once the best: the replay changed)
    Player before{}, after{}; // the spot's owner before and after it (0 none)
    float before_score{};     // the owner's score before it
    bool taken() const { return after == player && before != player; }
    bool improved() const { return best && before == player; }
};
// One claim as it happened: who, the score, and when (ms since the round started, as its claimant
// timed it). Each player's bests are kept in the order they were made (each higher than every
// earlier one), whatever order they arrived in, so that every copy of the board with the same
// claims holds the same history and replays the round the same way (who owned what when: hold
// times, steals).
struct Event {
    Player player{};
    float score{};
    std::uint32_t ms{};
    auto operator<=>(const Event &) const = default;
};
class Board {
public:
    Board() = default;
    explicit Board(std::size_t spots) : claims_(spots), history_(spots) {}
    std::size_t spots() const { return claims_.size(); }
    // A claim (a landed trick's score); only a player's best on a spot counts.
    ClaimOutcome claim(int spot, Player who, float score, std::uint32_t ms);
    Player owner(int spot) const; // 0 none
    float top(int spot) const;    // the owner's score
    float best(int spot, Player who) const;
    struct Standing {
        Player player{};
        int spots{};
        float total{}; // of the spots owned
    };
    // Every player with a claim: most spots first, then the higher total, then the lower id.
    std::vector<Standing> standings() const;
    int owned(Player who) const;
    float owned_total(Player who) const;
    // Every claim kept on a spot, in time order (ms, then player).
    std::vector<Event> history(int spot) const;
    std::size_t history_size(int spot) const;
private:
    std::vector<std::map<Player, Claim>> claims_;
    std::vector<std::set<std::tuple<std::uint32_t, Player, float>>> history_; // (ms, player, score)
};

// ---- what a round adds up to ------------------------------------------------------------
// Teams (an option): each player's team, 0 or 1; a spot is its owner's team's. Empty: free-for-all.
using Teams = std::map<Player, int>;
struct RoundStats {
    std::map<Player, double> hold;                   // seconds owning spots, all spots together
    std::vector<std::pair<Player, double>> spot_hold; // per spot: who owned it longest, and how long
    std::map<Player, int> steals;                    // spots taken from someone (in teams: from the other team)
    Event best;                                       // the round's best claim
    int best_spot = -1;
};
// The round replayed from the board's history up to `end_ms`: identical on every copy that has the
// same claims.
RoundStats round_stats(const Board &board, std::uint32_t end_ms, const Teams &teams = {});
struct Ranked {
    Player player{}; // free-for-all: the player; teams: the team (0 or 1)
    int spots{};
    float total{};
    double hold{};
    int steals{};
};
// The standings: the most spots at the end first (skate. 3's way; ties: the higher total, then the
// lower id). With teams, one row per team that has players.
std::vector<Ranked> ranked(const Board &board, const RoundStats &stats, const Teams &teams = {});
// Teams for a round's players: the two teams as even as can be, the players in id order taking
// turns (0, 1, 0, ...) after the ones already placed. Everyone placed keeps their team.
Teams balance_teams(const Teams &placed, const std::vector<Player> &players);
}
