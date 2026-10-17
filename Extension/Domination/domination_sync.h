#pragma once
#include "domination_model.h"
#include "domination_wire.h"
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// Domination's rounds between players, apart from the game: who runs them, joining, a round's
// clock, teams, the board every player keeps, and the results. One Node per player. The game
// gives it the session's players, the player's own tricks and the messages that arrive, each
// with the time by the player's own clock, and sends what it puts out (domination_wire.h). Every
// rule that makes the players' games agree is here, so that it can be tested with many players
// and a bad network (Test/domination_tests.cpp).
//
// - Who runs rounds (the controller): the session's host, when the host is a player in it; else
//   (a dedicated server, or a host who left) the player with the lowest Steam ID. Only the
//   controller starts a round for everyone and stops it. Outside one, anyone plays alone.
// - Joining: every player of the session on the map is taken into a round at once, late too, and
//   out of a round of their own. A player who leaves a round is not taken back into it (not by a
//   round of their own either); `rejoin` puts them back. Nobody is taken into a round that already
//   ended for them (a player who missed its stop may still be saying it runs). Leaving says so
//   (its own message, never taken for a stop): the others stop counting them in the round (its
//   authority, its teams).
// - Stopping: the controller (or the round's starter) stops a round for everyone at a moment.
//   Every copy that ends by a stop says it again with its boards after the end, so a stop missed or
//   lost on the way (or sent once by a controller who then changed maps) still reaches everyone.
// - Time: ms since the round started, by each player's own clock. Every clock follows the
//   round's authority (the controller while it is in the round, else the lowest id in it): a gap
//   above 300 ms is halved at each of its round messages.
// - The board: every claim that was ever a player's best on a spot. The same claims make the same
//   owners and the same replay on every copy (domination_model.h). A new best goes out at once,
//   the whole board every 5 s, and once the round is over (with the stop, when it was stopped,
//   and the teams) every 2 s for 10 s, then every 10 s for 3 minutes or until the next round, so a
//   player whose connection dropped across the end still hears it all when it comes back. Claims
//   made before the end, and teams, are taken whenever they come while the round is the current
//   one. A stop that arrives late, even after a copy's own end, moves that copy's end back to the
//   stop's moment, so a missed or late stop never leaves copies apart.
// - Teams (an option): handed out by the authority, as even as they can be; nobody is moved once
//   placed. A player who joins late takes the teams they are told before handing any out.
// - Two rounds started at once (two controllers, for a moment): the lower round number stands,
//   and the other's players move to it while nothing has been claimed in theirs.
namespace dingosdk::domination::sync {
using Ms = std::int64_t; // a player's own clock, ms

inline constexpr Ms countdown_ms = 4000; // 3, 2, 1, GO before the play; tricks in it count for nothing
inline constexpr Ms resend_ms = 5000, gone_ms = 25000, teams_patience_ms = 12000, clock_slack_ms = 300;
inline constexpr Ms after_resend_ms = 2000, after_window_ms = 10000, after_slow_ms = 10000, after_all_ms = 180000;
inline constexpr Ms claim_ahead_ms = 5000;
inline constexpr std::size_t max_players = 64, max_events_per_spot = 1024;
// The player on a solo round's board when the game has no Steam ID (offline).
inline constexpr Player solo_self = 2;

struct Options {
    bool teams{}; // red against blue (with other players)
    std::uint8_t bits() const;
    static Options from(std::uint8_t bits);
    bool operator==(const Options &) const = default;
};

// The session as the game sees it.
struct Roster {
    Player self{}; // 0: not in a session
    Player host{}; // the session's host when it is a player (0: none, e.g. a dedicated server)
    std::vector<Player> others; // the other connected players
    bool session() const { return self && !others.empty(); }
    bool operator==(const Roster &) const = default;
};
Player controller(const Roster &roster);

struct Results {
    RoundStats stats;
    std::vector<Ranked> rows; // most spots first (ranked); with teams, one row per team
};

// One round as this player has it.
struct Match {
    std::uint32_t id{};
    Player self{}, starter{};
    Options options;
    std::uint16_t duration_s{}; // to play, the countdown not counted
    bool multiplayer{};
    Ms origin{};                // when it started, by this player's clock
    Board board;
    Teams teams;
    bool teams_heard{};
    std::map<Player, Ms> seen;  // participants, when last heard from (this player included)
    std::set<Player> players;   // everyone with a claim
    bool finished{};
    bool left{}; // finished because this player left it (it goes on for the others)
    bool stopper{}; // it ended by a stop (this player's, or one heard): the stop goes out again with the boards after
    Ms finished_at{};
    std::uint32_t ended_ms{};
    Results results;
    Ms joined{}, next_resend{}, next_after{};
    std::size_t board_cursor{};

    std::uint32_t end_ms() const { return static_cast<std::uint32_t>(duration_s) * 1000u + static_cast<std::uint32_t>(countdown_ms); }
    std::uint32_t ms(Ms now) const;
    bool counting_down(Ms now) const { return !finished && ms(now) < countdown_ms; }
    double left_s(Ms now) const; // to play
    bool with_teams() const { return options.teams && multiplayer; }
    int team_of(Player who) const;
    // Whether `a` and `b` play against each other: other players, or in teams the other team (or a
    // player with no team yet).
    bool rivals(Player a, Player b) const;
    // A spot owned by this player, or by their team.
    bool ours(Player owner) const { return owner && (owner == self || (with_teams() && !rivals(owner, self))); }
    // The standings now (or, over, the results).
    Results standings(Ms now) const;
};

struct Event {
    enum class Kind {
        started,      // this player started a round
        joined,       // this player was taken into another player's round (who: its starter)
        moved,        // ... out of a round started at the same time (value: the old round)
        left,         // this player left a round (it stays theirs to look at)
        stopped,      // the round was stopped (who: by whom; this player if theirs)
        finished,     // this player's copy ended, or its end moved back (value: ended_ms; round)
        claim,        // a spot changed hands (claim: what did it; quiet: one of many at once)
        teams,        // this player's team was set (value: 0 red, 1 blue)
        version,      // a player sends another version of Domination's messages (value: theirs)
        park,         // a player plays another version of this park
        clock,        // this player's clock followed the authority's (value: ms moved)
    } kind{};
    Player who{};
    ClaimOutcome claim;
    bool quiet{};
    std::int64_t value{};
    std::uint32_t round{}; // the round it is about
};

enum class StartResult { started, running, not_controller, no_spots };

class Node {
public:
    Node(std::uint32_t park, std::size_t spots) : park_(park), spots_(spots) {}
    void set_roster(const Roster &roster) { roster_ = roster; }
    const Roster &roster() const { return roster_; }
    Player controller() const { return sync::controller(roster_); }
    // In a session, whether this player runs its rounds.
    bool controls() const { return roster_.self && controller() == roster_.self; }
    // A round with everyone in the session (the controller only), or alone (anyone, outside a
    // round of the session's); `id` a fresh round number (not 0). Every round starts with every
    // spot open.
    StartResult start(Ms now, Options options, std::uint16_t duration_s, bool solo, std::uint32_t id);
    // The controller (or the round's starter) ends the round for everyone; anyone else leaves it.
    // False: no round running.
    bool stop(Ms now);
    // This player's trick: claimed at once, and sent when it raises their best. Empty: no round to
    // claim in (or it counts down).
    struct Claimed {
        ClaimOutcome outcome;
        float score{};
        std::uint32_t ms{}; // when, by the round's clock (a stop before it drops it)
    };
    std::optional<Claimed> claim(int spot, float score, Ms now);
    // A Domination message from another player (domination_wire.h).
    void receive(Player from, std::span<const std::uint8_t> bytes, Ms now);
    // Resends, the authority's duties and the end; call often.
    void tick(Ms now);
    std::vector<std::vector<std::uint8_t>> take_outgoing();
    std::vector<Event> take_events();
    const std::optional<Match> &match() const { return match_; }
    bool running() const { return match_ && !match_->finished; }
    // A player who left the session's round is taken into it again with its next message (their
    // copy of it reopens and catches up from the others' boards).
    void rejoin() { left_round_ = 0; }
    // Who keeps the round's clock and hands out its teams.
    Player authority(Ms now) const;
    std::uint32_t park() const { return park_; }
    std::size_t spots() const { return spots_; }

private:
    void send_round(bool running, std::uint32_t elapsed, bool left = false);
    void send_board();
    void send_teams();
    // Ends this player's copy at `ended_ms` (a stop earlier than an end already had moves it back).
    // `after`: the boards (and a stop, the teams) go out again for a while, for whoever missed them.
    void finish(Ms now, std::uint32_t ended_ms, bool after = true);
    void recompute();
    void merge(Player from, const wire::BoardMessage &board, Ms now);
    std::optional<ClaimOutcome> take(int spot, Player who, float score, std::uint32_t at);
    void say(Event event) { if (events_.size() < 256) events_.push_back(std::move(event)); }
    std::uint32_t park_;
    std::size_t spots_;
    Roster roster_;
    std::optional<Match> match_;
    std::uint32_t left_round_{}; // a round this player left: not joined again
    // The last rounds that ended here: never joined again (a player who missed one's stop may still
    // be saying it runs, just as the next round starts).
    std::deque<std::uint32_t> over_;
    std::set<Player> told_version_, told_park_;
    std::vector<std::vector<std::uint8_t>> outgoing_;
    std::vector<Event> events_;
};
} // namespace dingosdk::domination::sync
