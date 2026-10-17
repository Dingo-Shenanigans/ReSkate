# Custom multiplayer modes

ReSkate's own game modes on the pause menu's **Multiplayer** page: each mode gets a tab of its own there, after the page's own tabs (SERVER BROWSER, HOST LOBBY, ...). The page is otherwise unchanged. Offline, where the Multiplayer page is normally left out, it shows with just the modes' tabs (a mode can be played alone).

- **Its tabs:** one per mode, in the order they registered.
- **Left side**, for the chosen mode: its own panel, with settings, status and buttons.
- **Right side:** **PLAY ONLINE**. Host a game for the mode, or join a game that plays it.

Modes run on the players' games. Lobbies and dedicated servers only advertise which mode they're for.

## What a mode provides

A mode registers once at startup with `custom_modes::add` (`custom_modes.h`). The menu does the rest.

```cpp
#include "Extension/CustomModes/custom_modes.h"

custom_modes::add({
    .id = "domination",                 // 1-16 of a-z 0-9 -: what lobbies and servers advertise
    .name = "Domination",
    .tagline = "Own the most spots",
    .panel = [] {                       // client thread, a few times a second while the menu is open
        custom_modes::Panel p;
        p.description = "Own the most spots in the park.";
        p.map = "levels/game/...";      // where it is played ("" any map); hosting loads it first
        p.map_name = "Red-bowl park";
        p.map_here = /* the player is on it */ true;
        p.settings.push_back({"Round length", "5:00", "", "mymode set duration 240", "mymode set duration 360"});
        p.buttons.push_back({"Start round", "", "mymode start"});
        return p;
    },
    .joined = [] { /* the player joined a game advertised for this mode */ },
});
```

**Panel fields:**
- **Settings:** each row is a label and value. Pressing it runs `press`; adjusting it left or right runs `less` / `more`. All three are console commands of the mode's own.
- **Status lines** are shown and not pressable: the round going on, the standings.
- **Dropdowns:** a setting with `choices` is a dropdown. Pressed, it lists them under it, one full-width row each, and picking one runs its console command.
- **Map:** `map` (a `load` destination) and `map_name` show where the mode is played. `map_choices` make the row a dropdown, for a mode played on several maps. A `map_name` with no `map` names a map that can't be loaded here.
- **Buttons** run their `command`. A button whose command contains ` start`, pressed on another map, loads the mode's map first.
- **`available` / `unavailable`** says whether the mode can be played here, and why not.

## Hosting a game for a mode

The menu's **Host a <mode> game**, or the console's `modes host <id> [public|code] [players] [name]`:
1. loads the mode's map if the player isn't on it;
2. waits for it, giving up after 4 minutes;
3. hosts with the player's saved tick rate and lobby name;
4. makes the lobby advertise the mode (`multiplayer::set_advertised_mode`, published as the Steam lobby value `rs_mode`).

Other commands:
- `modes cancel` stops waiting.
- `modes status` says what hosting is doing.
- Hosting already? The menu offers **Advertise your game as <mode>**.

When the session ends, nothing is advertised anymore.

A **dedicated server** is for a mode through its config: `[server] mode = "domination"`. It's advertised in the browser tags as `g<id>`. The server doesn't run the mode; the players' games do.

## Finding games

The menu lists the browser's lobbies and servers whose advertised mode is the chosen one (`MultiplayerLobby::mode`), and joins one when pressed. **Find <mode> games** refreshes the browser's lists.

When the player joins a game from the browser, its mode's `joined` is called once. Domination uses this to join the round going on.

Games joined by code carry no mode. The mode's own messages still reach those players.

## Compatibility

- **Older clients** ignore `rs_mode` and the `g` tag, so they still list and join these games.
- **Mode ids** that aren't valid are dropped; an empty id means no mode. The rule is `valid_mode_id` in `Engine/Game/Multiplayer/session_model.h`.
- **Tag budget:** the `g` tag takes up to 18 bytes of a server's 127-byte tag budget, out of the space the server name would otherwise use.
- **Offline mode:** the Multiplayer page shows with only the modes' tabs, and their online part says "Multiplayer is off in offline mode". The modes' own panels still work, for example a solo round.

## Files

| File | What it holds |
|---|---|
| `custom_modes.h/.cpp` | The registry, hosting for a mode, `joined`, the advertised mode |
| `custom_modes_commands.cpp` | The `modes` console commands |
| `Extension/UI/NativeMenu/native_menu.cpp` | The Multiplayer tab shown offline too when a mode is installed (`custom_modes::count()`) |
| `Extension/UI/Overlay/hub_page.cpp` (`modes_page`) | A tab per mode on the Multiplayer page, after its own: the mode's panel, PLAY ONLINE. Tabs shrink to fit when there are more than the page is wide |
| `Extension/UI/Overlay/overlay.h` (`CustomModesModel`) | The modes' model, filled in `Runtime/client_tick.cpp` |
| `Extension/Multiplayer/Steam/steam_lobbies.*` | `set_mode`, and `rs_mode` published and read |
| `Server/steam_server.*`, `Extension/Multiplayer/Steam/steam_server_browser.cpp` | The `g` tag |
| `Server/server_config.*` | The server's `mode` setting |
| `Extension/Multiplayer/Session/session.*` | `set_advertised_mode` / `advertised_mode` |

## Tests

Build with `-DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON`, then run `ctest -R "multiplayer_protocol|multiplayer_lobbies"`. They check:
- a server's mode round-trips through its tags, and invalid modes are dropped;
- lobby `rs_mode` values are read, and a missing or invalid one reads as none without refusing the lobby;
- the id rule.

That option also builds two music probe tools that fail to link in this tree. They're unrelated to this change; build the two test targets on their own.
