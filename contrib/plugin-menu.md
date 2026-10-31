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
