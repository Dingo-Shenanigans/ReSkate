#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Custom multiplayer modes: ReSkate's own game modes (Domination, ...), each a tab of its own on
// the pause menu's Multiplayer page, after the page's own tabs (hub_page.cpp modes_page). A mode
// registers itself once with add(); the page then shows its panel, hosts games for it and finds
// games that play it. Offline, the Multiplayer page is there for the modes alone (played alone).
// README.md shows how a mode plugs in.
//
// - A mode's panel is plain data: settings, status lines and buttons, each pressed one running a
//   console command of the mode's own. The menu draws every mode the same way.
// - Hosting: a public lobby or a join-code session advertising the mode (the lobby's rs_mode; a
//   dedicated server's `mode` setting). The mode's map is loaded first if the mode names one.
// - Finding: the browser's lobbies and servers whose advertised mode is this one.
// - Joining a game advertised for a mode calls the mode's `joined`, once.
// Modes run on the players' games; servers only advertise them.
namespace dingosdk::custom_modes {
// One entry of a dropdown: picking it runs its console command.
struct Choice {
    std::string label, command;
    std::string note;   // on its right: "not downloaded", "12 spots", ...
    bool chosen = false; // the one in use now
};
struct Setting {
    std::string label, value;
    std::string press;      // console command run when the row is pressed ("" none)
    std::string less, more; // console commands for a row adjusted left / right ("" none: a plain row)
    bool enabled = true;
    std::vector<Choice> choices; // non-empty: a dropdown (pressing the row lists them under it)
};
struct Line {
    std::string label, value;
};
struct Button {
    std::string label, value, command; // `command`: the console command it runs
    bool enabled = true;
};
struct Panel {
    std::string description;          // a sentence or two: what the mode is
    bool available = true;            // playable here and now
    std::string unavailable;          // why not, when it is not
    std::string map_name, map;        // where it is played (a `load` destination); empty: any map
    bool map_here = true;             // the local player is on that map
    // A mode played on several maps: the Map row is a dropdown of them (empty: it only shows the map).
    // `map_name` alone (no `map`) names a map that can't be loaded here.
    std::vector<Choice> map_choices;
    std::vector<Setting> settings;    // the next round's settings
    std::vector<Line> status;         // the round going on: time left, standings, ...
    std::vector<Button> buttons;      // start, join, stop, ...
    std::string note;                 // a line under the panel
};
struct Mode {
    std::string id;      // what lobbies and servers advertise: 1 to 16 of a-z, 0-9 and '-'
    std::string name;    // "Domination"
    std::string tagline; // "Own the most spots"
    // The panel as it is now. Called on the client thread a few times a second while the menu
    // is open, so it must be quick and must not block.
    std::function<Panel()> panel;
    // Optional: the local player has just joined a game advertised for this mode (client thread).
    std::function<void()> joined;
};
// Registers a mode (startup, any thread). False for an invalid id, no panel, or an id taken.
bool add(Mode mode) noexcept;

struct Listed {
    std::string id, name, tagline;
    Panel panel;
};
// Every registered mode in the order they were added, each with its panel now (client thread).
std::vector<Listed> list();
// How many modes are registered (any thread; no panels asked for).
std::size_t count() noexcept;

// Hosts a game for a mode: a public lobby (or join code) advertising it, with `capacity` players
// and an optional lobby name. When the mode names a map the local player is not on, the game is
// hosted once that map has loaded (load it first: the menu does). Any thread.
std::string host(std::string_view id, bool public_lobby, unsigned capacity, std::string_view name = {});
std::string cancel_host();
struct HostState {
    std::string mode;   // the mode the game being hosted (or waited for) advertises
    std::string status; // what hosting is doing
    bool pending = false;
};
HostState host_state();

// Client thread, every tick: hosting once the map is in, the `joined` calls, and the advertised
// mode dropped when the session ends.
void tick() noexcept;
}
