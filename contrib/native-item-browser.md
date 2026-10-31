# Favorites, search and filters in the skater item grids

Built into ReSkate in the same way the music screen's Mods shelf was added
([native-music-ui.md](native-music-ui.md)): the grids stay the game's own and their data models are
changed through the native model functions. Nothing is hooked for it and no file of the game or of
a mod is touched.

Code: [item_browser.cpp](../Extension/Customization/item_browser.cpp) (client thread),
[item_browser_order.h](../Extension/Customization/item_browser_order.h) (the pure ordering rule),
[item_browser_overlay.cpp](../Extension/UI/Overlay/item_browser_overlay.cpp) (search bar, the pickers' filters, prompts, keys).

## What the player gets

- **F** (left stick click on a pad): the highlighted item becomes a favorite, or stops being one.
  Favorites sit right after the worn item and carry a star where the tile shows its rarity icon.
- **-**, **Ctrl+F** or a click on it: a search box, at the top over the empty space right of the grid, with the
  key prompts on one line under it (it shows no count of what it found); every letter filters the grid at once. Enter, or a click
  elsewhere, keeps the search, Esc clears it. Each word typed must be in the item's name, in its mod's author, name or folder
  (so a pack's name or its author's shows that pack's items), or, from three letters on, in its
  asset name.
- **X**: steps a filter (All, Favorites, Mods, skate.: the game's own items). An item is a mod's
  when the mods' merge says a mod adds or changes it, as `reskate.mods.item_origins` reports.
- In the pickers (clothing, board, tattoos, gestures and appearance: hair, makeup, skin...), four lists in a line
  right of the search box, as tall as it and level with it: **Author** and **Mod pack** (the mods with items in the grid, and Any; the
  packs only the chosen author's), **Sort** (Game order, the default, Name A-Z, Name Z-A, Mod pack,
  Author; favorites still come first) and **Show** (All, Favorites, skate., Mods: the filter X
  steps). They stack with each other and the search. Click one, or **V**
  then Left/Right and Enter, to open its list; typing there searches its choices (Backspace edits, "No
  matches" when none has the text), Up/Down, Home/End, Page Up/Down and the wheel move, Enter or a
  click picks, Esc clears the search, then closes the list. While a list is open every key is its
  own: no F, X, V or - and nothing for the game. Esc, V or a click elsewhere gives the keys back to the game.
  Each picker keeps its own choices for the session (not saved), and
  keep them across tabs: a tab with none of the chosen author's items shows its first tile only.
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
- The search's mod words (author, name and folder, folded) are looked up with `item_origins::find`
  once, when a grid's rows are copied, and kept with each item. A keystroke only looks in those
  strings. An item no mod adds or changes has none and is found as before. A grid copied before the
  merge's item file was read has none until the game builds the grid again.
- The pickers' filters use the same per-item mod (author, folder, name), kept with each item when
  the rows are copied; a grid's list of mods for Author and Mod pack is made then too, not per frame.
  Which picker a grid is comes from most of its items' categories (`picker_of`): the game's clothing
  categories are `cust_` and the slot (`cust_tops`, `cust_hats`, `cust_bottoms`, `cust_shoes`,
  `cust_socks`, `cust_fullbodycostume`...); tattoos (`tattoo_...`) and gestures (`gesture`, `emote`)
  are their own pickers, and the body's own `custhead`, `custbodies`, `cust_makeup`, hair, skin...
  are the appearance picker (outfit_kind and the worn items are unchanged: those are still not worn
  items there); what is none of them (`voice`, `locomotion`) has no filters; the log has "Item browser: picker ... for category ..." once per new list. The
  search field starts at the item card's left edge (45.25% of the menu frame, past the grid's
  scrollbar), at the height it always had, 350 wide at most. The filters follow it, the same gap
  apart as from each other, the same height and top, up to the menu's right less its margin:
  Author, Mod pack and Sort, then Show, as wide as its longest choice; where the screen is too
  narrow for both they give way down to 60%, then the search field down to 200. Their name and
  choice sit at the search field's icon's inset. An open list drops from its control's left edge,
  a gap below it, as wide (wider only for its search field), stops above the item card, and is
  drawn over the key prompts. The prompts are on one line under the search field from its left;
  V Filters is on it, and it shows the filters' own keys while they have them. The search field
  is drawn like the filters (tile, lighter under the mouse, blue edge while typing, magnifier at
  their inset, "Search" in their muted colour, the text as their values); the favorite star sits
  at its right end, as far in.
  While they are on screen the overlay reads where the mouse is and its left button, taking nothing
  from the game; a click on a row, or on the search field, opens it once let go (that click also
  reaches the game, over nothing of its own there), and the overlay then has the keys and the
  pointer, as for the search box, until Esc, V or a click elsewhere.
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
- The search, order, Author, Mod pack and Sort in item_browser_order.h are tested on their own
  (`dingosdk_item_browser_order_tests`, built with the plugin tests, Linux too). Searching by a mod's
  author or pack has not been tried in game yet. The pickers' filters were seen in game in the
  board, costume and clothing pickers, with the search box top left, the filters right of it, the
  prompts under it, searching inside the lists and Show's skate. and Mods. Clicking the search
  field and its new look have not been seen yet. The tattoos, gestures
  and appearance pickers' filters have not been seen yet (nor whether those screens have anything
  of their own where the filters go). A controller cannot reach
  them yet.
- `items peek` / `items poke` are research aids used to find the fields above; drop or keep as wanted.
