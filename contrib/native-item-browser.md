# Favorites, search and filters in the skater item grids

Built into ReSkate in the same way the music screen's Mods shelf was added
([native-music-ui.md](native-music-ui.md)): the grids stay the game's own and their data models are
changed through the native model functions. Nothing is hooked for it and no file of the game or of
a mod is touched.

Code: [item_browser.cpp](../Extension/Customization/item_browser.cpp) (client thread),
[item_browser_order.h](../Extension/Customization/item_browser_order.h) (the pure ordering rule),
[item_browser_overlay.cpp](../Extension/UI/Overlay/item_browser_overlay.cpp) (search bar, prompts, keys).

## What the player gets

- **F** (left stick click on a pad): the highlighted item becomes a favorite, or stops being one.
  Favorites sit right after the worn item and carry a star where the tile shows its rarity icon.
- **-** or **Ctrl+F**: a search box; every letter filters the grid at once. Enter keeps the search,
  Esc clears it.
- **X**: steps a filter (All, Favorites, Mods, Game items).
- Favorites are saved to `ReSkate.favorites.json` beside Skate.exe (`RESKATE_FAVORITES_FILE` overrides
  the path), keyed by the item's asset name, so items added later just work.

## How it works

Each slot tab is a `GridListViewModel` root (schema `0x7422aa5b`). Its `OwnedItems` (`0x345b75b1`) are
inline `ContentPresenterTileViewModel` rows (`0xcbe47f59`, 1184 bytes). A row's content reference
(`0x214d4984` -> `0x25e4d6c8`) is the tile's `OwnableViewModel` (`0x6e591955`), whose
`UIOwnableDataRef` (`0xbbe34156`) is the catalog item (`0x389117ef`: key `0x7199d8ed`, title
`0xec725743`). The tile's widget keeps `IsFocused` in the row up to date
(`0x0fb0d794 -> 0xa704272a -> 0xc52416ef -> 0xa704272a -> 0x85a0b4e8`), which is how the highlighted
item is known without hooking anything.

Schema and field hashes are the `TypeNameHash` and `NameHash` of the data-defined types under
`ui/features/mystuff/cas` and `ui/foundations/components` (readable with `reskate_cli ebx-values`).

- The game lists the worn item first and the rest by title. When a grid is first seen, or its rows are
  no longer the ones we published (the game rebuilt it), its rows are copied into a grid list model of
  our own. The visible list is then republished from that copy: worn item, favorites, the rest, less
  what the search and filter leave out. Publishing is the engine's typed copy, as for the Mods shelf.
- The accessory grids start with a "none" tile that shows no item. Such tiles stay where they are,
  in front, whatever is searched for, and cannot be made a favorite.
- The star: a favorite's `OwnableStyleSet.RarityStyle` reference is pointed at our copy of that rarity
  style (`0xb6800075`) whose `Icon` is the game's `img_Star_Fill_64`. The game sets the reference when
  a tile is shown, so this is re-applied while the grid is open.
- Our copies are destroyed before level transitions and when the grids are gone.
- Nothing runs outside the game's menus except a cheap check of `sample_game_ui_state().in_menu`.

## Seen working in game

Keyboard, offline, 3440x1440, in Apparel (Tops, Bottoms, Shoes), Accessories, Board and Gestures:
favoriting and unfavoriting, the star, reordering, the search box, the filter, favorites loaded from
the file on the next launch, leaving the menus and coming back.

## Not tried yet

- A controller (the left stick click is read, but that path was not checked).
- Tattoos (the same grid type is expected; untested).
- Multiplayer sessions, level changes with a grid open, live mod apply.
- Other screen sizes for the overlay's placement.
- The star replaces the rarity icon (bottom left). The tile has no icon slot bottom right.
- No automated tests yet; `arrange()` in item_browser_order.h is written to be tested on its own.
- `items peek` / `items poke` are research aids used to find the fields above; drop or keep as wanted.
