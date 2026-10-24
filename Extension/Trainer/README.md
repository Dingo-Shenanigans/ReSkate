# ReSkate Trainer

The SANDBOX page in the ReSkate menu (Insert): live physics tuning, tricklining, Skate 3 presets, practice markers and a
telemetry HUD. It ships no game data: the list of values is built at run time from the player's own
`Gameplay/SkatePhysicsTuning`.

## What it does

**Flip trick speed, for all tricks or one by one.** "Flip trick speed" (TUNE > POP AND TRICKS) makes every flip trick turn slower or faster without changing your pop; "Advanced trick speed" under it gives each of 16 tricks a speed of its own. Below 1 a flip is slow from the start and is not hurried round before you land. **Give it about 15 seconds after every level load**: the trainer has to find the game's flip animations again before the speeds take hold, and the card says so while it looks.

**Pump power.** "Pump power" (TUNE > PUMPING) sizes what pumping a transition gives: 1 is the game's own, 2 is about one more of the game's pumps on top of each of yours, 10 and up is a rocket, under 1 takes speed away. The game still decides when you are pumping. Like the flip speed it needs about 15 seconds after a level load.

| Tab | What you get |
|---|---|
| **TUNE** | One page of sections, each folding open and shut: PRESETS AND QUICK SETTINGS (every kind of preset in one card: Skate 3 on one of its difficulties, the grind and transition feels, the built-in styles, your own; and under it a list of everything that is changed right now, to change in place, hide, or jump to where it lives), POP AND TRICKS (gravity, ollie and trick heights, flip trick speed for all tricks or one by one, body flips and spins), SPEED AND PUSHING, BAILS AND LANDINGS, GRINDS (feel like skate. or Skate 3, exits and transfers, pops off a grind), TRICKLINING (the board bending boost with its rules, revert friction, Dark Pop, manuals), TRANSITION (coping grinds, coming back in, handplants, lip tricks, vert airs), PUMPING (pump power and the game's own pump settings), ON FOOT, and EVERY VALUE (the whole tuning table). A section that holds a change says so on its bar and has a reset of its own. One search box looks through all of it. |
| **PRACTICE** | Game speed and pause, five marker slots per map (save / go / clear), return to the marker after a bail, teleport to coordinates or to the waypoint you placed on the pause map (onto the surface there), copy your position (game or Blender axes). |
| **MAP & HUD** | Speed and air-time HUD, a read-out after every jump (takeoff speed and angle, height, distance, drop, landing speed, spin and flip), telemetry recording to CSV, and whatever the map's author ships for the trainer. |

ReSkate's Noclip, No Bail and boosts are on SKATER > CHEATS, with Revert Boost (Sivaes); Hall of Meat, Road Rash, Board Break and the
grind exits are on ADDONS.

The HUD, the jump read-out and the controller shortcuts are off until you switch them on (MAP & HUD,
PRACTICE): a player who never opens the trainer sees and feels nothing of it.

Open the menu with **Insert**, as in ReSkate (the trainer's own controller combo, LB + RB + click the right stick, is gone:
it is also how you bail). With the menu open the controller works in it. With the marker shortcuts switched on: hold **LB + RB**, then D-pad **up** saves the marker,
**down** goes to it, **left / right** pick the slot.

Everything is also a console command (`~`): `trainer open [tune|practice|map|movement|presets|game|pop|speed|bails|grinds|tricklining|transition|pumping|foot|everything]`, `trainer status`, `trainer set <id> <value>`,
`trainer find <words>`, `trainer preset apply|remove|export <name>`, `trainer preset import`, `trainer dial <multiplier> <preset name>`,
`trainer reset <id>|all|tricks|presets|everything|section <name>`, `trainer marker save|go|clear [slot]`,
`trainer tp <x> <y> <z>`, `trainer waypoint [info]`, `trainer ground <x> <z>`, `trainer where`, `trainer jumps`, `trainer dump`, `trainer selftest`.

## Quick settings

The first section of TUNE is **PRESETS AND QUICK SETTINGS**.

- **PRESETS** is every kind of preset in one card: the whole game (skate. or Skate 3 on one of its difficulties), the grind feel,
  the transition feel, Skate 3's tricklining extras, the built-in styles and your own saved presets. Blue is on; press again to
  take it off.
- **QUICK SETTINGS** lists everything that is changed right now, whatever section it lives in, grouped by section. Change a row
  right there. Pick a preset and the list fills with everything it sets.
- **Hide** takes a row off the list without changing it (it comes back by itself the next time it changes from the game's own,
  or with "Show hidden"). **Go to** opens the section the row lives in and scrolls to it, with the related settings around it.
  **Reset** puts that one row back to the game's own. The card's title folds the whole list away.

## Wallies and moves on foot

The game has switches of its own that it ships turned off. The trainer can turn them on (`trainer foot`, and rows in the menu for the measured ones):

- **Wallie: Wallies** (TUNE > POP AND TRICKS). Ride at a kerb, ledge or wall and pop just before it; the board climbs it. The rows beside it
  are the numbers of the game's wallie script: how early the pop may come, the lowest obstacle it looks at, the extra pop, the forward
  push as it climbs, the fastest speed it works at.
- **On foot: Double and triple jump** (TUNE > ON FOOT). Press jump again right as you land: 0.8 m, then 1.4 m, then 3.1 m.
- **Flips and spins on foot**: jump, hold LT or RT and push the left stick (forward or back flips, sideways spins). Roll: LT and the stick on the ground.
- **On foot: Jump on release**: the jump goes when you let go of the button. **On foot: Wipeout bounce**: a slam bounces and slides on.
- **Sprint speed** has no 10 m/s limit any more: the game's cut rises with the speed you set.

## Gravity

**Gravity** (TUNE > POP AND TRICKS) is gravity in the air, on the board and on foot, as a multiple of the game's own. Measured
with a scripted ollie: x 0.5 falls at 5.5 m/s2 and the same pop goes 0.70 m for 1.05 s (the game's: 0.36 m, 0.58 s); x 1.5 falls
at 15.8 m/s2. It acts in the air only: rolling downhill, grinds and a bailed skater keep the game's own. The game aims an air off
a ramp back at the ramp with its own gravity, so with another gravity a vert air can come down somewhere else.

## Stock, presets and your game's own settings

Above the tabs: **RESET EVERYTHING**, a badge, and your presets.

- The badge reads **STOCK** when nothing at all is changing how the game plays, and otherwise says what is: "MODIFIED: 14 values,
  grind settings, No Bail". Counted: every changed tuning value, the trick, grind, transition, pumping and bending settings, No
  Bail, Noclip, Dark Pop and the game speed. Not counted: Hall of Meat, the HUD, a locked value that is still the game's own.
- **RESET EVERYTHING** puts all of that back, clears every lock and stops loading a preset on this map. Your saved presets stay.
- **Presets**: set the game up, type a name, Save. A preset holds every changed value and every setting of the trainer's own; Load
  sets them on top of what is set now (press RESET EVERYTHING first for exactly the preset), Turn off puts them back to the game's
  own. Manage: share a preset as one line of text (`RST1:...`), import one, delete one, or load one by itself whenever this map
  loads. Not in a preset: locks, No Bail, Noclip, Dark Pop, the game speed.
- **Rows marked GAME SETTING are your game's own settings** (Pump assist, Pump strength, Forward to push): the same values the
  game's Settings menu shows. Changing one here changes it there and the other way round. They are yours, not the trainer's: no
  preset carries them, no reset touches them, and they never count as MODIFIED.
- **Rows that read "your game setting: on / off"** (the grind and transition settings) are also settings of your own game, but the
  trainer never changes them: ticking one *holds* it on or off for as long as the trainer says so ("held on", with yours shown
  beside it). Your own value is not written anywhere, the game's Settings screen keeps showing yours, and it is back the moment
  you press "Game's own", pick the skate. feel, press RESET EVERYTHING, join a host's enforced session, or quit.

## Presets to pass around

Two starting points between the game's own feel and Realistic. Copy a whole line (from `RST1:` to the end), then press
**Manage > Import from clipboard**; it is added to your presets and nothing changes until you load it. They are first guesses to
tune from, not measured targets.

MiddleGround
RST1:eyJuYW1lIjoiTWlkZGxlR3JvdW5kIiwicmVza2F0ZV90cmFpbmVyIjoxLCJ2YWx1ZXMiOnsicGh5c2ljc2FpcnN0YXRlcy5tYXhzcGluc3BlZWQiOjUyMC4wLCJwaHlzaWNzYm9keXNwaW4ubWF4ZGVsdGF2c3RpbWUgeCI6MC44NjY2NjY2NjY2NjY2NjY3LCJwaHlzaWNzYm9keXNwaW4ubWF4ZGVsdGF2c3RpbWVlYXN5IHgiOjAuODY2NjY2NjY2NjY2NjY2NywicGh5c2ljc2JvZHlzcGluLnBic3ZzdF9lYXN5IHgiOjAuODY2NjY2NjY2NjY2NjY2NywicGh5c2ljc2JvZHlzcGluLnByb3Bib2R5c3BpbnZzdGltZSB4IjowLjg2NjY2NjY2NjY2NjY2NjcsInBoeXNpY3NqdW1wLm1heGhlaWdodHZzc3BlZWQgeCI6MC44ODI1Mzk2NTU4MjA0NjMzLCJwaHlzaWNzanVtcC5taW5oZWlnaHR2c3NwZWVkIHgiOjAuOTAzNzAzNjg3NzQzNzIyMiwicGh5c2ljc21vZGUuanVtcG1heGhlaWdodCI6MS4zOSwicGh5c2ljc21vZGUuanVtcG1pbmhlaWdodCI6MS4yMiwicGh5c2ljc21vZGUubWF4YXV0b2JvZHlzcGluc3BlZWQiOjcuMCwicGh5c2ljc3JlY2tvbmluZy5mbGlwc2NhbGFyIjowLjksInRyaWNrLmZsaXBfc3BlZWQiOjAuODUwMDAwMDIzODQxODU3OX19

SimLite
RST1:eyJuYW1lIjoiU2ltTGl0ZSIsInJlc2thdGVfdHJhaW5lciI6MSwidmFsdWVzIjp7InBoeXNpY3NhaXJzdGF0ZXMubWF4c3BpbnNwZWVkIjo0ODAuMCwicGh5c2ljc2JvZHlzcGluLm1heGRlbHRhdnN0aW1lIHgiOjAuOCwicGh5c2ljc2JvZHlzcGluLm1heGRlbHRhdnN0aW1lZWFzeSB4IjowLjgsInBoeXNpY3Nib2R5c3Bpbi5wYnN2c3RfZWFzeSB4IjowLjgsInBoeXNpY3Nib2R5c3Bpbi5wcm9wYm9keXNwaW52c3RpbWUgeCI6MC44LCJwaHlzaWNzanVtcC5tYXhoZWlnaHR2c3NwZWVkIHgiOjAuNzk5OTk5OTc1Nzc5NzAwNiwicGh5c2ljc2p1bXAubWluaGVpZ2h0dnNzcGVlZCB4IjowLjg1MTg1MTgzNjgwNzYwNywicGh5c2ljc21vZGUuanVtcG1heGhlaWdodCI6MS4yNiwicGh5c2ljc21vZGUuanVtcG1pbmhlaWdodCI6MS4xNSwicGh5c2ljc21vZGUubWF4YXV0b2JvZHlzcGluc3BlZWQiOjYuNCwicGh5c2ljc21vZGUuc3BlZWR3b2JibGVzdGFydHNwZWVkIjoxMS4wLCJwaHlzaWNzcmVja29uaW5nLmZsaXBzY2FsYXIiOjAuOCwidHJpY2suZmxpcF9zcGVlZCI6MC43NX19

## For map makers: `trainer.json`

Put a `trainer.json` in your mod folder (beside `manifest.json`). Stock ReSkate ignores it; with the
trainer, players get your spots and your recommended tuning on the MAP & HUD tab.

```json
{
  "schema": 1,
  "note": "Built for about 60 km/h off the first lip.",
  "preset": {
    "name": "Gravy Train",
    "values": { "PhysicsPush.MaxPushableSpeed": 12.0 }
  },
  "spots": [
    { "name": "Start deck", "position": [0.0, 790.0, 0.0] },
    { "name": "Jump 3", "position": [0.0, 700.0, 310.0] }
  ]
}
```

`"level"` (a level asset) is optional: without it the file stands for every level the mod's
`reskate-levels.json` adds. Value ids are the ones `trainer dump` lists.

## Multiplayer

The trainer goes through ReSkate's own session rules instead of around them:

- While a session's host sets everyone's physics (the session's "enforce tuning", on by default),
  a guest cannot edit anything here; the page says so. They skate with the host's whole setup: its
  tuning through ReSkate's host-tuning sync, and its class values, trick multipliers and auto push
  through the session's physics extras (`trainer_session.h`), sent whenever the host changes one
  and to players who join later. On a dedicated server all of it is the game's own.
- With that switched off, everyone's physics are their own, and the trick multipliers and auto push
  follow the host's boosts permission like ReSkate's other boosts.
- Teleports and markers follow the host's noclip / teleport permission.
- Game speed is ReSkate's `SimulationTime.TimeScale` setting, which ReSkate locks in a session.
- Servers' `score_check` / `enforce_tuning` see a player's tuning changes like any other tuning mod.

It unlocks no cosmetics or entitlements.

## How it works

- `physics_tuning_model` keeps the field names the game's EBX carries, so `trainer.cpp` can build its
  table from `read_game_tuning()`.
- Edits go into a target copy of the asset; `physics_tuning::write_live()` writes the differences and
  refreshes the local skater's cached block. Values are re-applied after a level load.
- The game thread owns all state. The menu only queues `trainer ...` console commands and reads two
  snapshots (`trainer_view.cpp`).
- Jumps are measured from the skater's position each client tick: the game's own physics state says when the skater is in the air.
  Wipeouts come from the same state, which the no-bail hook already sees.
- The pause map's waypoint is a point of interest in the map's registry (the one ReSkate's party
  markers use, `addr::native_party::map_manager`), kind `DingoMapPOIType_Waypoint` (3). The registry
  is walked only while the TELEPORT card is on screen or `trainer waypoint` runs (`trainer_waypoint.cpp`).
- A waypoint's height is not the ground. `trainer waypoint` and `trainer ground` cast the park
  editor's downward ray through the client physics world and land on the topmost surface; if
  collision there is still streaming in, a short watch puts the skater back on it once it arrives.
  The decisions are in `trainer_landing.h`, tested by `dingosdk_trainer_landing_tests`.
- Settings, presets, markers: `%LOCALAPPDATA%\ReSkate\trainer\trainer.json`.

## Checking a build

```
RESKATE_STARTUP_COMMANDS="load <level asset>;wait 25;trainer selftest"
ReSkateLauncher.exe --no-gui --no-update
```

then read the `trainer selftest:` lines in `logs\ReSkate.log`. `dingosdk_trainer_tuning_dump <Skate
folder>` (CMake option `DINGOSDK_BUILD_TRAINER_TESTS`) lists the tuning values without the game;
the same option builds the `trainer_session_extras`, `trainer_landing` and `trainer_presets` tests (`ctest -R trainer`).

## Known limits

- The jump read-out uses real time, so it reads low while game speed is not 1x.
- Masses and collision sizes (deck, trucks, wheels) only change on the next respawn.
- Curve and graph multipliers scale outputs only; a curve whose point count a mod changed keeps the
  mod's points.
- Not every tuning value is used by the game: about 4 in 10 rows have no code reading them
  (`trainer_used.inc`, found by a static pass over the game's code for this build). Ollie height
  comes from the `PhysicsJump` height graphs (the `PhysicsMode` jump heights are never read, so the
  trainer links them to those graphs: `value_links` in `trainer_presets.cpp`), body
  flips from `PhysicsReckoning.FlipScalar` and `FlipMaxSpeed` (`PerfectBodyFlips` forces exactly one
  rotation and ignores them), body spins from the `PhysicsBodyspin` graphs. "No use found" is not
  proof: the pass can miss a use.
- No comply, boneless and hippy jump heights are not tuning values: the game's trick scripts launch
  those. The trainer multiplies the launch speed where the game sets the jump's trajectory (no
  comply, boneless: `trainer_jump.cpp`) or scales the skater's upward speed as the jump starts
  (hippy jump).
- Much of the game's tuning is not in `Gameplay/SkatePhysicsTuning` but in data-defined classes
  (push speeds, everything on foot, dive and glide, bail speeds...). A live copy has no name to
  look up, so the trainer finds it by searching writable memory for the class's defaults laid
  out as the class lays them out (`trainer_classes.cpp`; the table in `trainer_classes.inc` is
  generated from the game's own data). The search reads all of the game's writable memory, so it
  only runs for a player with a use for it: one of those values or the flip speed is not the
  game's own (theirs, or a host's they skate with), or the EVERY VALUE section is open. Then it runs
  on its own thread a few seconds after a level loads, and at most three times per level;
  `trainer classes` says what it found. Native code keeps its own copy of the push speeds, found
  and written the same way.
- "Push speed" scales the push class's speeds (a tapped push, a held one, the top) and the
  tuning's top pushing speed, which only gates whether a push may start. Auto push is the trainer's doing as well (the game's flag only reaches its animation): once rolling and not braking, the skater gains speed up to the auto push speed. Push strength has no effect in
  this game build and is hidden.
- Built for one game build (the one ReSkate 1.0.3 supports). A game update needs a new build.
