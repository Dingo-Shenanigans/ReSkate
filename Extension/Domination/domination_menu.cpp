#include "domination_menu.h"
#ifdef DINGOSDK_CUSTOM_MODES // cmake/Runtime.cmake: Custom Multiplayer is in the tree
#include "domination.h"
#include "Extension/CustomModes/custom_modes.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

// Domination's tab on the Multiplayer page (Extension/CustomModes): its panel, built from
// domination::snapshot() and run with its console commands.
namespace dingosdk::domination {
namespace {
std::string clock(int seconds) {
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}
// The round lengths the menu steps through, and the next one each way from `now`.
int stepped(int now, int way) {
    constexpr std::array lengths{60, 120, 180, 300, 420, 600, 900};
    if (way > 0) {
        for (const int length : lengths)
            if (length > now) return length;
        return lengths.back();
    }
    for (auto it = lengths.rbegin(); it != lengths.rend(); ++it)
        if (*it < now) return *it;
    return lengths.front();
}

custom_modes::Panel panel() {
    const auto d = snapshot();
    custom_modes::Panel p;
    p.description = "Own the most spots in the park. The best single trick landed on a spot owns it; a higher one steals it.";
    p.available = d.started && d.available;
    p.unavailable = !d.started             ? std::string("Domination is not running in this build.")
                  : !d.map_problem.empty() ? d.map_problem
                                           : "No Domination park" + (d.problem.empty() ? std::string(".") : " (" + d.problem + ").");
    // The park's map: loaded first by Start and hosting when it is another one. A map that isn't
    // here is only named (nothing to load).
    const bool missing = d.map_status == MapStatus::missing || d.map_status == MapStatus::off;
    if (!missing) p.map = d.park_level;
    p.map_name = d.map + (missing ? (d.map_status == MapStatus::off ? " (switched off)" : " (not downloaded)") : d.elsewhere ? "" : " (this map)");
    p.map_here = !d.elsewhere;
    // Between rounds, whoever starts them picks the map (each map's first park) and then a park on it,
    // from dropdowns.
    const bool picks = !d.running && !d.pending && (d.controls || !d.others);
    const auto state_note = [](MapStatus status) {
        return status == MapStatus::missing ? std::string("not downloaded") : status == MapStatus::off ? std::string("switched off") : std::string();
    };
    std::vector<custom_modes::Choice> parks_here;
    for (const auto &k : d.parks) {
        if (std::none_of(p.map_choices.begin(), p.map_choices.end(), [&](const custom_modes::Choice &c) { return c.label == k.map; }))
            p.map_choices.push_back({k.map, "domination set park " + k.id, k.here ? std::string("this map") : state_note(k.status), k.map == d.map});
        if (k.map == d.map)
            parks_here.push_back({k.name, "domination set park " + k.id, std::to_string(k.spots) + " spots",
                                  d.chosen >= 0 && d.parks[static_cast<std::size_t>(d.chosen)].id == k.id});
    }
    if (!picks || p.map_choices.size() < 2) p.map_choices.clear();
    if (d.running) {
        // The round's park stays to the end; the Park row is back between rounds.
        p.status.push_back({"Park", d.park});
        p.status.push_back({"Round", (d.multiplayer ? std::string("With everyone, ") : std::string("Solo, ")) +
                                         (d.countdown ? std::string("starting") : clock(d.left) + " left")});
        if (d.team >= 0) p.status.push_back({"Team", d.team == 0 ? "Red" : "Blue"});
        p.status.push_back({"You own", std::to_string(d.owned) + " of " + std::to_string(d.spots) + " spots"});
        for (std::size_t i = 0; i < d.standings.size() && i < 8; ++i) p.status.push_back({std::to_string(i + 1) + ".", d.standings[i]});
        p.buttons.push_back({d.multiplayer && !d.starter && !d.controls ? "Leave round" : "Stop round", {}, "domination stop"});
    } else {
        if (d.others) p.status.push_back({"Rounds", d.controls ? std::string("You start them for everyone") : d.controller + " starts them"});
        // This player's rounds so far (domination stats): in all, in the chosen park, and with others.
        if (d.rounds) {
            std::string played = std::to_string(d.rounds) + (d.rounds == 1 ? " round" : " rounds");
            if (d.chosen >= 0) played += ", " + std::to_string(d.parks[static_cast<std::size_t>(d.chosen)].played) + " in this park";
            if (d.online) played += ", " + std::to_string(d.online) + " with others (" + std::to_string(d.won) + " won)";
            p.status.push_back({"You've played", played});
        }
        // The park: a dropdown of the chosen map's (the Map row above picks the map).
        if (d.chosen >= 0) {
            custom_modes::Setting park{"Park", d.parks[static_cast<std::size_t>(d.chosen)].name + " (" + std::to_string(d.spots) + " spots)"};
            park.enabled = picks && parks_here.size() > 1;
            park.choices = std::move(parks_here);
            p.settings.push_back(std::move(park));
        }
        p.settings.push_back({"Round length", clock(d.duration), {}, "domination set duration " + std::to_string(stepped(d.duration, -1)),
                              "domination set duration " + std::to_string(stepped(d.duration, 1))});
        p.settings.push_back({"Players", d.solo ? "Just me" : d.others ? "Everyone in the session" : "Everyone (nobody else here)",
                              std::string("domination set players ") + (d.solo ? "everyone" : "solo")});
        p.settings.push_back({"Teams", d.teams ? "Red against blue" : "Free-for-all", std::string("domination set teams ") + (d.teams ? "off" : "on")});
        if (d.pending) p.buttons.push_back({"Start round", "Loading the map...", {}, false});
        else if (d.others && !d.controls) {
            // In someone else's session their rounds take everyone in; a round alone is still yours to play.
            p.buttons.push_back({"Start round", d.controller + " starts the rounds", {}, false});
            p.buttons.push_back({"Play a round alone", {}, "domination start solo", p.available && !d.elsewhere});
            p.buttons.push_back({"Back into the round", "If you left it", "domination join", true});
        } else p.buttons.push_back({"Start round", missing ? "Get the map first" : d.elsewhere ? "Loads the map, then starts" : std::string(), "domination start",
                                    p.available});
    }
    p.note = d.pending ? std::string("Loading the park's map. The round starts when you're in it.")
           : d.running ? std::string("Stop the round to pick another park.")
           : missing   ? std::string("Pick a park on a map you have, or get this one and come back.")
           : !d.others ? std::string("Every round starts with every spot open. In a session, every player on the map plays the host's rounds.")
           : d.controls ? std::string("You run this session's rounds: everyone on the map is in each round you start, late arrivals too.")
           : d.controller + " runs this session's rounds: when one starts, you're in it.";
    return p;
}
} // namespace

void register_custom_mode() {
    // (Joining a Domination game needs nothing more: the host's rounds take every player in.)
    custom_modes::add({"domination", "Domination", "Own the most spots", panel, {}});
}
}
#else
// Without Custom Multiplayer, Domination is played from the console alone.
namespace dingosdk::domination {
void register_custom_mode() {}
}
#endif
