# ReSkate Trainer

A TRAINER page in the ReSkate menu (Insert): live physics tuning, presets, practice markers and a
telemetry HUD. It ships no game data: the list of values is built at run time from the player's own
`Gameplay/SkatePhysicsTuning`.

## What it does

| Tab | What you get |
|---|---|
| **TUNE** | Three lists: REALISTIC, FUN and EVERYTHING. Each opens on the dials: every built-in preset as one slider (Ollie height, Push speed, Body flip and spin speed, Bail resistance, Grind lock-on and friction, on-foot jump and sprint, glide, torpedo), where 1 is the game's own and the preset's button jumps to the preset; then the switches (Auto Push, No Speed Wobble, Smooth Surfaces, Long Wheelbase, Never bail), the trick sliders (flip trick speed, no comply, boneless, hippy and off-board jump height) and the values themselves: a short plainly named list, or under EVERYTHING the whole table (the physics tuning's values, one multiplier per curve and graph, and the 314 values of the game's other tuning classes) with search, groups, "only what I changed", locks, your own saved presets and a preset a map applies every time it loads. Changes apply while you skate. Values the game was never found or seen reading are hidden unless you ask for them. **Reset everything**, above the tabs, puts the game back as it shipped. |
| **PRACTICE** | Game speed and pause, five marker slots per map (save / go / clear), return to the marker after a bail, teleport to coordinates or to the waypoint you placed on the pause map (onto the surface there), copy your position (game or Blender axes). |
| **MAP & HUD** | Speed and air-time HUD, a read-out after every jump (takeoff speed and angle, height, distance, drop, landing speed, spin and flip), telemetry recording to CSV, and whatever the map's author ships for the trainer. |

The HUD, the jump read-out and the controller shortcuts are off until you switch them on (MAP & HUD,
PRACTICE): a player who never opens the trainer sees and feels nothing of it.

Controller, once switched on: hold **LB + RB**, then D-pad **up** saves the marker, **down** goes to it,
**left / right** pick the slot.

Everything is also a console command (`~`): `trainer open [tune|practice|map|realistic|fun|everything]`, `trainer status`, `trainer set <id> <value>`,
`trainer find <words>`, `trainer preset apply|remove <name>`, `trainer dial <multiplier> <preset name>`,
`trainer reset <id>|all|tricks|presets|everything`, `trainer marker save|go|clear [slot]`,
`trainer tp <x> <y> <z>`, `trainer waypoint [info]`, `trainer ground <x> <z>`, `trainer where`, `trainer jumps`, `trainer dump`, `trainer selftest`.

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
the same option builds the `trainer_session_extras` and `trainer_landing` tests (`ctest -R trainer`).

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
  game's own (theirs, or a host's they skate with), or the EVERYTHING list is open. Then it runs
  on its own thread a few seconds after a level loads, and at most three times per level;
  `trainer classes` says what it found. Native code keeps its own copy of the push speeds, found
  and written the same way.
- "Push speed" scales the push class's speeds (a tapped push, a held one, the top) and the
  tuning's top pushing speed, which only gates whether a push may start. Auto push is the trainer's doing as well (the game's flag only reaches its animation): once rolling and not braking, the skater gains speed up to the auto push speed. Push strength has no effect in
  this game build and is hidden.
- Built for one game build (the one ReSkate 1.0.3 supports). A game update needs a new build.
