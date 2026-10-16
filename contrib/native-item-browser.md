# Favorites, search, filters, own tabs and items worn on top in the skater item grids

Built into ReSkate in the same way the music screen's Mods shelf was added
([native-music-ui.md](native-music-ui.md)): the grids stay the game's own and their data models are
changed through the native model functions. Nothing is hooked for it and no file of the game or of
a mod is touched.

Code: [item_browser.cpp](../Extension/Customization/item_browser.cpp) (client thread),
[item_browser_order.h](../Extension/Customization/item_browser_order.h) (the pure ordering rule),
[item_browser_overlay.cpp](../Extension/UI/Overlay/item_browser_overlay.cpp) (search bar, prompts, keys, the
player's tab row and its panel), [tab_icons.cpp](../Extension/UI/Overlay/tab_icons.cpp) (the tabs' icons),
[outfit_layers.cpp](../Extension/OutfitLayers/outfit_layers.cpp) (items worn on top).

## What the player gets

- **F** (left stick click on a pad): the highlighted item becomes a favorite, or stops being one.
  Favorites sit right after the worn item and carry a star where the tile shows its rarity icon.
- **-** or **Ctrl+F**: a search box; every letter filters the grid at once. Enter keeps the search,
  Esc clears it.
- **X**: steps a filter (All, Favorites, Mods, Game items).
- Favorites are saved to `ReSkate.favorites.json` beside Skate.exe (`RESKATE_FAVORITES_FILE` overrides
  the path), keyed by the item's asset name, so items added later just work.

### The player's own tabs

- **Tab** (View on a pad): the grid's tab row becomes the player's own tabs ("Jewelry", "Gloves",
  "Random"...), drawn in the game's place and style on a panel of its own; Tab again gives the game's
  tabs back. With no tabs yet it opens the tab editor.
- **Q / E** (LB / RB) step through them, nine to a page ("1 / 2" under E). The grid then shows that
  tab's items from every grid of the screen (tops, bottoms and shoes together).
- **V** (hold View): a panel to put the highlighted item into tabs, or take it out; "+" makes a tab.
- **R**: the tab editor: name, icon, order, delete. Icons are the game's own category icons, read from
  the installed game at start-up (`ui/textures/icons/cas/img_cas_<kind>_128_128_128`, decoded like the
  park previews), and 26 of ReSkate's own for kinds the game has none for (`assets/tab_icons`, drawn by
  `make_tab_icons.py`, built into ReSkate.dll).
- Tabs are saved to `ReSkate.collections.json` beside Skate.exe (`RESKATE_COLLECTIONS_FILE` overrides).
- While the player's tabs are shown, Q, E and Enter do not reach the game (Tab never does over an item
  grid): `set_withheld_keys` drops their presses from the game's raw input and key-state reads.

### Items worn on top

- **B** (R3; Enter or A in the player's tabs): the highlighted item is worn on top of the outfit, or
  taken off again; **Delete** (hold R3) takes them all off. Several hats, chains, gloves... at once.
  Their tiles carry the game's brushed check where the rarity icon is.
- The skater's recipe has one slot per kind and the game only builds an item from a slot of its own
  category. An item worn on top goes into a slot the outfit leaves empty (its own kind's, else the
  accessory slots, then the tattoo and face slots), and the catalog item is given that slot's
  category besides its own, in memory, for this run. The world skater then wears the outfit and the
  layers in a recipe of its own through the native recipe copy, as Road Rash puts its marks on; while a
  game menu is up it is the game's again, so the menus change the outfit, and the layers go back on
  0.15 s after the menu closes.
- Not every item loads from every slot (a top from the left hand's slot leaves the skater without a
  body). An item never tried in a slot goes on alone; if the game describes a failed load within 0.9 s
  (`load_error_guard::described()`), it comes off and the next slot is tried. What loads where is kept
  in `ReSkate.layer_slots.txt` beside the tabs file, so it is not tried again.
- They wait while Road Rash has marks on the skater (both hand the skater a recipe of their own).

### Controller

The prompts follow what was used last (a key, or a pad's button; a pad that connects counts), drawn as
Xbox letters or PlayStation shapes. Buttons held for an action fill a ring while held. Over an item
grid the game does not see the stick clicks and View, nor LB, RB and A while the player's tabs are
shown (`set_withheld_pad_buttons`, applied in the XInput filter). `RESKATE_ITEM_PAD_PROMPTS=1` shows
the pad's prompts without a pad, for screenshots.

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

Own tabs and items worn on top (keyboard and an Xbox pad, offline, 3440x1440): making, naming,
re-iconing, moving and deleting tabs, pages of nine, a tab mixing items from Tops and Shoes, five hats
worn at once, layers kept through level reloads and menus, an item that does not load in a slot moved
to the next, the check on worn tiles while scrolling.

## Not tried yet

- A PlayStation pad read natively (not through XInput / Steam Input): its buttons are not withheld from
  the game.
- The menus' preview skater does not show items worn on top: it is not the world skater and was not
  found among the skater item components; they show in the world.
- Other players do not see items worn on top (their game holds items to their slots' categories).
- Two items in exactly the same place (two leggings) flicker between one another, as any overlapping
  meshes do.
- Tattoos (the same grid type is expected; untested).
- Multiplayer sessions, level changes with a grid open, live mod apply.
- Other screen sizes for the overlay's placement.
- The star replaces the rarity icon (bottom left). The tile has no icon slot bottom right.
- Tests: `dingosdk_item_browser_tests` (`DINGOSDK_BUILD_ITEM_BROWSER_TESTS`) for the ordering, the
  collection filter and the tab lookup.
- `items peek` / `items poke` are research aids used to find the fields above; drop or keep as wanted.
