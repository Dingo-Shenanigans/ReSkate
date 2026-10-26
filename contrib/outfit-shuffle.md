# Shuffled outfits

Other players' clothes and boards show as random items, for you only. What they really wear is
never changed, sent or saved, and nobody else sees the difference. Built into ReSkate, like Road
Rash ([road-rash.md](road-rash.md)): nothing is hooked for it, and no file of the game or of a mod
is touched.

Code: [outfit_shuffle.h](../Extension/Multiplayer/Remote/outfit_shuffle.h) (the rules on their own,
with [tests](../Extension/Multiplayer/Test/outfit_shuffle_tests.cpp)),
[native_cosmetics.cpp](../Extension/Multiplayer/Remote/native_cosmetics.cpp) (reading the catalog and
putting the outfit on), [session_commands.cpp](../Extension/Multiplayer/Session/session_commands.cpp)
(`edit_shuffle_outfits`, `edit_shuffle_mods_only`), [hub_page.cpp](../Extension/UI/Overlay/hub_page.cpp) and
[multiplayer_menu.cpp](../Extension/UI/Overlay/multiplayer_menu.cpp) (the menus).

## What the player gets

One setting, **Shuffle outfits**, with three values:

| Value | What other players show |
| --- | --- |
| **Nothing** (default) | Their items as they wear them. An item you do not have installed (their cosmetics mod) shows the game's usual stand-in: a black tee, olive chinos, a plain deck, or nothing. |
| **Everything** | Every item they wear or ride is a random item that fits its slot, installed or not. |
| **Not installed** | Only the items you do not have installed are random, in place of the stand-in. Items you have show as worn. |

- A second setting, **Mod items only**, apart from the mode: the picks come only from items a mod
  installed, in whichever mode. Either can change without the other, but the menus grey it out,
  like Reroll shuffle, while the mode is Nothing (the console still takes it).
- It is in the ReSkate menu (Insert), **Online** tab, under OUTFITS: pressing the row steps Nothing,
  Everything, Not installed, and round. **Mod items only** and **Reroll shuffle** (new picks for
  everyone) are below it. It is also under Settings > Interface: a strip of three, the mod-items
  switch and a Reroll shuffle button.
- Console: `mp shuffle-outfits off|all|missing|cycle|reroll` and `mp shuffle-mods-only on|off|toggle`.
- The choices are saved with the profile (`ShuffleMode`: 0 nothing, 1 everything, 2 not installed;
  `ShuffleModsOnly`, a preference).
- Boards work the same way: deck graphic, grip, trucks and wheels. A board's seed is the player's,
  with a constant mixed in (`Shuffle::board`), so its picks do not follow their clothes'.
- A slot the player left empty stays empty, so nobody is handed a hat or a tattoo they did not wear.
  An item you lack that has no stand-in is never left empty, in either mode.
- A player keeps the same picks until you reroll. Changing the mode keeps them too, so going from
  Everything to Not installed and back gives the same items.
- Only the slots listed in `shuffled_slot_ids` (the skater's clothes, eyewear and tattoos, and the
  whole board) can be shuffled. Every other slot, including the skater's body, face and voice and
  any slot a mod adds, always shows as worn. `mp cosmetic-slots` lists every slot and marks the shuffled ones.

## How it works

- A peer's outfit arrives as a `CosmeticRecipe` and is put on their skater by
  `apply_cosmetic_recipe`. After the recipe is fitted to this game's slots, it notes which items
  are not installed here (`CosmeticMemory::item_installed`: the asset is in the catalog and fits the
  slot's category). Then the existing step swaps each of them for a stand-in.
- `shuffle_slots` says which slots the mode shuffles: with Not installed the lacking ones; with
  Everything those and every slot that holds an item. Either way, only slots listed in
  `shuffled_slot_ids` (outfit_shuffle.h, by the template slot ids `mp cosmetic-slots` prints); the
  rest show the item worn, or its stand-in, as with the shuffle off. `shuffle_recipe` gives each of them an installed
  item that fits and is not the one already there. The recipe the peer sent is never edited.
- Items reserved for their owners (`profile_runtime::reserved_cosmetic`) are still swapped for
  stand-ins as before, and are never picked. Under Everything their stand-in is shuffled too.
- The parameters the sender chose are kept: the slot, not the item, decides their layout.
- **The catalog** is every installed cosmetic, read from the game's cosmetics manager
  (`CosmeticMemory::catalog`). It is names and categories only, kept until the number of installed
  items changes (a mod applied live), so mod items you have are included. It is built as soon as
  the game's cosmetics manager is ready (`prepare_shuffle_catalog`, from the session tick, in a
  session or not), so the first shuffled player does not wait for it.
- **Mod items** are the catalog's items that are in no retail catalogue of the installed content
  cache (`content_cache::catalogs().items`, by lower-cased name), the same test as the item
  browser's Mods filter. The catalog is two pools that share no item (`ShufflePools`): `game`,
  the items in a retail catalogue, and `mods`, the rest. **Mod items only** draws from `mods`
  alone; otherwise a pick is one draw over both, so every item that fits has the same chance.
  With no content pack installed nothing can be judged modded, so the mod pool is empty and with
  **Mod items only** every slot keeps what it shows. A mod that replaces a retail item under its
  own name counts as the game's.
- **The pick** is seeded by the player's Steam ID and the session's shuffle epoch, and per slot, so
  one slot changing does not move the others. Only a reroll changes the epoch.
- `update_remote_cosmetics` compares the `Shuffle` (mode, seed and `mods_only`) it last used as well
  as the recipes, so changing the mode or the pool, or rerolling, puts every skater's outfit and board
  on again. The
  session bumps each peer's `cosmetic_revision` so those applies are spread over frames, one native
  pass per frame. An off shuffle is always the default one, so Nothing never re-applies.
- The menus read the mode from the view (`MultiplayerModel::outfit_shuffle`), which carries the
  `ShuffleMode` value, and `outfit_shuffle_mods_only`, and send the console words above.
- If anything fails (the catalog is not ready, or the result does not validate), the outfit is
  shown with the stand-ins and a warning is logged, three times at most.

## Looking at the slots: `mp cosmetic-slots`

A debug command that shows how the game dresses your own skater and board, one row per slot. It
reads only the local player and changes nothing, and needs a loaded map but no session
(`describe_cosmetic_slots` in native_cosmetics.cpp).

For the skater and then the board, a header:

```
Skater (recipe key 1234): 24 slots, 18 worn items
```

- **recipe key**: the template the outfit belongs to. The skater and the board each have one.
- **slots**: how many slots the template has, meaning places an item can go (a hat, a top, the
  deck, a tattoo).
- **worn items**: how many entries your current outfit has.

Then one row per slot:

```
  [ 3] slot 0xac4c6e13  category 0x1a2b3c4d  wearing "Mod_Hoodie_A"  fits 198 game, 14 mod  shuffled  from Cool Hoodies by someone
```

| Part | Meaning |
| --- | --- |
| `[ 3]` | The slot's position in the template. |
| `slot 0x…` | The slot's id, the same in every game. Outfits sent between players name their slots by it, and `shuffled_slot_ids` lists slots by it. |
| `category 0x…` | The kind of item the slot takes. An item goes in the slot only if this is one of its categories; the shuffle uses the same check to choose what can replace an item. |
| `wearing "…"` / `wearing nothing` | The item in the slot now, by its asset name, or nothing. |
| `fits N game, M mod` | How many installed items fit the slot's category, in each pool: the game's items and the mod items. A shuffle draws from both, or with **Mod items only** from the mod items alone. Items reserved for their owners are not counted. |
| `shuffled` | The shuffle can change this slot. A slot without it always shows what the player wears. |
| `from <mod>` / `changed by <mod>` | The mod the worn item comes from, or the mod that changes one of the game's own items (`item_origins::find`, the same lookup as the item cards). Nothing for an untouched game item, and nothing until ReSkate has merged mods at least once. |

What it helps with:

- **Why a slot never shuffles:** it lacks the `shuffled` mark (it is not in `shuffled_slot_ids`), or
  the two `fits` counts add up to 0 or 1 (with **Mod items only**, the mod count alone). With only
  one item that fits there is nothing else to pick.
- **Whether a mod's item fits a slot:** compare the slot's `category` with the item's categories.
- **Letting a slot shuffle:** add its id, from the `slot 0x…` column, to `shuffled_slot_ids`.
  Take one out to keep it from shuffling.

### Slot names

The command prints only ids, but a slot id is the game's hash of the slot's name: djb2 with xor
(`native_name_hash` in native_data.h), so `"truck"` gives `0x0add98be`. The hash is case-sensitive:
`tattoo_armL_1` is a slot and `tattoo_arml_1` is not. Every name below is a string found in the
merged game data (`Mods\.reskate`), decoded from its CAS archives and hashed to the slot's id
(2026-10-23). They are in template order, down the left column and then the right: the skater's
slots, then the board's from `board_gripcolor1` on.

| Id | Name | Id | Name |
| --- | --- | --- | --- |
| 0x9b15af3e | cust_fullBodyCostume | 0xa0ef5ee9 | cust_eyebrows |
| 0x7462c17f | cust_skintypes | 0xf8984dad | cust_outfits |
| 0xac3d4305 | cust_hats | 0x3ac1d7f4 | locomotion |
| 0xac4c6e13 | cust_tops | 0x348e55c7 | cust_eyebrowColor |
| 0xc06aa817 | cust_bottoms | 0x5034ae03 | cust_lipcolor |
| 0x35c5f429 | cust_shoes | 0xf958c570 | cust_maturity |
| 0xfbbab416 | cust_voicetypes | 0x17a31f28 | cust_freckles |
| 0x33e79eb0 | cust_heads | 0x00db8764 | cust_vitiligo |
| 0x3455758f | cust_noses | 0x3440dad3 | cust_moles |
| 0xac4493c1 | cust_eyes | 0xac4227e2 | cust_acne |
| 0xbc6996d3 | cust_mouths | 0xb5e61950 | cust_stubble |
| 0xc7bcb6fd | cust_bodies | 0xbc821c2c | cust_makeup |
| 0x6a8c54b4 | cust_scalpHair | 0x4f1c0009 | cust_lipstyle |
| 0x4e2cfe99 | cust_facialHair | 0xb4ec0b53 | tattoo_armL_1 |
| 0xede28ac4 | cust_hairColor | 0xb4ebb3cd | tattoo_armR_1 |
| 0xf18689e4 | cust_facialHairColor | 0xa70e1f88 | tattoo_chest_1 |
| 0xa39b4cef | cust_eyeColor | 0xa7ecb563 | tattoo_legL_1 |
| 0x6fd34bd5 | cust_face_accessory | 0xa7ecddfd | tattoo_legR_1 |
| 0xecc49d03 | cust_earwear_right | 0x755d5ee0 | board_gripcolor1 |
| 0x8a833f3f | cust_eyebrowwear_left | 0xc83bb94e | board_wheelcolor |
| 0xdba45e64 | cust_eyebrowwear_right | 0x66071628 | board_bottomart |
| 0xb5c4d799 | cust_nosewear_left | 0x976f18c4 | board_grippattern |
| 0x6e4482c2 | cust_nosewear_right | 0x0add98be | truck |
| 0xc6ca18f3 | cust_eyewear | 0x355284de | boardsticker0 |
| 0x3221be49 | cust_handwear | 0x355284df | boardsticker1 |
| 0xb488b276 | cust_handwear_right | 0x355284dc | boardsticker2 |
| 0x35caa14c | cust_socks | | |