# Road Rash

Bails leave marks on the local skater that build up over a session: road dust and a graze first,
then scrapes and bruises, then raw wounds, a bruised face and (if the player leaves it on) blood.
Built into ReSkate; the pictures themselves come from a content mod.

Code: [road_rash.cpp](../Extension/RoadRash/road_rash.cpp) (client thread),
[road_rash_model.h](../Extension/RoadRash/road_rash_model.h) (the rules on their own, with
[tests](../Extension/RoadRash/Test/road_rash_tests.cpp)),
[skater_items.h](../Extension/RoadRash/skater_items.h) (reading and changing the skater's recipe),
[road_rash_commands.cpp](../Extension/RoadRash/road_rash_commands.cpp) (console).

## What the player gets

- After 10 bails the leg of the side that took more of them shows the first marks, its arm one bail
  later; at 14 they are bigger and the other side starts; at 20 they are at their worst and the face
  is bruised, at 24 the face is worse too. A mark never gets smaller.
- `roadrash 0|1`, `roadrashblood 0|1` (the worst wounds bleed, or are dry and scabbed),
  `roadrashtattoos 0|1` (wounds may take the place of the player's own tattoos and moles),
  `roadrash heal`, and `roadrash bails <count>` to look at a stage. The first two and the healing
  are in the menus beside Hall of Meat (Mod Options > Player). The choices are saved with the profile.
- Without the item pack installed nothing shows and nothing is changed.

## How it works

- **A bail** is the physics state selector choosing Wipeout (`watched_physics_state`, no_bail.h).
- **Its side** comes from the ragdoll's contacts, read with Hall of Meat's reader
  (`hall_of_meat::read_step`) on every client tick while the body is a ragdoll: the impact and the
  slide of each arm and leg body that touches something other than the skater's own board are summed
  per side. The side with three fifths of it or more took the fall; otherwise it counts half for
  each. Read outside the physics step a contact can be half written; the sums are only compared.
- **The marks** are cosmetic items: tattoo items for the limbs (the skater has one tattoo slot per
  arm and leg) and moles items for the face. Road Rash puts them into the local skater's recipe
  with the game's own recipe copy, the call the multiplayer code makes for peers, after the same
  validation. The game rebuilds the skater in about one frame.
- **Where a limb's decal sits** is the slot's five parameters (x, y, size, turn, unused). The
  numbers for knee and elbow are in road_rash_model.h: x and y are a position in the limb's
  rectangle of the body mesh's second UV set, the right limb mirrors x.
- **Nothing is saved** to the outfit. The game builds the skater from the profile at every level
  change; the marks are put back from the session's count. A slot that holds the player's own
  tattoo or moles is left alone unless `roadrashtattoos` is on, and what was covered comes back
  with healing or switching off.
- **The outfit menus.** The game feeds a skater's items either from the player's outfit or from a
  recipe of the skater's own, never both. After the recipe copy the local skater no longer gets
  what the player changes in the menus: the new top shows on the menu's skater and is saved, and
  the skater in the world keeps the old one. So Road Rash gives the skater back to the game while
  a game menu is up (`sample_game_ui_state`, game_ui_state.h), and also with nothing to show, on
  healing, when a switch changes and when switched off. Giving back (`hand_back`, skater_items.h)
  writes the four words of the item component the copy had changed back to what they were and
  sets the component's "feed changed" byte; at its next update the game puts the outfit's own
  items back and feeds the skater again. The marks go back on, over the outfit as it is by then, a
  second after the menu has gone. A skater whose four words do not read as "follows its outfit"
  before the first marks never gets a recipe.
- **Respawns.** After a bail the game hands over what looks like a new skater and is the same one,
  items and all. So whose recipe a skater wears is read from the skater at every look (the four
  words, and whether Road Rash items are in the recipe), not remembered from before.
- **Hashes, no names.** The recipe handed to the copy names no item, like the game's own for the
  local skater: the game changes an item of that skater where it lies (hash and parameters only),
  and a name left beside it would no longer match. That made every later read of the skater's
  recipe fail, the multiplayer code's too.
- **Parameters come back rounded.** The game works a slot's parameters out again when it updates
  the skater; they are compared with a tolerance, not bit for bit.
- Off, it does nothing. On and without marks, a tick costs one physics-state read.

## The item pack

A content mod (no code) with items named `RoadRash_<Set>_[<design>_]<level>[_Dry|_Blood]`:

| Set | Kind of item | Levels |
| --- | --- | --- |
| `Leg`, `LegR` (the left leg's pictures mirrored, for the right leg) | tattoo | 1, 2, 3_Dry, 3_Blood |
| `Arm` | tattoo | 1, 2, 3_Dry, 3_Blood |
| `Face` | moles | 1, 2_Dry, 2_Blood |

A design letter (`B_`, `C_`) after the set names a second and third look; each limb draws one per
session. A missing design falls back to the first, a missing `LegR` to `Leg`, a missing level to
the one below. Any pack that follows the names works.

## Seen working in game

Offline, on main at b7936a3 with a 45 item pack: every stage through `roadrash bails`, the blood
switch, healing, switching off and on, and real bails counted with their side read from the
contacts (falls on the left and on the right both came out as such), the marks growing in place
through respawns. With marks on: a top changed in the apparel menu and a switch to another skater
preset both show on the skater in the world, with the marks back over them; no wound item ends up
in a saved outfit. A female and a male body.

## Not tried yet

- Multiplayer: what peers see, with and without the pack; dedicated servers.
- The online route: all of the above ran on the offline profile.
- A game menu that keeps the skater in view (the marks are off while any game menu is up), and
  anything outside the menus that changes the outfit while marks are on.
- A level change in the middle of a bail.
- No marks for the torso or the neck: the chest tattoo slot is not measured.
