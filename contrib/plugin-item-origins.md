# Item origins and favorites

Read-only. A plugin can ask where a cosmetic item comes from, and which items the player has
starred in the game's own item grids. Filtering the apparel or board picker stays the plugin's
job. Declare the permission in `reskate-plugin.json`. A call without it raises before anything
is read.

| Call | Permission |
| --- | --- |
| `reskate.mods.item_origins` | `mods.read` |
| `reskate.menu.favorites` | `menu` |
| `favorites_changed` | `menu` |

`reskate.mods.list` (also `mods.read`) gains `author` and `version` when the mod's manifest
declares them. It does not grow an install time: ReSkate does not record when a folder was
first seen, and none is invented.

## `reskate.mods.item_origins(keys)`

`keys` is an array of item keys, the same strings `reskate.menu.grid` returns in `items`.
At most 4000 keys, each at most 255 characters, the same bounds as `reskate.menu.grid_show`.

Returns two values. The first is a table keyed by the keys that were asked for. The second
is `false` while the merge's item list or the content catalogues have not been read yet, and
`true` once both have. While it is `false`, every key's `source` is `"unknown"`.

| Field | Type | Meaning |
| --- | --- | --- |
| `source` | `"mod"`, `"changed"`, `"game"` or `"unknown"` | `"mod"`: a mod adds it. `"changed"`: the game ships it and a mod changes it. `"game"`: the game ships it and no mod touches it. `"unknown"`: not known, or the catalogues are not ready. |
| `folder` | string, optional | The winning mod's folder, the same string `reskate.mods.list` uses. Omitted when there is no mod. |
| `title` | string, optional | That mod's manifest name, with underscores as spaces. |
| `author` | string, optional | The manifest's `author`, when it declares one. Never guessed from a package name. |
| `version` | string, optional | The manifest's version, when it declares one. |

The table is keyed by the strings that were passed in. Matching the merge and the catalogues
ignores ASCII case, so `Own_Hat` and `own_hat` are the same item and the answer is stored under
whichever spelling was asked for.

The winning mod is the one the merge keeps: highest priority first, the same mod the item
card names. A key that is in neither the merge nor the catalogues is `"unknown"` even when
the second value is `true`. `"unknown"` is never reported as `"game"`.

No path is returned. The call does not read files for the plugin, and it does not change
favorites or the grid.

```lua
local origins, ready = reskate.mods.item_origins(grid.items)
if not ready then
    return -- Wait. An unread catalogue is not the base game.
end
local hat = origins[key]
if hat.source == "mod" or hat.source == "changed" then
    -- hat.folder, hat.title, hat.author, hat.version
end
```

## `reskate.menu.favorites()`

Returns the player's starred item keys as a set (`{ [key] = true }`), or `nil` and a short
reason when that set has not been read yet. An empty table means it was read and nothing is
starred. `"not loaded"` is the reason before the first read. Starring and unstarring stay
the player's own key; this call does not write.

`favorites_changed` has no arguments. It fires after the set changes: the player stars or
unstars an item, or the set is read for the first time after a plugin has already looked.
A plugin that starts once the set is already loaded does not get an event for that load; it
calls `favorites()` instead.

```lua
reskate.on("favorites_changed", function()
    local favs = reskate.menu.favorites()
    if not favs then
        return
    end
    -- favs[key] == true for each starred item
end)
```

The set is the one the item grids use for their star, for Apparel and for board parts.
Keys match the grid's keys, including case.
