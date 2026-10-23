#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::anim_studio {
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
inline int playback_pop_rank(const std::string& wanted, const std::string& saved) noexcept {
    // A different source pop has different native body/board motion. Only an
    // explicitly generic binding may substitute for the requested height.
    return saved == wanted ? 0 : saved.empty() ? 1 : -1;
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
// graded, then the same fakie state (a fakie recording carries the game's fakie upper body), then the same
// lead foot (one made on the other foot is mirrored, which is exact). Only the best group is returned, so
// cycling variants alternates between equally good recordings.
inline std::vector<std::size_t> choose_stance_variants(std::span<const StanceCandidate> candidates, LiveStance live, bool front_foot) {
    std::vector<std::size_t> out;
    int best_pop = 1 << 20, best_tier = 1 << 20;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.pop_rank < 0) continue;
        const bool same_lead = c.leads_right == live.leads_right, same_fakie = c.fakie.value_or(false) == live.fakie;
        if (!front_foot && (!same_lead || !same_fakie)) continue;
        const int tier = (same_fakie ? 0 : 2) + (same_lead ? 0 : 1);
        if (c.pop_rank < best_pop || (c.pop_rank == best_pop && tier < best_tier)) {
            out.clear();
            best_pop = c.pop_rank;
            best_tier = tier;
        }
        if (c.pop_rank == best_pop && tier == best_tier) out.push_back(i);
    }
    return out;
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
