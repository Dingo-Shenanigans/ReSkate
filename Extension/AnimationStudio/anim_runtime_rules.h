#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::anim_studio {
// The game's pop grade (EnumGS.PopHeight) as the studio's pop name: "" when it did not grade it.
inline std::string_view graded_pop_name(int grade) noexcept {
    return grade == 1 ? "low" : grade == 2 ? "mid" : grade == 3 ? "high" : "";
}
// How far back to look for the grade of the pop `elapsed_seconds` ago (and a little more), never before 0.
inline std::uint64_t pop_grade_since(std::uint64_t now, double elapsed_seconds) noexcept {
    const double age = std::isfinite(elapsed_seconds) ? std::max(0.0, elapsed_seconds) * 1000.0 + 300.0 : static_cast<double>(now);
    return age >= static_cast<double>(now) ? 0 : now - static_cast<std::uint64_t>(age);
}
// The game's pop grades as it wrote them, any skater's, each with the skater's graph that wrote it (its own instance:
// one per skater), and which of them are the local skater's. Alone in the game every grade is his, and his graph is
// learned from them. With others about (a session, AI skaters) only his graph's grade is his; until his graph is known,
// it is learned from his own pops: the one graph that graded around his takeoff, two takeoffs running (a skater joining
// a session can be given a new graph). Until then, none (the studio goes by the air time). Not thread-safe on its own.
struct PopGradeLog {
    struct Grade {
        std::uint64_t ms{}; // when (GetTickCount64)
        std::uintptr_t graph{};
        int value{};
    };
    std::array<Grade, 32> grades{};
    std::size_t next{};
    std::uintptr_t ours{}, candidate{};
    int candidate_seen{};
    std::uint64_t learned_attempt{~std::uint64_t{0}};
    void add(std::uint64_t ms, std::uintptr_t graph, int value) noexcept {
        grades[next] = {ms, graph, value};
        next = (next + 1) % grades.size();
    }
    // The grade of the local skater's pop at `takeoff_ms` (0 none), asked at `now_ms`. `solo`: no other skater in the
    // game. `attempt`: the air it is for (his graph is looked for once an air).
    int local(std::uint64_t takeoff_ms, std::uint64_t now_ms, bool solo, std::uint64_t attempt) noexcept {
        const std::uint64_t from = takeoff_ms > 300 ? takeoff_ms - 300 : 0; // the grade comes as he pops, a little before
        const auto latest = [&](std::uintptr_t graph) -> const Grade* {
            const Grade* found = nullptr;
            for (const auto& g : grades)
                if (g.value > 0 && g.ms >= from && g.ms <= now_ms && (!graph || g.graph == graph) && (!found || g.ms >= found->ms)) found = &g;
            return found;
        };
        if (solo) {
            const auto* g = latest(0);
            if (g) ours = g->graph; // every grade is his: his graph, for when others come
            return g ? g->value : 0;
        }
        // Once an air, when his grade is surely written: the graph that graded around his takeoff, alone.
        if (attempt != learned_attempt && now_ms >= takeoff_ms + 150) {
            learned_attempt = attempt;
            std::uintptr_t only = 0;
            bool many = false;
            for (const auto& g : grades)
                if (g.value > 0 && g.ms >= from && g.ms <= takeoff_ms + 150) {
                    if (!only) only = g.graph;
                    else if (g.graph != only) many = true;
                }
            if (only && !many) {
                if (only == ours) candidate_seen = 0;
                else if (only == candidate) {
                    if (++candidate_seen >= 2) {
                        ours = candidate;
                        candidate_seen = 0;
                    }
                } else {
                    candidate = only;
                    candidate_seen = 1;
                }
            }
        }
        if (!ours) return 0;
        const auto* g = latest(ours);
        return g ? g->value : 0;
    }
};
// One name for a trick, however the game spelled it. The game names its tricks in more than one way: what it says in
// the air (current_trick) is "n_kickflip", "fsimpossible"; its scoring data, where the studio's first trick list came
// from, says "nolliekickflip", "nolliebsimpossible"; its flick list says "N_Kickflip", "FrontFoot_Impossible". A saved
// trick and the game's trick are matched by this key, so every spelling of a trick is the same trick and a trick
// saved under an older name still plays. Pieces of the game's animation ("native:...") are left as they are.
inline std::string trick_key(std::string_view name) {
    if (name.starts_with("native:")) return std::string(name);
    std::string s;
    for (const unsigned char c : name) {
        const char l = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_') s += l;
    }
    bool nollie = false;
    std::string_view rest = s;
    if (rest.starts_with("nollie") && rest.size() > 6) nollie = true, rest.remove_prefix(6);
    else if (rest.starts_with("n_")) nollie = true, rest.remove_prefix(2);
    std::string word;
    for (const char c : rest)
        if (c != '_') word += c;
    if (word == "frontfootimpossible" || word == "ffimpossible") word = "fsimpossible";
    else if (word == "backfootimpossible" || word == "bfimpossible") word = "bsimpossible";
    else if (word == "bspopshuvit") word = "popshuvit";
    else if (word == "bs360popshuvit") word = "360popshuvit";
    if (!nollie) return word;
    return word.empty() ? std::string("nollie") : "n_" + word;
}
struct ContactWindow {
    std::optional<double> since;
    bool observe(double now, bool contact) noexcept {
        if (!contact) { since.reset(); return false; }
        if (!since) since = now;
        return now - *since >= 1.0 / 30.0;
    }
};
struct GameplayClipClock {
    float time{-1}, air_rate{1}, landing_time{};
    double at{-1};
    std::optional<double> landing_started;
    void reset() noexcept { *this = {}; }
    float advance_air(double now, float start, float rate, float limit) noexcept {
        if (!std::isfinite(now) || !std::isfinite(start) || !std::isfinite(rate) || rate < 0 || !std::isfinite(limit)) return time;
        if (at >= 0 && now < at) return time;
        const double next = at < 0 || time < 0 ? start : time + (now - at) * rate;
        const float shown = static_cast<float>(std::min(static_cast<double>(limit), next));
        observe_air(now, shown, shown >= limit ? 0.0f : rate);
        return shown;
    }
    void observe_air(double now, float source_time, float rate) noexcept {
        if (!std::isfinite(now) || !std::isfinite(source_time) || !std::isfinite(rate)) return;
        at = now; time = source_time; air_rate = std::max(0.0f, rate); landing_started.reset();
    }
    float land(double now, float speed, float duration, float fallback) noexcept {
        if (!std::isfinite(now) || !std::isfinite(speed) || speed <= 0 || !std::isfinite(duration) || duration < 0) return time;
        if (at < 0 || time < 0) { at = now; time = std::clamp(fallback, 0.0f, duration); air_rate = speed; return time; }
        if (now < at) return time;
        if (!landing_started) { landing_started = at; landing_time = time; }
        // Continue from the last airborne sample, not clip.landing + contact age.
        // Integrate a smooth rate ramp exactly, including callbacks spanning the whole ramp.
        constexpr double ramp = 0.08;
        const double elapsed = std::max(0.0, now - *landing_started), u = std::clamp(elapsed / ramp, 0.0, 1.0);
        const double head = ramp * (air_rate * u + (speed - air_rate) * (u*u*u - 0.5*u*u*u*u));
        const double advance = head + speed * std::max(0.0, elapsed - ramp);
        time = static_cast<float>(std::min(static_cast<double>(duration), landing_time + advance));
        at = now;
        return time;
    }
};
// The pop a saved trick is for, against the pop the game graded: 0 the same, 1 any pop (made so, or a pack's ANY). With
// `nearest` - a trick on its own pop (a pack's OWN, or no pack) - then the other pops, the nearest first, so a trick made at
// one pop plays on every pop there is no version of it for: a heelflip saved at mid played on mid pops alone, and the game
// grades a pop by how it is popped (TEST IN GAME plays on any pop, so there it did play). The game's clock follows the
// clip of whichever pop it plays and stretches it onto the trick's own air (NativeModels). A pop a pack's LOW, MID or HIGH
// fixed plays on that pop alone. Below 0: never.
inline int playback_pop_rank(const std::string& wanted, const std::string& saved) noexcept {
    // A trick on its own pop plays only on that height; on every pop only when its pack's rule says any (saved empty).
    // (From 2026-10-24 it also played on the pops it had no version for: a low trick on the game's high pop laid its keys
    // on a body tucked otherwise, not the editor's pose, a foot through the deck - renztre, 2026-10-25.)
    if (saved == wanted) return 0;
    return saved.empty() ? 1 : -1;
}
inline bool episode_selection_changed(std::string_view selected, std::string_view reported,
                                      std::string_view selected_pop = {}, std::string_view wanted_pop = {}) noexcept {
    // Blank names are transient state. An explicit pop binding must be reconsidered
    // when the native grade refines an estimate; generic bindings remain stable.
    return !reported.empty() && (selected != reported || (!selected_pop.empty() && selected_pop != wanted_pop));
}
inline bool riding_switch(bool left_foot_leads, bool goofy) noexcept { return left_foot_leads == goofy; }
// Which foot is forward. A goofy skater leads with his right; riding switch turns that round, so a regular
// skater riding switch leads with his right too. A trick belongs to the front foot and the back foot, not
// to the left and the right: a kickflip is flicked with the front foot whoever is standing on the board.
inline bool leads_right(bool goofy, bool switch_stance) noexcept { return goofy != switch_stance; }
// ---- which recording plays, from everything the game tells us at takeoff ---------------------------
// What the game does (contrib/animator-studio/GAME_STANCE_NOTES.md): one mirror for the foot that leads
// (regular is its goofy clips mirrored, switch flips it), fakie is the same clips with an upper-body layer
// on top (never a mirror), nollie is its own clips and its own trick names, and board backwards (a shuvit,
// or a push out of fakie into switch) only turns the deck under him. So a recording is described by the
// foot it leads with and whether it was made rolling fakie; nollie is told apart by the trick's name before
// this, and board backwards is taken care of by the board's frame when it plays.
struct StanceCandidate {
    bool leads_right{};        // the foot it was made leading with, read from its clip
    std::optional<bool> fakie; // made rolling fakie; empty for a recording from before that was kept (taken as not)
    int pop_rank{};            // playback_pop_rank against the pop the game graded; below 0 cannot play
};
struct LiveStance {
    bool leads_right{}; // the game's mirror: clear when the right foot leads
    bool fakie{};       // rolling backwards at takeoff
};
// The recordings that may play now, best first: exact stance only takes one made on the foot that leads now
// and in the same fakie state, and never mirrors it. Front foot takes any, the closest first: the pop the game
// graded, then the same fakie state (a fakie recording carries the game's fakie upper body). The foot it was made
// leading with does not part them: one made on the other foot plays mirrored, which is exact, and shuffle and In
// turns go round all of them. (The same lead foot came first too until 2026-10-26: a playlist of tricks made in
// different stances shuffled only those made on the foot that led, often one - "shuffle does not shuffle".)
// Only the best group is returned, so cycling variants alternates between equally good recordings.
inline std::vector<std::size_t> choose_stance_variants(std::span<const StanceCandidate> candidates, LiveStance live, bool front_foot) {
    std::vector<std::size_t> out;
    int best_pop = 1 << 20, best_tier = 1 << 20;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.pop_rank < 0) continue;
        const bool same_lead = c.leads_right == live.leads_right, same_fakie = c.fakie.value_or(false) == live.fakie;
        if (!front_foot && (!same_lead || !same_fakie)) continue;
        const int tier = same_fakie ? 0 : 1;
        if (c.pop_rank < best_pop || (c.pop_rank == best_pop && tier < best_tier)) {
            out.clear();
            best_pop = c.pop_rank;
            best_tier = tier;
        }
        if (c.pop_rank == best_pop && tier == best_tier) out.push_back(i);
    }
    return out;
}
// Which of `count` equally good recordings of a trick plays this attempt (choose_stance_variants' best group). `previous`:
// the one that played last, plus one (0: none yet). In turns: the one after it, round and round, for as long as he skates;
// else any of them at random but the one before (never the same twice running). Any number of them: a thousand too.
template <class Random>
std::size_t next_variant(std::size_t previous, std::size_t count, bool in_turns, Random& random) {
    if (count <= 1) return 0;
    if (in_turns) return previous % count;
    const bool avoid = previous > 0 && previous <= count;
    auto pick = std::uniform_int_distribution<std::size_t>(0, count - (avoid ? 2 : 1))(random);
    if (avoid && pick >= previous - 1) ++pick;
    return pick;
}
// A recording made on the other foot is played mirrored, the game's way.
inline bool plays_mirrored(const StanceCandidate& c, LiveStance live) noexcept { return c.leads_right != live.leads_right; }
inline float advance_time(float time, float elapsed, float speed, float duration, bool loop) noexcept {
    if (!std::isfinite(elapsed) || elapsed <= 0 || !std::isfinite(speed) || speed <= 0 ||
        !std::isfinite(duration) || duration <= 0) return time;
    const double next = static_cast<double>(time) + static_cast<double>(elapsed) * speed;
    return static_cast<float>(loop && next > duration ? std::fmod(next, duration) : std::min(next, static_cast<double>(duration)));
}
inline std::string variation_name(std::string base, unsigned number) {
    const auto suffix = " " + std::to_string(number);
    base.resize(std::min(base.size(), std::size_t{48} - suffix.size()));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    return base + suffix;
}
inline bool fresh_sample(double now, double captured, double previous) noexcept {
    return std::isfinite(captured) && captured > previous && captured <= now && now - captured < 0.1;
}
} // namespace dingosdk::anim_studio
