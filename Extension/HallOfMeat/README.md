# Hall of Meat

skate. 3's Hall of Meat for skate.: when you bail, the bones you hurt show through your skater,
yellow where a hit bruised one and red where one broke it, with the bail's Meat card in the bottom
left corner. It ships no game data: the skeleton (the Dem Bones costume's mesh), the card's icons, the
THRASHER wordmark and the brush strokes are read from the installed game.

## What it does

- **The skeleton.** From the first hurt bone until you are back on your feet, then it fades out. A fall
  that hurts nothing shows nothing. A fresh hit flashes the bone it hit.
- **The card.** It takes the place of skate.'s own HUD in that corner (the d-pad and the score HUD come
  back after). Time, hits (head hits count double, hits from vehicles half again), broken bones, road
  rash, airtime, fall, top speed and rotations, each with its points, then the Meat. Small stats join
  the card once they are worth showing. Each map keeps its best Meat (**NEW BEST** / **BEST 12,345**).
- **A break.** The screen's edges pulse red and, in single player only, the game slows down for a
  moment. The slow motion never runs in a multiplayer session. It is the same at every break, but
  never faster than the trainer's game speed (PRACTICE), which it gives back after.

Off until you switch it on: skate.'s menu, **Mod Options > Player > Hall of Meat**, or the console (`~`)
with `hallofmeat 0|1`. The choice and each map's best are saved with the profile.

With No Bail on, a wipeout No Bail stops is not a bail, so nothing shows.

## How it works

| Part | Files |
|---|---|
| Start, client tick, the switch and each map's best | `hall_of_meat.h`, `hall_of_meat.cpp` |
| Each physics step of the local skater, handed on by No Bail's skeleton response hook (`Extension/Skater/no_bail.h`) while the feature is on: what every ragdoll body hit and how hard, and whether it is a ragdoll | `hall_of_meat_skater.*` |
| The bail's phases (riding, bailing, down, getting up) and its Meat, on the game's clock; no game access | `hall_of_meat_model.*` |
| The card's rows | `hall_of_meat_card.*` |
| skate.'s skeleton mesh, read from the game's data and posed with the renderer's skinning matrices | `hall_of_meat_skeleton.*`, `hall_of_meat_render.*` |
| Hiding skate.'s bottom left HUD while the card shows | `hall_of_meat_hud.*` |
| The slow motion: SimulationTime's rate, frame cap and time scale as a session override, only where slower than the speed it found and writing that back after, the local skater's step along with them | `hall_of_meat_slow_motion.*` |
| Drawing, on the overlay's background draw list | `hall_of_meat_overlay.*` |
| The `hallofmeat` console switch | `hall_of_meat_commands.cpp` |

Every address, offset and fingerprint is in `Engine/Game/Build/20260929/` (`skater_body.h`,
`skater_state.h`, `skater_render.h`, `skater_skeleton.h`, `hud_corner.h`, `ui_model.h`,
`ui_textures.h`), and each is checked before use. Without a matching game build the feature stays
unavailable and the menu's switch is greyed out.

Switched off, it costs the game nothing while it plays: the renderer's two hooks and the UI model's
write hook are prepared at startup but only switched on with the feature (and off again once the HUD
corner and the game's speed are given back), and No Bail hands no physics step on. The skeleton mesh
is read the first time it is switched on. Only the images are read at startup, in the background:
the overlay builds its atlas once, when its graphics start.

A finished bail logs what it scored at `--log-level=debug`.

## Tests

```bash
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_HALL_OF_MEAT_TESTS=ON -DDINGOSDK_TEST_GAME_ROOT=<Skate folder>
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R hall_of_meat
```

`hall_of_meat` covers the bail's phases, the Meat and the card. `hall_of_meat_skeleton` reads and poses
the skeleton mesh from a real Skate folder.

The skater's render hooks, its skeleton and its body maps were first found by
[gBGYo's ReSkate fork](https://github.com/gBGYo/ReSkate).
