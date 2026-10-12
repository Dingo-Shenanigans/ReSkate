# Style

Your own body movement on skate.'s trick animations. A style rotates the skater's joints on top of the
game's animation, while riding, grinding and off the board, and along each flip trick's timeline with
keyframes.

## What it does

- **The layer.** Every frame, your style's joint rotations are added to the local skater's pose. With
  **Show to other players** on, other players receive the styled pose as ordinary pose data. Off: only you
  see it.
- **Presets.** Each preset is one file in `%LOCALAPPDATA%\ReSkate\profiles\offline\styles\`. To share a
  preset, send that file. The preset in use is saved with the profile. A preset from a newer ReSkate is not
  read, and this version does not write over it.
- **The console.** `style joint` rotates a joint in a state or at a flip trick's keyframe; `style key` adds,
  moves and deletes keyframes and sets a keyframe's blend back to the game's pose. Undo and redo; a save after
  each edit, or on `style save`.
- **Multiplayer.** The layer and **Show to other players** work on a multiplayer server.

Off until you switch it on: skate.'s menu, **Mod Options > Player > Style**, or the console (`~`) with
`style 1`. The choice is saved with the profile. `commands style` lists the console commands.
The switch is greyed out when this game build or its data does not allow the layer; `style status` says why.

## How it works

| Part | Files |
|---|---|
| Start, client tick and the menu's model | `style.h`, `style.cpp` |
| What the menus read: names, targets and the model | `style_model.h` |
| Pose maths, the rig kinds and the presets' data; no game access | `style_pose.h` |
| Edits of a style: keyframes, rotations, blend-outs; no game access | `style_edits.*` |
| The layer: the settings, the edits' undo steps, the model and the client tick | `style_layer.*`, `style_internal.h` |
| The layer's shared state, and what the render thread runs | `style_live.h`, `style_render.cpp` |
| Reading and writing preset files | `style_file.*` |
| The presets folder: list, read, write, delete | `style_presets.*` |
| The skater's skeleton, read from the game's data | `style_skeleton.*` |
| The `style` console commands | `style_commands.*` |

Offsets and type hashes are in `Engine/Game/Build/20260929/style.h`. The layer
runs only while No Bail's physics state contract matches this game build; otherwise style stays
unavailable and says why. Each tick it also checks the skater component's and the bail core's vtables
before it reads the pose, and leaves the skater alone when they do not match.

Until it is first switched on, the layer reads no skeleton and adds no hooks; switched off, its hooks do
nothing.

Multiplayer: style sends no data of its own. In one test, a second player saw the style.

## Tests

```bash
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_STYLE_TESTS=ON -DDINGOSDK_TEST_GAME_ROOT=<Skate folder>
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R style_
```

`style_pose`, `style_edits`, `style_file` and `style_commands` need no game data. `style_skeleton` reads the skeleton from
the Skate folder. `core_platform` and `skinned_mesh` (multiplayer tests) also cover code the style uses.
