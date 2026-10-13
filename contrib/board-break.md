# Board Break

A hard slam can break the local skater's board: at the nose, in the middle or at the tail. Half the
time the grip tape holds the pieces together and the board folds or an end hangs; otherwise a piece
comes off, flies and lies where it lands, truck and wheels with it. The board is whole again once
the skater has picked it up, rides again or respawns. Built into ReSkate; the broken pieces
themselves come from a content mod with the deck shapes in it.

Code: [board_break.cpp](../Extension/BoardBreak/board_break.cpp) (client tick and the boards'
render pose hook), [board_break_model.h](../Extension/BoardBreak/board_break_model.h) (the rules and
the geometry on their own, with [tests](../Extension/BoardBreak/Test/board_break_tests.cpp)),
[board_break_commands.cpp](../Extension/BoardBreak/board_break_commands.cpp) (console), and the
deck's place in the mod list in [mod_catalog.cpp](../Engine/Vfs/mod_catalog.cpp).

## What the player gets

- A slam of force 10 or more always breaks the board, a lighter one a quarter of the time, rising
  in between (the force is the bail cause's that led to the wipeout). Now and then a board gives way
  while it is ridden: the nose goes.
- `boardbreak 0|1`, `boardbreak deck [name|number|next]`, `boardbreak snap [nose|mid|tail]` to look
  at a break (that one stays until `boardbreak mend` or a respawn). The switch and the deck are in
  the menus beside Road Rash (Mod Options > Player, and the overlay's Skater page). The overlay has
  a card BOARD BREAK DECK with a button for each deck, on the Mods page and under the switch: a
  press asks for that deck, the mods are applied and the level loads again with it. The switch is
  saved with the profile.
- A break plays back in the replay editor as it happened.
- Without a deck pack installed nothing breaks and nothing is changed.

## How it works

- **One deck model.** Every board in the game draws the same deck mesh
  (`characters/skateboard/unlicensed/deck/generic/popsicle/2022/deck_gen_popsicle_mesh`); deck
  graphics and grip tapes are items on top of it, and an item cannot bring a mesh of its own. So a
  breakable deck is a content mod that replaces that mesh: the same deck, plus four stretches of it
  (nose, front, back, tail) cut along three rows of vertices the deck already has, each rigidly
  skinned to a bone of the board's rig that the game does not use (Trajectory, Connect, Ground, and
  Connect2 for the tail, which hangs on Connect).
- **Only bones move.** `board_render_pose` (the hook multiplayer already has for other players'
  boards) runs after a board's pose has been handed to the renderer, which is the one moment a
  change to the pose is still drawn and not yet rewritten: the board's animation writes every bone
  again every frame. A broken board gets its Deck bone moved 2 km down, the trucks (which hang on
  it) moved back by as much, and each stretch moved to where its piece lies. The piece that came
  off is flown by the hook itself, from the board's place at the break: a kick, gravity, the
  ground, and written relative to wherever the board is in each frame.
- **Nothing is scaled.** The first version shrank the deck and scaled the stretches up. The game's
  replay records where a bone is and how it is turned, and not its size: a shrunk deck played back
  as a small whole board, and stretches that were scaled up not at all. Moves play back exactly.
- **Whole boards hide their stretches too.** The deck is a hollow shell (the wood has no top face,
  the grip is a sheet over it), and the game draws wood and grip as layers that do not hide what
  lies just under them: stretches left inside a whole deck showed as cracks through the grip,
  fainter the deeper they lay and never gone (tried down to 4 mm). So every frame, on every board
  that passes the hook (the local one, other players'), the stretches of a whole board are moved
  away like a broken board's deck. The call into Board Break is the last thing the hook does, after
  another player's pose has been written, so that pose's bones do not bring them back.
- **Which deck is in the game** is a question for the mod list, because any mod that reshapes the
  deck replaces the same mesh and the one higher in the order has it. When the mods are put
  together, the Board Break deck the player chose goes first, and what would fight it is set
  aside for the run: the other Board Break decks, and other mods whose bundle lists name the
  deck's bundle. Those are left out whole (with the breakable deck in place of its own, a wide-deck
  mod has its trucks and wheels on a deck they were not made for). mods.json is not changed; the
  mod list shows them as SET ASIDE.
- **Another deck without a restart.** The deck mesh is in the items superbundle, which a live apply
  of the mods already refreshes, and the game reads the mesh when a level loads. Choosing a deck
  saves the choice, applies the mods and loads the level again (live_mods.h).
- **The choice** is a line in `%LOCALAPPDATA%\ReSkate\BoardBreak.deck`, not a profile preference:
  the mods are put together before the profile is there.
- **The sound** is a wave file from the `sounds` folder of an installed deck pack (subfolders
  `light`, `medium`, `heavy`, by the size of the break), played straight to the speakers.
- **The slam's force** comes through No Bail's existing hooks: the cause it records for the watched
  skater, and the cause collector at the moment the selector chooses Wipeout. Nothing new is hooked.
- **The trucks' baseplates.** The game's truck model (one for every truck item,
  `characters/skateboard/unlicensed/truck/generic/default/2022/truck_gen_default_mesh`) has its
  hangers on the trucks' bones and everything bolted to the deck, the baseplates and their bolts,
  on the deck's bone. With the deck's bone moved away to hide the real deck, a broken board had
  hangers and wheels and no baseplates. The decks inside a deck pack bring a copy of the truck
  model with those vertices on the trucks' effectors instead (`eff_truckBack`, `eff_truckFront`:
  a bone under each truck's that no model uses), and the hook writes each effector from its
  truck's bone as the animation left it, so that the truck's bone followed by the effector's is
  what the two are at rest: on a whole board the baseplate sits on the deck however the truck
  steers, and a truck that a piece carries off takes its baseplate along
  (`baseplate_bone`, with a test). That is done on every board that passes the hook. With the
  game's own truck model nothing is on the effectors and writing them shows nothing.
- **On foot the grip tape always holds.** A loose piece takes the ground from where the board
  rolled or lies; a board in the skater's hand does not tell where the ground is, and a piece that
  came off there hung in the air at hand height.
- Off, the board holds. Without a deck pack in the game the hook returns at once.

## The deck packs

A deck is a content mod's copy of the deck model (no code), and comes in one of two ways:

- a mod folder named `Board_Break_<Name>` holds the deck `<name>`;
- a deck pack, a mod folder of any name, holds a deck in each `decks/<name>/`, laid out like a mod
  folder (`layout.toc`, `Win32/...`). The launcher installs one mod folder per package, so this is
  how one package brings every deck. When the mods are put together each deck inside becomes a mod
  of its own for the run, listed as `<folder>#<name>`: it is chosen, set aside and switched like a
  deck in a folder of its own (its archives are placed like a disabled mod's). mods.json and the
  mod list only know the pack, which counts as loaded when one of its decks is. The launcher takes
  a folder for a mod only when it has a `layout.toc` of its own. That layout is not one of the
  pack's decks, whatever the folder is named, and is set aside like any other deck model; it is
  what an older ReSkate loads from the pack. ReSkate 2.0.6 knows a deck by its folder's name only,
  so a pack in a folder named `Board_Break_<Name>` (the package `Board_Break_Pack`, installed as
  `Team-Board_Break_Pack`) with a breakable deck as its own layout breaks boards there with that
  one deck, and brings all its decks from this version on. The truck model with the baseplates on
  the effectors belongs in the decks inside the pack only: a ReSkate that does not place those
  bones would draw the baseplates turning with the hangers, and such a version reads the pack's
  own layout, never `decks/`.

The name is what the deck goes by. The eight the feature was made with are standard, wide,
concave, powell, slim, egg, square and shaped, and the console also knows those by number. Any
other deck that follows the recipe works, with no change to ReSkate:

- Start from the deck mesh as ReSkate Studio exports it (three parts on the bone Deck: the grip
  sheet, the wood, and a plain copy of the whole deck). Studio only takes back a mesh whose
  triangles are the donor's own, so a shape is made by moving vertices, never by adding any. Keep
  every vertex where it is along the board: the break rows (z = 0.260, 0.009 and -0.260 m) and the
  truck bolts stay put.
- For each of the four stretches, append a copy of every part's vertices and triangles, skinned to
  that stretch's bone, with every vertex on the far side of its break rows folded onto the row's
  cross-section (top skin onto its top line, bottom skin onto its bottom line). That leaves the
  deck's own edge faces standing in the gap as the broken end. Push the row itself along the board
  by a jagged line, the same line for both sides of a break, so the pieces fit together.
- Build the stretches at their own size, in the place they have in the whole deck.
- The truck model for a deck inside a pack: the game's own, with every vertex that is on the bone
  Deck moved to `eff_truckBack` (behind the middle of the board) or `eff_truckFront` (ahead of
  it). Nothing else changes; Studio takes it back as it is.
- The sounds are `sounds/light|medium|heavy/*.wav` in the folder of any mod that brings a deck.

## Seen working in game

Offline, on main at f4292d6, with eight decks installed, each in a folder of its own and all of
them in one deck pack: breaks from real slams at the nose,
in the middle and at the tail, clean and held by the grip, the board whole again in hand and on
respawn; breaks from the console on foot (folded, nose and tail hanging) that stay until mended.
The grip side of a whole board, close up in the board menu, shows no cracks. A deck chosen while
the game runs is the one in the board menu and in the world after the level has loaded again
(about 20 s), and the one the next launch starts with. With a wide-deck mod switched on and first
in the order, the breakable deck is still the one in the game and the mod is set aside; so is an
older deck mod of Board Break's own. The mod list shows the decks that are set aside as such, with
nothing waiting to be applied. With a pack first in the order (on main at 1b15a04, 2.0.6), the
deck the game starts with and the deck chosen while it runs both come from inside the pack, the
deck list has the pack's eight decks and no more, and the pack's own layout is set aside: tried
with the game's own deck there, and with a breakable deck there in a folder named
`FourthStudio-Board_Break_Pack`. That last pack on 2.0.6 as released: its own layout is the deck
"pack", loads first and breaks at the nose, in the middle and at the tail. The switch on the
overlay's Skater page turns it off and on. On the
older build the feature was developed on, a middle break played back in the replay editor.

On main at 8351654, with the pack as the only source of decks (its decks with the truck model
that has the baseplates on the effectors): a dozen breaks from real slams at the nose, in the
middle and at the tail, clean and held by the grip, with the baseplates on the pieces, and on the
whole board in hand and on the ground. The effectors are bones 5 and 9 of the pose, each at
0.0395 m along its truck's bone and not turned, as the rig has them. A press on Shaped in the
overlay's deck card: the mods applied in 3.8 s, the level loaded again in 15 s with that deck,
which then broke. In the game's Mod Options > Player page, on 2.0.6: one press on Board Break
deck is one deck further and one reload.

## Not tried yet

- Multiplayer: other players' boards are only handled by the code path (their stretches are moved
  away), not seen; what peers see of a broken board, with and without the packs.
- The online route: all of the above ran on the offline profile.
- The replay editor on this branch (the replay was checked on the older build the feature was
  developed on).
- The deck card with a deck that is not one of the eight, and with none installed (it draws
  nothing then).
- A board that does not pass the render pose hook, with the truck model from a pack: its
  baseplates would follow the hangers.
- A mod that brings a truck model of its own beside a deck pack: the pack's is the one that loads.
- A level change in the middle of a break.
