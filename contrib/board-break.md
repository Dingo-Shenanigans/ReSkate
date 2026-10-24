# Board Break

A hard slam can break the local skater's board: at the nose, in the middle or at the tail. Half the
time the grip tape holds the pieces together and the board folds or an end hangs; otherwise a piece
comes off, flies and lies where it lands, truck and wheels with it. The board is whole again once
the skater has picked it up, rides again or respawns. A plugin, [board-break](plugins/board-break)
(it was built into ReSkate until 2026-10), that ships inside the Board Break deck pack: the broken
pieces themselves are the pack's deck models.

Boards also wear: every hard landing, rail hit, grind and slam takes something out of the board of
the deck in the game, and a worn board steers lazier, wobbles, breaks from less and in the end can
give way under the rider. A boardslide or darkslide that comes down too hard onto a rail can snap
the board and throw the rider. A break cracks (and sounds like metal when it happened on metal),
and the body's hits in a bail are heard where and as hard as they land. The player sets all of it.

Code: the plugin's Lua in [plugins/board-break/plugin](plugins/board-break/plugin): the break,
wear, rail snaps, sounds, the deck, its console command and its page. It draws through
`reskate.board` (set, hide, park and toss the board's bones; `fit` every board for the deck in
the game), and picks the deck with `reskate.package.variants` and `reskate.package.choose`: the
pack's decks are variants of its mod, which its `reskate-plugin.json` declares. Nothing in ReSkate
knows Board Break.

## What the player gets

- A slam of force 10 or more always breaks the board, a lighter one a quarter of the time, rising
  in between (the force is the bail cause's that led to the wipeout). Now and then a board gives way
  while it is ridden: the nose goes.
- `boardbreak 0|1`, `boardbreak snap [nose|mid|tail]` to look at a break (that one stays until
  `boardbreak mend` or a respawn). Its settings are on its page in the overlay's PLUGINS section.
  `boardbreak deck [name|number|next]` and the DECK card on that page pick the deck: a press asks
  for that deck, the mods are applied and the level loads again with it.
- A break plays back in the replay editor as it happened.
- **Wear and tear.** Each deck's board wears from 0% (new) to 100% (worn out): a landing coming down
  faster than 3.5 m/s, a hit onto a rail or ledge (harder on a slide than on the trucks), every second
  of grinding or sliding, and a slam of force 4 or more. The menus call it new, used, worn, beat or
  done. From a fifth worn the board answers the steering less and its trucks go soft and twist
  further; worn out, it takes half the force to break and can give way under the rider. A board
  that broke is a new one once it is whole again; a worn one is renewed with the button New board
  or `boardbreak worn 0`. The scratches are the game's own board wear (Mod Options > Board wear),
  cleared with every new board.
- **Rail snaps.** A boardslide, lipslide or darkslide that comes down onto a rail or ledge at 3 m/s
  or more (2.2 on the grip) can snap the board in the middle, certainly 3 m/s above that, and the
  rider goes down with it. Grinds on the trucks never do.
- **Sounds.** A crack by the size of the break, a metal one on top when the board broke on metal,
  and the body's hits in a bail, louder the harder they land.
- **Settings**, on the plugin's page in the overlay's PLUGINS section (with how worn the board is):
  always break (for replay clips), rail snaps, wear and tear, break sounds, body hit sounds, break
  sensitivity, how many breaks the grip tape holds, wear speed, and New board. The console has them
  all: `boardbreak always|sensitivity|joined|rails|wear|wearrate|cracks|bodysounds [value]`,
  `boardbreak worn [percent]`, `boardbreak status`. They are kept, with the board's wear, in the
  plugin's storage.
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
- **Which deck is in the game.** Any mod that reshapes the deck replaces the same mesh, so the
  pack's decks are variants of its mod: one is in the game at a time, the one the player chose
  (else the first of the manifest's `order`). The manifest names the deck's bundle in `displaces`,
  so any other mod that brings a deck model is set aside while a deck is in the game (with the
  breakable deck in place of its own, a wide-deck mod has its trucks and wheels on a deck they
  were not made for). The mod list shows those as SET ASIDE.
- **Another deck without a restart.** `reskate.package.choose(deck)` keeps the choice, applies the
  mods and loads the level again: the deck mesh is in the items superbundle, which a live apply
  refreshes, and the game reads it when a level loads.
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
- **Every physics step of the local skater** (a step listener of No Bail's) is where wear, rail
  snaps and the body's hits are seen. How fast the board came down is its root body's speed in its
  last step in the air; a landing is that step followed by rolling (physics states 1xx), a hit onto
  a rail by grinding (4xx). What kind of contact it is comes from the board's axes, from the client
  tick, against how it moves (`contact_of`): across its way it slides, on its grip it is a darkslide,
  else it grinds on its trucks.
- **What wear does to the handling** is four values of the game's own physics tuning
  (`PhysicsSteering.GeneralScalar`, `PhysicsTrucks.StaticTruckSpringStrength`, `StaticTruckDamping`
  and `TruckTwistLimit`), each the game's value times a factor of the wear. A value the player
  changed (the trainer) is left alone, and nothing is changed in a multiplayer session, whose
  players share their tuning.
- **A rail snap throws the rider** through No Bail (`no_bail_request_wipeout`): the cause goes in
  just before the game's selector picks the next state, the way the game's own contact tests
  record theirs, and the pick becomes Wipeout. The force is kept low (2.5): the game reads a cause's
  force as how hard the rider was hit, and a slam's (10 and more) with the board flat on a rail
  threw the rider high into the air.
- **Metal.** Each contact effect the game plays (sparks, dust, landings) names a material of the
  level (Extension/Multiplayer/Remote/native_vfx.h, `recent_local_impacts`); Board Break installs
  that hook once in a world, as multiplayer would. The level's materials have no names that can be
  read, so which are metal is a list: marked in play with `boardbreak metal mark` while grinding a
  metal rail, and brought by a pack in `sounds/metal/materials.txt`.
- **The body's hits** are the ragdoll's contacts that Hall of Meat reads each step, one sound at a
  time, none below a graze.
- **The sounds** play through XAudio2 on a thread of their own, several at once and each at its own
  loudness; 16-bit and 32-bit float wave files both play.
- **Wider decks, wider trucks.** The game puts each wheel on its truck's bone along the axle, about
  0.0876 m out. A deck that says so (deck.ini `axle`) has its wheels set further apart, or closer:
  the plugin reads it and asks `reskate.board.fit{plates = true, axle = ...}`, which sets them in
  the render pose hook (set, not scaled, so a pose that comes by twice is not moved twice, and only
  what is drawn changes) and holds the baseplates on the deck, on every board. The deck's own truck
  model is made to match.
- Off, the board holds. With no deck in the game the plugin fits nothing.

## The deck pack

A deck is a copy of the deck model (no code). The Board Break deck pack holds a deck in each
`decks/<name>/`, laid out like a mod folder (`layout.toc`, `Win32/...`), and the plugin beside
them (`reskate-plugin.json`, `plugin/`). The plugin's manifest declares the decks:

```json
"variants": {
    "folder": "decks",
    "order": ["standard", "wide", "concave", "powell", "slim", "egg", "square", "shaped", "oldschool", "pig"],
    "displaces": ["deck_gen_popsicle_cas_main_bundlereftable"]
}
```

The pack's own `layout.toc` is set aside while one of its decks is in the game; it is what an older
ReSkate loads from the pack. The truck model with the baseplates on the effectors belongs in the
decks inside the pack only: a ReSkate that does not place those bones would draw the baseplates
turning with the hangers, and such a version reads the pack's own layout, never `decks/`.

The name is what the deck goes by (its folder's name: letters, digits, `_` and `-`). The ten the
feature was made with are in the manifest's `order`, which is also how the console numbers them.
Any other deck folder that follows the recipe works, with no change to ReSkate or the plugin:

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
- The sounds are `sounds/light|medium|heavy|metal|body/*.wav` in the folder of any mod that brings
  a deck (a crack size without sounds of its own takes the nearest that has some), and
  `sounds/metal/materials.txt` may list the level's materials that are metal, by number.
- A deck may say something of itself in a `deck.ini` in its folder (`decks/<name>/deck.ini`):
  `label = Old School`, its name in the menus, and `axle = 1.17`, how far apart its wheels sit
  against a standard board's (0.8 to 1.6), for a deck wider or narrower than the standard one.

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

Wear, rail snaps, sounds and settings, in the player's own game offline, on the first build of
them (on main at 6c45e96): boardslides that came down onto rails at 4.6 to 7.0 m/s snapped the
board in the middle; the settings card switched always break on; boards wore, at that time far
too fast (the rates are a third of that now).

## Not tried yet

- Multiplayer: other players' boards are only handled by the code path (their stretches are moved
  away), not seen; what peers see of a broken board, with and without the packs.
- The online route: all of the above ran on the offline profile.
- The replay editor on this branch (the replay was checked on the older build the feature was
  developed on).
- The plugin's DECK card and `boardbreak deck` (2026-10-20, built and tested without the game):
  choosing a deck, a deck that is not one of the ten, and a pack with no decks.
- A board that does not pass the render pose hook, with the truck model from a pack: its
  baseplates would follow the hangers.
- A mod that brings a truck model of its own beside a deck pack: the pack's is the one that loads.
- A level change in the middle of a break.
- The rider going down with a board that snapped on a rail, and the material a board broke on: both
  changed after the build that was played. No material is marked as metal yet, so the metal sound
  waits for `boardbreak metal mark` or a pack's list.
- The body's hit sounds in a real bail.
- Wear's effect on the handling, felt in play, and a worn-out board giving way.
- The Old School and Pig decks, and wheels set further apart by `axle`, in play.
- This branch as it is, on main at 6df4f9a: built without warnings and its tests run, not played.
