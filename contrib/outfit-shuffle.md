# Shuffled outfits

Other players' clothes and boards show as random items, for you only. What they really wear is
never changed, sent or saved, and nobody else sees the difference. Built into ReSkate, like Road
Rash ([road-rash.md](road-rash.md)): nothing is hooked for it, and no file of the game or of a mod
is touched.

Code: [outfit_shuffle.h](../Extension/Multiplayer/Remote/outfit_shuffle.h) (the rules on their own,
with [tests](../Extension/Multiplayer/Test/outfit_shuffle_tests.cpp)),
[native_cosmetics.cpp](../Extension/Multiplayer/Remote/native_cosmetics.cpp) (reading the catalog and
putting the outfit on), [session_commands.cpp](../Extension/Multiplayer/Session/session_commands.cpp)
(`edit_shuffle_outfits`), [hub_page.cpp](../Extension/UI/Overlay/hub_page.cpp) and
[multiplayer_menu.cpp](../Extension/UI/Overlay/multiplayer_menu.cpp) (the menus).

## What the player gets

One setting, **Shuffle outfits**, with three values:

| Value | What other players show |
| --- | --- |
| **Nothing** (default) | Their items as they wear them. An item you do not have installed (their cosmetics mod) shows the game's usual stand-in: a black tee, olive chinos, a plain deck, or nothing. |
| **Everything** | Every item they wear or ride is a random item that fits its slot, installed or not. |
| **Not installed** | Only the items you do not have installed are random, in place of the stand-in. Items you have show as worn. |

- It is in the ReSkate menu (Insert), **Online** tab, under OUTFITS: pressing the row steps Nothing,
  Everything, Not installed, and round. **Reroll shuffle** below it deals everyone new picks. It is
  also under Settings > Interface, as a strip of three.
- Console: `mp shuffle-outfits off|all|missing|cycle|reroll`.
- The choice is saved with the profile (`ShuffleMode`: 0 nothing, 1 everything, 2 not installed).
- Boards work the same way: deck graphic, grip, trucks and wheels. A board's seed is the player's,
  with a constant mixed in (`Shuffle::board`), so its picks do not follow their clothes'.
- A slot the player left empty stays empty, so nobody is handed a hat or a tattoo they did not wear.
  An item you lack that has no stand-in is never left empty, in either mode.
- A player keeps the same picks until you reroll. Changing the mode keeps them too, so going from
  Everything to Not installed and back gives the same items.
- Near the game's texture limit, at 80% of the streamed textures it can hold, the shuffle picks only
  items the game has already loaded for a skater, so it brings in no new textures. Chat says so once.
  Players already dressed keep their picks; modes and rerolls still work, from the smaller pool.
- Only the slots listed in `shuffled_slot_ids` are ever shuffled (clothes, tattoos and the board's
  parts); any other slot shows as worn. `mp slots` lists every slot and marks the shuffled ones.

## How it works

- A peer's outfit arrives as a `CosmeticRecipe` and is put on their skater by
  `apply_cosmetic_recipe`. After the recipe is fitted to this game's slots, it notes which items
  are not installed here (`CosmeticMemory::item_installed`: the asset is in the catalog and fits the
  slot's category). Then the existing step swaps each of them for a stand-in.
- `shuffle_slots` says which slots the mode shuffles: with Not installed the lacking ones; with
  Everything those and every slot that holds an item. Either way, only slots listed in
  `shuffled_slot_ids` (outfit_shuffle.h, by the template slot ids `mp slots` prints); any other slot
  shows the item worn, or its stand-in, as with the shuffle off. `shuffle_recipe` gives each of them an installed
  item that fits and is not the one already there. The recipe the peer sent is never edited.
- Items reserved for their owners (`profile_runtime::reserved_cosmetic`) are still swapped for
  stand-ins as before, and are never picked. Under Everything their stand-in is shuffled too.
- The parameters the sender chose are kept: the slot, not the item, decides their layout.
- **The catalog** is every installed cosmetic, read from the game's cosmetics manager
  (`CosmeticMemory::catalog`). It is names and categories only, kept until the number of installed
  items changes (a mod applied live), so mod items you have are included.
- **The pick** is seeded by the player's Steam ID and the session's shuffle epoch, and per slot, so
  one slot changing does not move the others. Only a reroll changes the epoch.
- `update_remote_cosmetics` compares the `Shuffle` (mode and seed) it last used as well as the
  recipes, so changing the mode or rerolling puts every skater's outfit and board on again. The
  session bumps each peer's `cosmetic_revision` so those applies are spread over frames, one native
  pass per frame. An off shuffle is always the default one, so Nothing never re-applies.
- The menus read the mode from the view (`MultiplayerModel::outfit_shuffle`), which carries the
  `ShuffleMode` value, and send the console words above.
- **Room for textures.** Every item picked can bring a bundle of its own, and the game freezes
  when it needs one streamed texture more than it can hold (`texture_handle_limit`). The game does
  not let an item's textures go once loaded, even when nobody wears it any more, so every item put
  on a skater stays loaded. `apply_cosmetic_recipe` notes each one as it is put on, shuffled or not
  (`note_loaded`), and the local player's own. Past `shuffle_texture_ceiling` (80%) the catalog is
  cut to those (`ShuffleCatalog::only`) before `shuffle_recipe`. The list starts again in a new
  level (`note_cosmetics_level`, the local skater's context): whether a level load lets them go is
  not known, and a smaller pool is the safe side. Nobody is re-dressed when the line is crossed, so
  nothing flips around it; a slot with nothing loaded that fits keeps what it holds.
  `check_shuffle_room` only tells the player, once, and again after use falls under 70%.
- If anything fails (the catalog is not ready, or the result does not validate), the outfit is
  shown with the stand-ins and a warning is logged, three times at most.

## Not tried yet

- In game: nothing here has been built or run against `Skate.exe`, and the menu rows have not been
  drawn. The pure rules, the mode words and the console handler are tested on Linux; the catalog
  walk, the apply path and the menus reuse the existing code's patterns but were not compiled.
- Whether a costume or full-body outfit slot gives odd results under Everything, when another item
  takes its place.
- Whether board parts that belong together (a grip cutout made for one deck shape, say) can end up
  mismatched. The only check is the game's own: an item must fit its slot's category.
