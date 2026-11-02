# Stand-ins for other players' missing items

A player's outfit can hold items from a cosmetics mod other players do not have. Before this,
each such item became the matching `default_cosmetic_items` entry (Extension/Multiplayer/Remote/
cosmetics.h) or an empty slot, so every modded skater looked the same on a PC without their mods.

`reskate.outfit.stand_ins(options)` lets a plugin choose what stands in instead. Declare
`"outfit.stand_ins"` in `reskate-plugin.json`; a call without it raises an API error.

```lua
-- A random installed item in every slot whose item is missing, skater and board.
reskate.outfit.stand_ins({pick = "random", parts = "both", seed = 1234})
-- The first item of a list that fits each missing board part.
reskate.outfit.stand_ins({pick = "first", parts = "board", items = {"Own_Truck_Gen_Default_00001"}})
-- The game's defaults again.
reskate.outfit.stand_ins(nil)
```

| Field | Default | Meaning |
| --- | --- | --- |
| `pick` | `"first"` | `"first"`: the candidates in order; `"random"`: shuffled per missing item |
| `parts` | `"both"` | which recipes it covers: `"skater"`, `"board"` or `"both"`; the others keep the defaults |
| `items` | every installed item | the candidates, at most 4096 names; only those that fit the slot are taken |
| `exclude` | none | never these: names, `*` for any run of characters (`*GhostSkater*`), any case; at most 4096 |
| `seed` | 0 | 0 to 4294967295; a new seed gives new random picks |

It returns `true`, or `false` and why not: another plugin holds the stand-ins, or it was asked
more than 4 times in a second (each change dresses every player shown again).

## What it does and does not do

- Only what this PC shows of other players changes. Nothing is sent, saved, owned or unlocked;
  the local skater's outfit is untouched.
- A paid store item never stands in, from the catalog or from a plugin's list
  (`profile_runtime::reserved_cosmetic`). Without the content catalogs that name those items no
  policy is used at all, only the defaults.
- After a plugin's candidates come the game's defaults, then an empty slot, so a slot with
  nothing that fits looks as it did before. `exclude` applies to the defaults too.
- `exclude` only removes candidates: an item a player really wears, and this PC has, still shows.
  It is applied after the shuffle, so blocking an item changes the stand-in only of the players
  who were shown it.
- "random" is seeded by everything the peer wears, the slot, the missing item and `seed`: a
  player keeps the same stand-ins for as long as they wear the same outfit, two players in
  different outfits get different ones, and every PC with the same items and seed shows the same.
- A change re-dresses the players already shown one at a time, 100 ms apart
  (`update_remote_cosmetics`), not the whole server in one frame.
- One plugin holds the stand-ins at a time; they go back to the defaults when it unloads.

## Where it lives

- `Extension/Multiplayer/Remote/stand_ins.h/.cpp`: the policy, its store and the ordering.
- `native_cosmetics_layout.h`: `catalog()` (the whole installed catalog), `slot_category`,
  `substitute_missing_each` (fallbacks per missing item).
- `native_cosmetics.cpp`: the candidates per slot category, cached per catalog.
- `Extension/Plugins/plugin_api_world.cpp`: the Lua call; `plugin_game_runtime.cpp`: the policy
  set and cleared.
- Tests: `cosmetic_layout_regression` (catalog walk, first and random, paid items never taken)
  with the exclude list and its stars, and the store, and `plugins_choose_stand_ins` (the calls,
  their checks, and the permission).

Not verified in the game: the catalog walk over the real cosmetics manager and how random
stand-ins look on real players. The layout matches `lookup`, which the game already uses.
