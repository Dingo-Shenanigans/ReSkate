# Other players' items

A plugin can read what another player wears and choose items of theirs not to show on this
screen. Together with `reskate.mods.item_origins` (which mod an item is from) and
`reskate.outfit.stand_ins` (what shows instead), that is enough for a plugin to hide, say, every
item of one mod, or every mod item on one player. The rules and their menu are the plugin's.

| Call | Permission |
| --- | --- |
| `reskate.session.outfit(player)` | `session.read` |
| `reskate.session.hide_items(list)` | `session.skaters` |

`session.skaters` is the permission that already lets a plugin hide a whole player
(`reskate.session.hide`); hiding some of their items is less than that.

## `reskate.session.outfit(player)`

`player` is an id or a player table, as `reskate.session.players` gives them. Returns
`{skater = {names}, board = {names}}`: the item names of that player's skater and board, as their
game sent them, empty slots left out. `nil` before their outfit has arrived, for a player not in
the session, and for the local player (`reskate.outfit` reads that one).

The names are the ones `reskate.mods.item_origins` and the item grids use. An item this PC does
not have is in the list all the same: it is what they wear, not what is shown.

## `reskate.session.hide_items(list)`

```lua
reskate.session.hide_items({
    items = {"Own_Skull_Mask"},                     -- on everyone
    players = {["76561198000000001"] = {"Own_Tee"}}, -- on this player only
})
reskate.session.hide_items(nil) -- show them all again
```

A hidden item is shown the way an item this PC does not have is: a plugin's stand-in, the slot's
default, or nothing. The list is the plugin's whole list and replaces the one before. At most 4096
names in all and 64 players; names in any case. Returns `true`, or `false` and why not (asked more
than 4 times in a second: each change dresses every player shown again, one at a time).

- Only this screen changes. Nothing is sent and nobody else sees a difference.
- Every plugin's list applies; a plugin's goes when it unloads.
- The game's own items can be hidden too: an item is a name, wherever it comes from.

## Where it lives

- `Extension/Plugins/plugin_session_feed.h`: `Outfit` (published by the session as outfits arrive)
  and `HiddenItems` (read when an outfit is put on).
- `Extension/Multiplayer/Session/session.cpp`: publishes the outfits; puts an outfit on again when
  the hidden items change.
- `Extension/Multiplayer/Remote/native_skater_spawn.cpp` and `native_cosmetics.cpp`: a hidden item
  is taken for a missing one, next to the paid store items.
- `Extension/Plugins/plugin_api_world.cpp`: the Lua calls.
- Tests: `plugins_read_and_hide_other_players_items` (the calls, their checks, both permissions,
  and the hidden list as the session reads it).

Verified in the game, on a public server of 65 players: `session.outfit` read every player's items,
`mods.item_origins` named the mod of each; an item hidden on everyone (a game item and mod items)
no longer showed on those players, and showed again once the list was cleared.
