# Game menu observation

`reskate.menu.game()` returns the current observation of the game's menu.
Declare `"menu"` in `reskate-plugin.json`; a call without that permission raises
an API error before reading the game state.

The call takes no arguments and returns a table with these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `known` | boolean | The native screen model was available for this observation. |
| `open` | boolean | The observed game menu is open. Interpret this together with `known`. |
| `skaters` | boolean | The menu is open on the Skaters page; otherwise false. |
| `buttons` | array of strings | Up to 64 observed menu button names, in their reported order; empty when closed or unavailable. |

`known = false` means the menu state is unavailable: `open = false` alone does
not prove that the menu is closed. `known = true` with `open = false` is an
observed closed menu; `known = true` with `open = true` is an observed open menu.

```lua
local menu = reskate.menu.game()
if not menu.known then
    return -- Wait for a usable observation.
end
if menu.open then
    return -- The game menu was observed open.
end
-- The game menu was observed closed.
```

The sampler caches observations, so the table is not an atomic guarantee that
the menu remains closed during later calls. It grants no input ownership or
physics readiness. `known` is additive; the existing `open`, `skaters` and
`buttons` fields and the `menu` permission keep their meanings.

# What is up over the game

`reskate.menu.state()` says which of the game's pause menu, ReSkate's menu and
the replay editor are on screen, and the `menu_changed` event says when that
changes. Both need the `"menu"` permission; without it the call raises an API
error, and `reskate.on("menu_changed", ...)` is refused.

The call takes no arguments and returns a table with these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `known` | boolean | The native screen model was available, as for `menu.game()`. |
| `pause` | boolean | The game's own pause menu is up, on any of its pages. Interpret this together with `known`. |
| `reskate` | boolean | ReSkate's menu is up: its window over the game, or one of its pages in the pause menu. |
| `page` | string or nil | Which of ReSkate's pages the pause menu shows: a `reskate.Enums.MenuPage` value (`"multiplayer"`, `"mod_options"` or `"mod_manager"`), or nil for none. |
| `replay_editor` | boolean | The replay editor is open. |

`known` covers only `pause`, the one field read from the game's screen model;
`reskate`, `page` and `replay_editor` are always read. As with `menu.game()`,
`known = false` with `pause = false` does not prove the pause menu is closed.

`menu_changed` passes two of these tables, what is up now and what was up
before. It is heard once for each change, including a change in `known`. What
was already up when the plugin started listening is not reported as a change,
so read `menu.state()` once at load.

```lua
local function paused(state)
    return not state.known or state.pause or state.reskate or state.replay_editor
end

local hidden = paused(reskate.menu.state())
reskate.on("menu_changed", function(state, previous)
    hidden = paused(state) -- Hide this plugin's HUD under any menu.
end)
```

Like `menu.game()`, these are observations, not guarantees. They grant no
input ownership.
