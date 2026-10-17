#include "domination.h"
#include "domination_internal.h"
#include "domination_model.h"
#include "domination_sync.h"
#include "domination_wire.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/World/level_loading.h"
#include "Extension/UI/NativeMenu/ui_sound.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <set>

namespace dingosdk::domination {
using namespace detail;
namespace detail {
State &state() {
    static auto *value = new State;
    return *value;
}
} // namespace detail
namespace {
// ---- the round's ticks ------------------------------------------------------------------
const sync::Match *match_of(const State &s) { return s.node && s.node->match() ? &*s.node->match() : nullptr; }
bool round_on(const State &s) { return s.node && s.node->running(); }
const char *team_name(int team) { return team == 0 ? "Red" : team == 1 ? "Blue" : "no team"; }
void play(multiplayer::UiSound sound) { multiplayer::queue_ui_sound(static_cast<std::uint32_t>(sound)); }

std::string name_of(const State &s, Player who) {
    if (!who) return "nobody";
    const auto *m = match_of(s);
    if ((m && who == m->self) || (s.node && who == s.node->roster().self)) return "you";
    if (const auto it = s.names.find(who); it != s.names.end() && !it->second.empty()) return it->second;
    return std::format("player {:x}", who & 0xffff);
}
// A results row's name: the player, or in teams the team.
std::string row_name(const State &s, const sync::Match &m, Player row) {
    return m.with_teams() ? std::string(team_name(static_cast<int>(row))) : name_of(s, row);
}

// The session as Domination sees it, once a second (and at once when asked): its players, who
// hosts it, and their names.
// The session's players and host as the rules see them (domination_sync.h).
sync::Roster roster_of(const MultiplayerModel &m) {
    sync::Roster r;
    if (m.active && m.connected && m.local_id && !m.echo) {
        r.self = m.local_id;
        r.host = m.dedicated ? 0 : m.host_id; // a dedicated server is no player
        for (const auto &p : m.roster)
            if (p.connected && p.id && p.id != m.local_id) r.others.push_back(p.id);
        std::ranges::sort(r.others);
    }
    return r;
}
void refresh_roster(State &s, Clock::time_point now, bool force = false) {
    if (!force && now < s.next_roster) return;
    s.next_roster = now + std::chrono::seconds(1);
    const auto m = multiplayer::model();
    const auto r = roster_of(m);
    for (const auto &p : m.roster)
        if (p.id && !p.name.empty()) s.names[p.id] = p.name;
    if (m.local_id && !m.local_name.empty()) s.names[m.local_id] = m.local_name;
    if (s.node && !(s.node->roster() == r)) s.node->set_roster(r);
}

// This player's start: in a session, their place among everyone's ids (so no two players share a
// start while there are enough); alone, any.
int start_index(const State &s, const sync::Match &m) {
    if (!m.multiplayer) return static_cast<int>(Clock::now().time_since_epoch().count() % 1000);
    std::vector<Player> ids{m.self};
    for (const auto who : s.node->roster().others) ids.push_back(who);
    std::ranges::sort(ids);
    return static_cast<int>(std::ranges::find(ids, m.self) - ids.begin());
}
// Puts the skater at their start, facing the park (client thread).
void place(State &s, Play &p) {
    p.place = false;
    const int i = start_for(*s.park, p.start);
    if (i < 0) return;
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        toast(s, "The host has turned off teleporting: skate to the park.", 0);
        say(logging::Level::info, "start not used: the session does not allow teleporting");
        return;
    }
    const auto &start = s.park->starts[i];
    float y = start.position[1];
    if (const auto ground = local_ground_height(start.position[0], start.position[2], y + 6, y - 6)) y = *ground;
    constexpr float stand_height = 0.5f, degrees = 3.14159265f / 180;
    const bool sent = teleport_local_skater_heading({start.position[0], y + stand_height, start.position[2]}, start.heading * degrees);
    say(logging::Level::info, std::format("start {} of {} at ({:.1f}, {:.2f}, {:.1f}) facing {:.0f}: {}", i + 1, s.park->starts.size(),
                                          start.position[0], y, start.position[2], start.heading, sent ? "teleporting" : "teleport unavailable"));
}

// A round's end, once: the results up.
// This player's round counts (Stats), read from their profile the first time they are needed.
int saved_count(const std::string &key) {
    const auto value = profile_runtime::local_value(key);
    if (!value || !value->is_number()) return 0;
    const double n = value->get<double>();
    return std::isfinite(n) && n > 0 && n < 1e9 ? static_cast<int>(n) : 0;
}
std::string park_key(std::string_view id) { return "Domination.Park." + std::string(id); }
bool load_stats(State &s) {
    if (s.stats.loaded) return true;
    if (!profile_runtime::local_profile_store_ready()) return false;
    s.stats.rounds = saved_count("Domination.Rounds");
    s.stats.online = saved_count("Domination.Online");
    s.stats.won = saved_count("Domination.Won");
    s.stats.loaded = true;
    return true;
}
int park_rounds(State &s, std::string_view id) {
    if (!load_stats(s)) return 0;
    auto it = s.stats.parks.find(id);
    if (it == s.stats.parks.end()) it = s.stats.parks.emplace(std::string(id), saved_count(park_key(id))).first;
    return it->second;
}
// A round played to the end (or stopped once tricks counted; not one this player left) counts once:
// in all, for its park, and with others (won: first, no tie, alone or as a team).
void count_round(State &s, const sync::Match &m) {
    if (s.play->counted || m.left || m.ended_ms <= sync::countdown_ms || s.park_id.empty() || !load_stats(s)) return;
    s.play->counted = true;
    const int here = park_rounds(s, s.park_id) + 1;
    s.stats.parks[s.park_id] = here;
    ++s.stats.rounds;
    std::vector<std::pair<std::string, Json>> values{{"Domination.Rounds", std::int64_t{s.stats.rounds}}, {park_key(s.park_id), std::int64_t{here}}};
    if (m.multiplayer) {
        ++s.stats.online;
        values.emplace_back("Domination.Online", std::int64_t{s.stats.online});
        const auto &rows = m.results.rows;
        const bool tie = rows.size() > 1 && rows[1].spots == rows[0].spots && rows[1].total == rows[0].total;
        if (!rows.empty() && !tie && (m.with_teams() ? static_cast<int>(rows[0].player) == m.team_of(m.self) : rows[0].player == m.self))
            values.emplace_back("Domination.Won", std::int64_t{++s.stats.won});
    }
    profile_runtime::set_local_values(values); // (saved on the profile's own thread)
}

void end_of_round(State &s, const sync::Match &m, Clock::time_point now, const char *why) {
    s.play->done = true;
    count_round(s, m);
    s.results_until = now + std::chrono::seconds(static_cast<int>(results_shown));
    std::string table;
    for (const auto &r : m.results.rows)
        table += std::format(" {} {} spots, {:.0f} s held, {} steals ({});", row_name(s, m, r.player), r.spots, r.hold, r.steals, points(r.total));
    say(logging::Level::info, std::format("round {} ({}{}): you own {} of {} spots, {} points on them. Standings:{}", why,
                                          m.multiplayer ? "multiplayer" : "solo", m.with_teams() ? ", teams" : "", m.board.owned(m.self),
                                          s.park->spots.size(), points(m.board.owned_total(m.self)), table));
}

// Keeps the game's side in step with the node's round: a new round gets its own Play (and the
// skater put at a start); a round that ended gets its results shown.
void follow(State &s, Clock::time_point now) {
    const auto *m = match_of(s);
    if (!m || !s.park) return;
    if (!s.play || s.play->round != m->id) {
        Play p;
        p.round = m->id;
        p.watch = TrickWatch(s.park->spots.size());
        p.flashed.assign(s.park->spots.size(), {});
        p.start = start_index(s, *m);
        p.place = !m->finished && !s.park->starts.empty();
        s.play = std::move(p);
        s.results_until = {};
    }
    if (!m->finished && s.play->done) { // a round left and rejoined: its end is to come again
        s.play->done = false;
        s.results_until = {};
    }
    if (m->finished && !s.play->done) end_of_round(s, *m, now, m->left ? "left" : m->ended_ms < m->end_ms() ? "stopped" : "over");
}

// What another player's claim did, said to the local player.
void tell(State &s, const ClaimOutcome &out) {
    const auto &park = *s.park;
    const auto &m = *match_of(s);
    const auto &spot = park.spots[out.spot].name;
    if (out.before == m.self)
        toast(s, std::format("{} STOLE {} FROM YOU   {}", name_of(s, out.player), spot, points(out.score)), 3, out.player);
    else if (m.ours(out.before) && !m.ours(out.player))
        toast(s, std::format("{} STOLE {} FROM {}   {}", name_of(s, out.player), spot, name_of(s, out.before), points(out.score)), 3, out.player);
    else
        toast(s, std::format("{} TOOK {}   {}", name_of(s, out.player), spot, points(out.score)), 0, out.player);
}

// The node's news, turned into what the player sees and hears.
void hear(State &s, Clock::time_point now) {
    if (!s.node) return;
    int quiet = 0;
    for (const auto &e : s.node->take_events()) {
        const auto *m = match_of(s);
        switch (e.kind) {
        case sync::Event::Kind::started: break; // launch() says it
        case sync::Event::Kind::joined:
            toast(s, std::format("{}'s round: take the most spots!", name_of(s, e.who)), 0);
            say(logging::Level::info, std::format("joined {}'s multiplayer round {:08x}", name_of(s, e.who), m ? m->id : 0));
            break;
        case sync::Event::Kind::moved:
            toast(s, std::format("Two rounds started at once: moved to {}'s", name_of(s, e.who)), 0);
            say(logging::Level::info, std::format("round {:08x} gave way to {:08x}", static_cast<std::uint32_t>(e.value), m ? m->id : 0));
            break;
        case sync::Event::Kind::left: toast(s, "You left the round (domination join to get back in)", 0); break;
        case sync::Event::Kind::stopped:
            if (m && e.who != m->self) toast(s, name_of(s, e.who) + " stopped the round", 0);
            break;
        case sync::Event::Kind::finished: break; // follow() shows the results
        case sync::Event::Kind::claim: {
            if (!m || !s.park) break;
            follow(s, now);
            const auto &out = e.claim;
            if (s.play && out.spot >= 0 && static_cast<std::size_t>(out.spot) < s.play->flashed.size()) s.play->flashed[out.spot] = now;
            const bool gained = m->ours(out.after), lost = m->ours(out.before);
            if (gained && !lost) play(multiplayer::UiSound::select);
            else if (lost && !gained) play(multiplayer::UiSound::cancel);
            if (out.player == m->self) break; // landed() says it
            if (e.quiet) ++quiet;
            else tell(s, out);
            say(logging::Level::info, std::format("{} claims {} with {}: owner {} -> {}", name_of(s, out.player), s.park->spots[out.spot].id,
                                                  points(out.score), name_of(s, out.before), name_of(s, out.after)));
            break;
        }
        case sync::Event::Kind::teams:
            toast(s, std::format("You're on team {}", team_name(static_cast<int>(e.value))), 1);
            say(logging::Level::info, std::format("on team {}", team_name(static_cast<int>(e.value))));
            break;
        case sync::Event::Kind::version:
            toast(s, std::format("{} has another version of Domination: update ReSkate to play together", name_of(s, e.who)), 2);
            say(logging::Level::info, std::format("{} sends Domination messages of version {} (this build: {})", name_of(s, e.who), e.value, wire::version));
            break;
        case sync::Event::Kind::park:
            toast(s, std::format("{}'s Domination park differs from yours: you cannot play together", name_of(s, e.who)), 2);
            say(logging::Level::info, std::format("{} plays another version of this park", name_of(s, e.who)));
            break;
        case sync::Event::Kind::clock:
            say(logging::Level::info, std::format("clock moved {} ms to follow {}", e.value, name_of(s, e.who)));
            break;
        }
    }
    if (quiet) toast(s, std::format("Caught up: {} spots changed hands", quiet), 0);
}

// A landed trick: claimed at once (the best single trick on a spot owns it).
void landed(State &s, const TrickEvent &e, Clock::time_point now) {
    const auto &park = *s.park;
    if (e.spot < 0) {
        say(logging::Level::info, std::format("trick +{} on no spot", points(e.points)));
        toast(s, std::format("+{}  no spot", points(e.points)), 0);
        return;
    }
    const auto &m = *match_of(s);
    const auto claimed = s.node->claim(e.spot, e.points, ms_of(now));
    if (!claimed) return;
    const auto &out = claimed->outcome;
    const auto &name = park.spots[e.spot].name;
    const auto gain = "+" + points(e.points);
    std::string verdict;
    if (out.taken()) {
        verdict = out.before ? std::format("TAKEN from {} ({})", name_of(s, out.before), points(out.before_score)) : "TAKEN";
        toast(s, out.before ? std::format("YOU STOLE {} FROM {}   {}", name, name_of(s, out.before), gain) : std::format("YOU TOOK {}   {}", name, gain), 1);
    } else if (out.improved()) {
        verdict = std::format("new best (was {})", points(out.before_score));
        toast(s, std::format("NEW BEST ON {}   {}", name, gain), 1);
    } else {
        verdict = std::format("short of {} ({})", name_of(s, out.before), points(out.before_score));
        toast(s, std::format("{} ON {}: {} NEEDS {}", gain, name, name_of(s, out.before), points(out.before_score)), 2);
    }
    say(logging::Level::info, std::format("trick +{} on {}: {}. You own {} of {}.", points(e.points), park.spots[e.spot].id, verdict,
                                          m.board.owned(m.self), park.spots.size()));
}

void tick_round(State &s, const trainer::Telemetry &t, Clock::time_point now, double dt) {
    if (!round_on(s) || !s.park || !s.play) return;
    if (!s.hud.model.handle) {
        // Nothing to read tricks from yet. Long after the start, this game build's score HUD is not
        // what Domination knows (domination_hud.cpp): said once, or tricks would silently count for nothing.
        if (!s.hud.blind_told && match_of(s)->ms(ms_of(now)) > sync::countdown_ms + 4000) {
            s.hud.blind_told = true;
            toast(s, "Domination can't read tricks from the score HUD in this game build: tricks won't count", 3);
            say(logging::Level::warning, "the score HUD's view model was not found: tricks can't be read");
        }
        return;
    }
    const auto &f = s.hud.fields;
    TrickSignals in;
    in.action = flag_of(f[action_active]);
    in.line_score = score_of(f[line_score]);
    in.multiplier = score_of(f[multiplier]);
    in.slam = flag_of(f[is_slam]);
    in.wipeout = t.skater && t.physics_state >= 300 && t.physics_state < 400;
    in.spot = s.here;
    in.dt = dt;
    const auto event = s.play->watch.update(in);
    if (match_of(s)->counting_down(ms_of(now))) return; // the trick watch keeps up, but nothing counts yet
    switch (event.kind) {
    case TrickEvent::Kind::landed: landed(s, event, now); break;
    case TrickEvent::Kind::broken:
        say(logging::Level::info, "slam: the trick counts for nothing");
        toast(s, "Slam: the trick counts for nothing", 3);
        break;
    case TrickEvent::Kind::none: break;
    }
}

// The node's turn: its resends and duties, the messages it puts out, and its news.
void tick_node(State &s, Clock::time_point now) {
    if (!s.node) return;
    refresh_roster(s, now);
    s.node->tick(ms_of(now));
    for (auto &bytes : s.node->take_outgoing())
        if (s.outgoing.size() < 64) s.outgoing.push_back(std::move(bytes));
    hear(s, now); // (first: a round's end before a next round replaces it)
    follow(s, now);
}

// Leaving this park (another level): a round going on ends first, on the park it was played on.
// The player leaves it (or, running it, ends it for everyone).
void leave_park(State &s, Clock::time_point now) {
    if (round_on(s)) {
        s.node->stop(ms_of(now));
        for (auto &bytes : s.node->take_outgoing())
            if (s.outgoing.size() < 64) s.outgoing.push_back(std::move(bytes));
        hear(s, now);
        follow(s, now);
    }
    if (s.play) s.play->done = true;
}
// The node for the park now loaded (none without a park).
void new_node(State &s, Clock::time_point now) {
    s.play.reset();
    s.node.reset();
    s.park_hash = s.park ? park_hash(*s.park) : 0;
    if (s.park) {
        s.node.emplace(s.park_hash, s.park->spots.size());
        refresh_roster(s, now, true);
    }
}
// Another park of this map becomes the loaded one (between rounds).
void switch_park(State &s, const KnownPark &k, Clock::time_point now) {
    if (s.park_id == k.id || k.park.level != s.level || round_on(s)) return;
    use_park(s, k);
    new_node(s, now);
}
std::string missing_map_text(const Park &park, MapState state) {
    if (state == MapState::off) return map_name(park) + " is installed but switched off: switch it on in the launcher's Mods.";
    return map_name(park) + " isn't installed" + (park.package.empty() ? std::string(".") : ": get " + park.package + " from Thunderstore (the launcher's Mods).");
}
} // namespace

// Starts a round on this map's park, with the session's players or alone (state locked).
std::string launch(State &s, double duration, bool solo);

void start(std::uintptr_t base) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.base = base;
}

void set_level(std::string_view level) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto now = lower(std::string(level));
    if (now == s.level) return;
    try {
        leave_park(s, Clock::now()); // (before the new park replaces it)
    } catch (...) {
    }
    s.level = now;
    try {
        load_park(s);
    } catch (...) {
        s.park.reset();
    }
    try {
        new_node(s, Clock::now());
    } catch (...) {
    }
}

void on_client_tick() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    try {
        tick_scan(s);
    } catch (...) {
        s.scan.reset();
        s.last_scan = "error: the scan stopped after a native error";
        say(logging::Level::warning, "scan stopped after a native error.");
    }
    const auto now = Clock::now();
    const double dt = s.last_tick == Clock::time_point{} ? 0 : std::chrono::duration<double>(now - s.last_tick).count();
    s.last_tick = now;
    if (s.teleport && trainer::telemetry().skater) {
        auto [position, heading] = *s.teleport;
        s.teleport.reset();
        // `ground`: standing 0.5 m over the highest ground below 200 m (a start stands them so). Collision
        // loads only around the skater, so far away there is none to stand on yet.
        const auto ground = std::isnan(position[1]) ? local_ground_height(position[0], position[2], 200, -100) : std::optional<float>(position[1] - 0.5f);
        if (!ground)
            say(logging::Level::warning, std::format("teleport to ({:.1f}, ground, {:.1f}) not done: no ground found there yet (it loads near the skater); give a height",
                                                     position[0], position[2]));
        else {
            position[1] = *ground + 0.5f;
            say(logging::Level::info, std::format("teleport to ({:.1f}, {:.2f}, {:.1f}) facing {:.0f}: {}", position[0], position[1], position[2], heading,
                                                  face(position, heading) ? "done" : "unavailable"));
        }
    }
    try {
        tick_survey(s, now);
    } catch (...) {
        s.survey.reset();
        say(logging::Level::warning, "survey stopped after a native error.");
    }
    if (s.pending) {
        auto &p = *s.pending;
        const auto t = trainer::telemetry();
        if (now - p.asked > pending_patience) {
            say(logging::Level::info, "the round asked for on " + p.level + " gave up waiting for that map");
            s.pending.reset();
        } else if (s.level == p.level && s.park && t.skater && level_content_active()) { // in game, not loading
            if (p.skater_since == Clock::time_point{}) p.skater_since = now;
            if (now - p.skater_since >= (p.solo || !multiplayer_session_active() ? settle_after_load : settle_in_session)) {
                const auto pending = *s.pending;
                s.pending.reset();
                const auto reply = launch(s, pending.duration, pending.solo);
                if (reply.starts_with("error")) say(logging::Level::info, "the round asked for could not start: " + reply); // (launch says the rest)
            }
        } else p.skater_since = {};
    }
    try {
        tick_node(s, now);
    } catch (const std::exception &error) {
        say(logging::Level::warning, std::string("round tick failed: ") + error.what());
    } catch (...) {
    }
    if (round_on(s) && s.play && s.play->place && s.park && trainer::telemetry().skater) place(s, *s.play);
    if (round_on(s) && s.play) {
        // The countdown heard: a tick at each number, and GO.
        const int beat = countdown_beat(match_of(s)->ms(ms_of(now)));
        if (beat >= 0 && beat != s.play->beat) play(beat ? multiplayer::UiSound::navigate : multiplayer::UiSound::select);
        s.play->beat = beat;
    }
    if (s.outline_left && s.park && level_content_active()) {
        try {
            if (const auto t = trainer::telemetry(); t.skater) tick_outlines(s, {t.position[0], t.position[1], t.position[2]});
        } catch (...) {
            s.outline_left = 0;
            say(logging::Level::warning, "spot outlines stopped after a native error.");
        }
    }
    const bool on = round_on(s);
    if (on && s.park)
        try {
            if (const auto t = trainer::telemetry(); t.skater) tick_sight(s, {t.position[0], t.position[1], t.position[2]});
        } catch (...) {
        }
    if (!on && !(s.park && now < s.results_until)) return;
    try {
        const auto t = trainer::telemetry();
        s.skater = t.skater;
        s.position = {t.position[0], t.position[1], t.position[2]};
        // The results go once the skater rides off (after a look of 3 s at least).
        if (s.park && now < s.results_until && t.skater && t.speed > 2.5f &&
            now > s.results_until - std::chrono::seconds(static_cast<int>(results_shown)) + std::chrono::seconds(3))
            s.results_until = now;
        s.here = s.park && t.skater ? spot_at(*s.park, s.position) : -1;
        if (on) read_hud(s);
        tick_round(s, t, now, dt);
    } catch (const std::exception &error) {
        say(logging::Level::warning, std::string("round tick failed: ") + error.what());
    } catch (...) {
    }
}

void receive(std::uint64_t sender, std::span<const std::uint8_t> bytes) noexcept {
    try {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        // A round on another park of this map (the one whoever runs the rounds chose): that park
        // becomes this player's too, between rounds, so they play it with everyone.
        if (!round_on(s))
            if (const auto message = wire::decode(bytes))
                if (const auto *round = std::get_if<wire::RoundMessage>(&*message); round && round->running && round->park != s.park_hash)
                    for (const auto &k : s.parks)
                        if (k.hash == round->park && k.park.level == s.level) {
                            s.chosen = k.id;
                            switch_park(s, k, Clock::now());
                            break;
                        }
        if (!s.node) return;
        refresh_roster(s, Clock::now());
        s.node->receive(sender, bytes, ms_of(Clock::now()));
    } catch (...) {
    }
}

std::vector<std::vector<std::uint8_t>> take_outgoing() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.outgoing, {});
}

// ---- console ----------------------------------------------------------------------------
std::string round_start(const std::vector<std::string> &words) {
    double duration = default_round;
    bool solo = false;
    {
        // Unless the words say otherwise: the next round's settings.
        auto &s = state();
        std::lock_guard lock(s.mutex);
        duration = s.next_duration;
        solo = s.next_solo;
    }
    for (const auto &word : words) {
        if (word == "solo") solo = true;
        else if (const auto value = number(word)) {
            if (*value < 20 || *value > 3600) return "error: a round lasts 20 to 3600 seconds.";
            duration = *value;
        } else return "usage: domination start [seconds] [solo]";
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.park) {
        load_park(s); // a park file added since the level loaded
        new_node(s, Clock::now());
    }
    if (round_on(s)) return "error: a round is running (domination stop ends it).";
    if (s.parks.empty()) scan_parks(s);
    const auto *chosen = next_park(s);
    if (!chosen) return "error: no Domination park" + (s.park_problem.empty() ? std::string(".") : " (" + s.park_problem + ").");
    if (chosen->park.level == s.level) switch_park(s, *chosen, Clock::now());
    else {
        // A park on another map: the round starts once that map has loaded (the menu loads it).
        check_maps(s, Clock::now());
        if (const auto state = map_state(s, chosen->park); state == MapState::missing || state == MapState::off)
            return "error: " + missing_map_text(chosen->park, state);
        const auto level = chosen->park.level, name = chosen->park.name, map = map_name(chosen->park);
        s.chosen = chosen->id; // the park that map loads with
        if (!solo) {
            // In a session everyone goes where the host (or a server admin who may) loads: whoever
            // runs the rounds and may change the map starts one from anywhere, and the others follow.
            const auto mp = multiplayer::model();
            const auto roster = roster_of(mp);
            if (roster.session() && sync::controller(roster) != roster.self)
                return std::format("error: in this session {} starts the rounds; domination start solo plays one alone.", name_of(s, sync::controller(roster)));
            if (roster.session() && multiplayer_controls_level(mp))
                return "error: this session's map is the host's to change: " + name + " is on " + map + " (" + level + ").";
        }
        s.pending = Pending{level, name, duration, solo, Clock::now(), {}};
        say(logging::Level::info, "round asked for in " + name + " on " + level + ": it starts when that map has loaded");
        return "The round starts when " + map + " has loaded (load " + level + ").";
    }
    s.pending.reset();
    return launch(s, duration, solo);
}

std::string launch(State &s, double duration, bool solo) {
    const auto now = Clock::now();
    refresh_roster(s, now, true);
    const bool together = !solo && s.node->roster().session();
    const sync::Options options{s.next_teams};
    const auto self = s.node->roster().self;
    const auto id = static_cast<std::uint32_t>(now.time_since_epoch().count() ^ (self * 0x9E3779B97F4A7C15ull)) | 1u;
    // Every round starts with every spot open, alone too: only this game's tricks show while it
    // is played.
    switch (s.node->start(ms_of(now), options, static_cast<std::uint16_t>(duration), !together, id)) {
    case sync::StartResult::started: break;
    case sync::StartResult::running: return "error: a round is running (domination stop ends it).";
    case sync::StartResult::not_controller:
        return std::format("error: in this session {} starts the rounds (the host), and everyone on the map plays them. "
                           "Domination start solo plays one alone.", name_of(s, s.node->controller()));
    case sync::StartResult::no_spots: return "error: the park has no spots.";
    }
    tick_node(s, now);
    const auto &m = *match_of(s);
    toast(s, std::format("Domination{}: {} spots, {}:{:02}. {}", together ? " with everyone here" : "", s.park->spots.size(),
                         static_cast<int>(duration) / 60, static_cast<int>(duration) % 60,
                         m.with_teams() ? "Teams are being picked!" : "Take the most spots!"),
          0);
    const auto reply = std::format("{} round started on {}: {} spots, {:.0f} s, best single trick wins a spot{}.", together ? "Multiplayer" : "Solo",
                                   s.park->name, s.park->spots.size(), duration, m.with_teams() ? ", red against blue" : "");
    say(logging::Level::info, reply + (together ? std::format(" Round {:08x}.", id) : std::string()));
    return reply;
}

std::string round_stop() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!round_on(s)) return "No round is running.";
    const auto &m = *match_of(s);
    const bool leaving = m.multiplayer && m.starter != m.self && !s.node->controls();
    s.node->stop(ms_of(Clock::now()));
    tick_node(s, Clock::now());
    return std::format("Round {}: you own {} of {} spots.", leaving ? "left" : "stopped", m.board.owned(m.self), s.park->spots.size());
}

std::string status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    std::string text;
    const auto now = Clock::now();
    if (!s.park) text = s.level.empty() ? "No level." : "No Domination park for " + s.level + (s.park_problem.empty() ? "." : " (" + s.park_problem + ").");
    else {
        text = std::format("Park \"{}\": {} spots.", s.park->name, s.park->spots.size());
        if (s.node && s.node->roster().session())
            text += std::format(" In a session: {} start the rounds.", s.node->controls() ? std::string("you") : name_of(s, s.node->controller()));
        if (round_on(s)) {
            const auto &m = *match_of(s);
            text += std::format(" {} round: {:.0f} s left, you own {} ({} points).", m.multiplayer ? "Multiplayer" : "Solo", m.left_s(ms_of(now)),
                                m.board.owned(m.self), points(m.board.owned_total(m.self)));
            if (m.multiplayer) {
                if (m.with_teams()) text += std::format(" You are on team {}.", team_name(m.team_of(m.self)));
                text += " Standings:";
                for (const auto &r : m.standings(ms_of(now)).rows)
                    text += std::format(" {} {} spots, {:.0f} s held ({});", row_name(s, m, r.player), r.spots, r.hold, points(r.total));
            }
        } else text += " No round running.";
        // Between rounds the tick does not follow the skater: look now.
        const auto t = trainer::telemetry();
        const int here = t.skater ? spot_at(*s.park, {t.position[0], t.position[1], t.position[2]}) : -1;
        text += !t.skater ? " No skater." : here >= 0 ? " You are on " + s.park->spots[here].name + "." : " You are on no spot.";
    }
    if (s.scan) text += std::format(" Scan running: {} of {} points.", s.scan->next, s.scan->heights.size());
    else if (!s.last_scan.empty()) text += " Last scan: " + s.last_scan;
    return text;
}

std::string spots() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.park) return "error: no Domination park for this level.";
    std::string text;
    for (std::size_t i = 0; i < s.park->spots.size(); ++i) {
        const auto &spot = s.park->spots[i];
        text += std::format("{}{} ({})", i ? "; " : "", spot.name, spot.id);
        if (round_on(s)) {
            const auto &board = match_of(s)->board;
            const auto owner = board.owner(static_cast<int>(i));
            text += owner ? std::format(": {} {}", name_of(s, owner), points(board.top(static_cast<int>(i)))) : std::string(": open");
        }
    }
    return text;
}

std::string parks() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    scan_parks(s);
    s.next_map_check = {};
    check_maps(s, Clock::now());
    if (s.parks.empty()) return "No Domination park.";
    const auto *next = next_park(s);
    std::string text;
    for (const auto &k : s.parks) {
        const auto state = map_state(s, k.park);
        text += std::format("{}{}{} ({}): {} spots on {}, {}", text.empty() ? "" : "; ", &k == next ? "* " : "", k.park.name, k.id, k.park.spots.size(),
                            map_name(k.park),
                            k.park.level == s.level      ? "this map"
                            : state == MapState::missing ? "not installed" + (k.park.package.empty() ? std::string() : " (" + k.park.package + ")")
                            : state == MapState::off     ? "installed, switched off"
                                                         : "installed");
    }
    for (const auto &[level, problem] : s.park_problems) text += "; refused: " + problem;
    return text + ". (* the next round's)";
}

std::string stats() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!load_stats(s)) return "Your profile isn't loaded yet.";
    if (s.parks.empty()) scan_parks(s);
    std::string text = std::format("{} Domination round{} played to the end: {} with other players ({} won).", s.stats.rounds,
                                   s.stats.rounds == 1 ? "" : "s", s.stats.online, s.stats.won);
    std::vector<std::pair<std::string, int>> maps; // in the parks' order
    std::string parks;
    for (const auto &k : s.parks) {
        const int n = park_rounds(s, k.id);
        const auto map = map_name(k.park);
        auto it = std::ranges::find(maps, map, &std::pair<std::string, int>::first);
        if (it == maps.end()) maps.emplace_back(map, n);
        else it->second += n;
        if (n) parks += std::format("{}{} {}", parks.empty() ? "" : ", ", k.park.name, n);
    }
    std::string by_map;
    for (const auto &[map, n] : maps) by_map += std::format("{}{} {}", by_map.empty() ? "" : ", ", map, n);
    return text + " By map: " + by_map + "." + (parks.empty() ? std::string() : " By park: " + parks + ".");
}

std::string join() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.node) return "error: no Domination park for this level.";
    if (round_on(s) && match_of(s)->multiplayer) return "You are in the session's round.";
    if (!s.node->roster().session()) return "Not in a session: rounds with other players need one.";
    s.node->rejoin();
    return "You'll be back in the session's round with its next message (within 5 s), or in the next round.";
}

std::string settings(const std::vector<std::string> &words) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto now = [&] {
        return std::format("Next round: {}:{:02}, {}, {}.", s.next_duration / 60, s.next_duration % 60,
                           s.next_solo ? "just you" : "everyone in the session", s.next_teams ? "red against blue" : "free-for-all");
    };
    constexpr const char *usage = "usage: domination set duration <seconds>|players everyone|solo|teams on|off|map next|previous|park <id>|next|previous";
    if (words.empty()) return now();
    if (words.size() != 2) return usage;
    const auto &what = words[0], &value = words[1];
    const bool on = value == "on", off = value == "off";
    if (what == "map") {
        // The next round's map: its first park (domination set park then steps through its parks).
        if (round_on(s)) return "error: a round is running: the map changes between rounds.";
        if (s.parks.empty()) scan_parks(s);
        if (s.parks.empty()) return "error: there is no Domination park.";
        if (value != "next" && value != "previous") return usage;
        std::vector<const KnownPark *> firsts; // each map's first park, in park order
        for (const auto &p : s.parks)
            if (std::none_of(firsts.begin(), firsts.end(), [&](const KnownPark *f) { return f->park.level == p.park.level; })) firsts.push_back(&p);
        const auto *at = next_park(s);
        const auto i = std::find_if(firsts.begin(), firsts.end(), [&](const KnownPark *f) { return at && f->park.level == at->park.level; }) - firsts.begin();
        const auto n = static_cast<std::ptrdiff_t>(firsts.size());
        const auto *k = firsts[static_cast<std::size_t>(((i + (value == "next" ? 1 : -1)) % n + n) % n)];
        s.chosen = k->id;
        switch_park(s, *k, Clock::now());
        check_maps(s, Clock::now());
        const auto state = map_state(s, k->park);
        if (state == MapState::missing || state == MapState::off) return std::format("Next round on {}. {}", map_name(k->park), missing_map_text(k->park, state));
        return std::format("Next round on {}{}, in {}.", map_name(k->park), k->park.level == s.level ? " (this map)" : "", k->park.name);
    }
    if (what == "park") {
        if (round_on(s)) return "error: a round is running: the park changes between rounds.";
        if (s.parks.empty()) scan_parks(s);
        if (s.parks.empty()) return "error: there is no Domination park.";
        const auto *k = find_park(s, value);
        if (value == "next" || value == "previous") {
            // Through the chosen map's parks.
            const auto *at = next_park(s);
            std::vector<const KnownPark *> here;
            for (const auto &p : s.parks)
                if (!at || p.park.level == at->park.level) here.push_back(&p);
            const auto n = static_cast<std::ptrdiff_t>(here.size());
            const auto i = std::find(here.begin(), here.end(), at) - here.begin();
            k = here[static_cast<std::size_t>(((i + (value == "next" ? 1 : -1)) % n + n) % n)];
        }
        if (!k) return "error: no park called " + value + " (domination parks lists them).";
        s.chosen = k->id;
        switch_park(s, *k, Clock::now());
        check_maps(s, Clock::now());
        const auto state = map_state(s, k->park);
        if (state == MapState::missing || state == MapState::off)
            return std::format("Next round in {}. {}", k->park.name, missing_map_text(k->park, state));
        return std::format("Next round in {} on {}{}.", k->park.name, map_name(k->park),
                           k->park.level == s.level ? " (this map)" : ": Start loads it");
    }
    if (what == "duration") {
        const auto seconds = number(value);
        if (!seconds || *seconds < 20 || *seconds > 3600) return "error: a round lasts 20 to 3600 seconds.";
        s.next_duration = static_cast<int>(*seconds);
    } else if (what == "players" && (value == "everyone" || value == "solo")) {
        s.next_solo = value == "solo";
    } else if (what == "teams" && (on || off)) {
        s.next_teams = on;
    } else return usage;
    return now() + (s.next_teams && s.next_solo ? " (Teams need other players: a solo round is free-for-all.)" : "");
}

Snapshot snapshot() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    Snapshot m;
    const auto now = Clock::now();
    m.started = s.base != 0;
    if (s.parks.empty()) scan_parks(s);
    check_maps(s, now);
    const auto *next = next_park(s);
    for (const auto &k : s.parks) {
        m.parks.push_back({k.id, k.park.name, map_name(k.park), k.park.package, static_cast<int>(k.park.spots.size()), map_state(s, k.park),
                           k.park.level == s.level, park_rounds(s, k.id)});
        if (&k == next) m.chosen = static_cast<int>(m.parks.size()) - 1;
    }
    // A round going on is in the loaded park, whatever is chosen for the next.
    const Park *park = round_on(s) && s.park ? &*s.park : next ? &next->park : nullptr;
    if (park) {
        m.park = park->name;
        m.park_level = park->level;
        m.map = map_name(*park);
        m.package = park->package;
        m.map_status = map_state(s, *park);
        if (m.map_status == MapState::missing || m.map_status == MapState::off) m.map_problem = missing_map_text(*park, m.map_status);
        m.elsewhere = park->level != s.level;
        m.available = !m.elsewhere || (m.map_status != MapState::missing && m.map_status != MapState::off);
        m.spots = static_cast<int>(park->spots.size());
        m.starts = static_cast<int>(park->starts.size());
    }
    m.pending = s.pending.has_value();
    m.problem = s.park_problem;
    if (load_stats(s)) m.rounds = s.stats.rounds, m.online = s.stats.online, m.won = s.stats.won;
    m.duration = s.next_duration;
    m.solo = s.next_solo;
    m.teams = s.next_teams;
    if (s.node) {
        refresh_roster(s, now);
        m.others = s.node->roster().session();
        m.controls = !m.others || s.node->controls();
        if (m.others) m.controller = name_of(s, s.node->controller());
    } else m.controls = !multiplayer_session_active();
    if (round_on(s) && s.park) {
        const auto &r = *match_of(s);
        m.running = true;
        m.multiplayer = r.multiplayer;
        m.starter = r.starter == r.self;
        m.left = static_cast<int>(std::ceil(r.left_s(ms_of(now))));
        m.countdown = r.counting_down(ms_of(now));
        m.owned = r.board.owned(r.self);
        m.total = r.board.owned_total(r.self);
        m.team = r.with_teams() ? r.team_of(r.self) : -1;
        for (const auto &row : r.standings(ms_of(now)).rows)
            m.standings.push_back(std::format("{}: {} spot{}, {} held ({})", row_name(s, r, row.player), row.spots, row.spots == 1 ? "" : "s",
                                              std::format("{}:{:02}", static_cast<int>(row.hold) / 60, static_cast<int>(row.hold) % 60),
                                              points(row.total)));
    }
    return m;
}

void note_camera(const std::array<float, 3> &position) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.camera = {position[0], position[1], position[2]};
    s.camera_at = Clock::now();
}

OverlayView overlay_view() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    OverlayView view;
    const auto now = Clock::now();
    const auto *round = match_of(s);
    if (!s.park || !round || !s.play) return view;
    const auto &live = *round;
    const auto &p = *s.play;
    const auto nm = ms_of(now);
    view.running = !live.finished;
    view.results = live.finished && now < s.results_until;
    if (view.results) view.results_age = results_shown - std::chrono::duration<double>(s.results_until - now).count();
    view.visible = view.running || view.results;
    if (!view.visible) return view;
    view.base = s.base;
    view.skater = s.skater;
    view.skater_at = s.position;
    view.multiplayer = live.multiplayer;
    view.duration = live.duration_s;
    view.left = live.left_s(nm);
    view.teams = live.with_teams();
    view.my_team = view.teams ? live.team_of(live.self) : -1;
    if (!live.finished) {
        const int beat = countdown_beat(live.ms(nm));
        view.countdown = beat > 0 ? beat : 0; // 3, 2, 1
        view.go = beat == 0;                  // then GO, as tricks start to count
    }
    const auto kind_of = [&](Player who) { return !who ? OverlayOwner::none : who == live.self ? OverlayOwner::you : OverlayOwner::other; };
    const bool outlined = s.outlines.size() == s.park->spots.size() && s.outline_shown.size() == s.outlines.size();
    // How much of each outline point is drawn eases toward what it should be: seen from the camera
    // and near the skater (fading out with distance), so nothing pops in or out.
    const float step = s.shown_at == Clock::time_point{} ? 1.0f : std::min(1.0f, std::chrono::duration<float>(now - s.shown_at).count() * 4.0f);
    s.shown_at = now;
    for (std::size_t i = 0; i < s.park->spots.size(); ++i) {
        const auto &spot = s.park->spots[i];
        const auto owner = live.board.owner(static_cast<int>(i));
        OverlaySpot o;
        o.marker = spot.marker;
        o.name = spot.name;
        o.owner = kind_of(owner);
        o.owner_id = owner;
        o.owner_name = owner ? name_of(s, owner) : std::string();
        o.team = owner && view.teams ? live.team_of(owner) : -1;
        o.top = live.board.top(static_cast<int>(i));
        o.mine = live.board.best(static_cast<int>(i), live.self);
        o.here = static_cast<int>(i) == s.here;
        o.ours = live.ours(owner);
        if (outlined && std::ranges::none_of(s.outlines[i], [](const Vec3 &q) { return std::isnan(q[1]); })) {
            o.outline = s.outlines[i];
            auto &shown = s.outline_shown[i];
            for (std::size_t n = 0; n < shown.size(); ++n) {
                const auto &q = s.outlines[i][n];
                const float away = std::hypot(q[0] - s.position[0], q[2] - s.position[2]);
                const float close_by = std::clamp((outline_far - away) / (outline_far - outline_near), 0.0f, 1.0f);
                const float want = s.skater && s.outline_seen[i][n] ? close_by : 0.0f;
                shown[n] += std::clamp(want - shown[n], -step, step);
            }
            o.outline_alpha = shown;
        }
        if (i < p.flashed.size() && p.flashed[i] != Clock::time_point{}) {
            const double since = std::chrono::duration<double>(now - p.flashed[i]).count();
            if (since < flash_seconds) o.flash = static_cast<float>(1 - since / flash_seconds);
        }
        const float dx = spot.marker[0] - s.position[0], dy = spot.marker[1] - s.position[1], dz = spot.marker[2] - s.position[2];
        o.distance = s.skater ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0;
        view.spots.push_back(std::move(o));
    }
    // Edge arrows: the nearest spots that are not yours (or your team's).
    if (view.running && s.skater) {
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < view.spots.size(); ++i)
            if (!view.spots[i].ours) order.push_back(i);
        std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return view.spots[a].distance < view.spots[b].distance; });
        for (std::size_t n = 0; n < order.size() && n < arrows_shown; ++n) view.spots[order[n]].arrow = true;
    }
    view.taken = live.board.owned(live.self);
    view.total = live.board.owned_total(live.self);
    const auto results = live.standings(nm);
    for (const auto &r : results.rows) {
        OverlayStanding row{row_name(s, live, r.player), r.player, OverlayOwner::other, r.spots, r.total, r.hold, r.steals, -1};
        if (view.teams) {
            row.team = static_cast<int>(r.player);
            row.kind = row.team == view.my_team ? OverlayOwner::you : OverlayOwner::other;
        } else row.kind = kind_of(r.player);
        view.standings.push_back(std::move(row));
    }
    if (!view.teams && live.multiplayer) { // everyone in the round shows, those with nothing yet last
        std::vector<Player> playing{live.self};
        for (const auto &[who, when] : live.seen) playing.push_back(who);
        for (const auto who : playing)
            if (std::ranges::none_of(view.standings, [&](const OverlayStanding &row) { return row.id == who; }))
                view.standings.push_back({name_of(s, who), who, kind_of(who), 0, 0, 0, 0, -1});
    }
    if (view.teams) // both teams always show, a team with nothing yet last
        for (const int team : {0, 1})
            if (std::ranges::none_of(view.standings, [&](const OverlayStanding &row) { return row.team == team; }))
                view.standings.push_back({team_name(team), static_cast<Player>(team), team == view.my_team ? OverlayOwner::you : OverlayOwner::other,
                                          0, 0, 0, 0, team});
    // The other players' colours: given out in the order of their ids, so no two are alike.
    std::vector<Player> others;
    for (const auto &row : view.standings)
        if (row.team < 0 && row.kind == OverlayOwner::other) others.push_back(row.id);
    for (const auto &spot : view.spots)
        if (spot.team < 0 && spot.owner == OverlayOwner::other) others.push_back(spot.owner_id);
    std::ranges::sort(others);
    others.erase(std::unique(others.begin(), others.end()), others.end());
    const auto colour = [&](Player who) { return static_cast<int>(std::ranges::lower_bound(others, who) - others.begin()); };
    for (auto &row : view.standings)
        if (row.team < 0 && row.kind == OverlayOwner::other) row.colour = colour(row.id);
    for (auto &spot : view.spots)
        if (spot.team < 0 && spot.owner == OverlayOwner::other) spot.colour = colour(spot.owner_id);
    if (live.finished) {
        const auto &st = results.stats;
        // The round's awards: the most steals, the best trick, the longest any player held spots.
        Player stealer{};
        int steals = 0;
        for (const auto &[who, count] : st.steals)
            if (count > steals) stealer = who, steals = count;
        if (stealer) view.most_steals = std::format("{}  {} steal{}", name_of(s, stealer), steals, steals == 1 ? "" : "s");
        if (st.best_spot >= 0)
            view.best_trick = std::format("{}  {} on {}", name_of(s, st.best.player), points(st.best.score), s.park->spots[st.best_spot].name);
        Player holder{};
        double held = 0;
        for (const auto &[who, seconds] : st.hold)
            if (seconds > held) holder = who, held = seconds;
        if (holder) view.held_longest = std::format("{}  {}:{:02}", name_of(s, holder), static_cast<int>(held) / 60, static_cast<int>(held) % 60);
        if (live.multiplayer && !results.rows.empty()) {
            const auto &first = results.rows.front();
            const bool tie = results.rows.size() > 1 && results.rows[1].spots == first.spots && results.rows[1].total == first.total;
            view.tie = tie;
            view.you_won = !tie && (view.teams ? static_cast<int>(first.player) == view.my_team : first.player == live.self);
            view.winner = tie ? std::string("A TIE") : view.teams ? std::format("{} WINS", team_name(static_cast<int>(first.player)))
                                                                  : first.player == live.self ? std::string("YOU WIN")
                                                                                              : std::format("{} WINS", name_of(s, first.player));
        }
    }
    // The spot the trick going on counts for.
    if (const int trick = p.watch.trick_spot(); trick >= 0) view.trick_spot = s.park->spots[trick].name;
    while (!s.toasts.empty() && s.toasts.front().until < now) s.toasts.pop_front();
    for (const auto &t : s.toasts) {
        OverlayToast toast{t.text, t.kind};
        toast.left = std::chrono::duration<float>(t.until - now).count();
        toast.age = static_cast<float>(toast_shown) - toast.left;
        if (t.who && t.who != live.self) { // another player's claim: in their colour (their team's)
            toast.owner = kind_of(t.who);
            toast.id = t.who;
            toast.team = view.teams ? live.team_of(t.who) : -1;
            if (toast.team < 0 && toast.owner == OverlayOwner::other && std::ranges::binary_search(others, t.who)) toast.colour = colour(t.who);
        }
        view.toasts.push_back(std::move(toast));
    }
    return view;
}
}
