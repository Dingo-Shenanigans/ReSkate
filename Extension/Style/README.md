# Style

Your own body movement on skate.'s trick animations. A style rotates the skater's joints on top of the
game's animation, while riding, grinding and off the board, and along each flip trick's timeline with
keyframes.
The style editor shows a trick on a stand-in skater and lets you pose each keyframe.

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
- **The editor.** A keyframe timeline for each flip trick, with the trick played on a stand-in skater in the game menu's
  character room. The editor needs that room: if the room is gone for more than 4 s, the editor closes. The editor
  records your tricks first, once, in solo play: your skater does each trick on a practice stage (Esc or Back stops
  after the current trick). Slower playback. The recorded clips are in `style-clips\`, next to `styles\`.
- **Multiplayer.** The layer and **Show to other players** work on a multiplayer server. The editor works there too,
  with the clips you already have: your tricks are recorded in solo play. The stand-in is a skater on
  your game only, in the last player slot, so the editor does not open once 240 or more player slots are in use.

Off until you switch it on: skate.'s menu, **Mod Options > Player > Style**, or the console (`~`) with
`style 1`. The choice is saved with the profile. `commands style` lists the console commands.
The STYLE page and the Style editor row show while it is on.
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
| The editor's start, its part of each client tick and of the menu's model | `style_editor_entry.cpp` |
| The editor's calls into the layer, and its part of the render thread: the recording and the menu room | `style_layer_editor.*`, `style_render_editor.cpp` |
| The recording's format; no game access | `style_recording.*` |
| The editor: the stand-in and the timeline's state | `style_editor.*` |
| The editor camera's framing | `style_camera.h` |
| Which tricks a batch records, and its texts; no game access | `style_batch.h` |
| The menu room: the game menu's character room, where the editor shows its stand-in | `menu_room.*` |
| The trick stage and the recorded clips, in one batch, for a capture | `style_capture.*`, `style_clips.*`, `trick_stage.*`, `trick_recorder.*` |
| A scripted pad and the flip tricks' stick gestures, to do a trick on the stage | `trick_pad.*`, `trick_gestures.*` |
| The overlay's part: the STYLE page, the editor screen, the playhead feed | `style_overlay.h`, `style_overlay.cpp` |
| The editor's console commands | `style_editor_commands.cpp` |

Offsets and type hashes are in `Engine/Game/Build/20260929/style.h`. The layer
runs only while No Bail's physics state contract matches this game build; otherwise style stays
unavailable and says why. Each tick it also checks the skater component's and the bail core's vtables
before it reads the pose, and leaves the skater alone when they do not match.
The menu room's measured places and the editor camera's angle are in the same file.

Until it is first switched on, the layer reads no skeleton and adds no hooks; switched off, its hooks do
nothing.

The editor's camera and HUD takeover is in `Extension/Skater/client_debug.cpp`, next to the park editor's. The debug camera
reaches the editor through `Extension/Skater/editor_screen.h`, which the feature registers at start.

Multiplayer: style sends no data of its own. In one test, a second player saw the style.

## Tests

```bash
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_STYLE_TESTS=ON -DDINGOSDK_TEST_GAME_ROOT=<Skate folder>
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R "style_|trick_gestures"
```

`style_pose`, `style_edits`, `style_file` and `style_commands` need no game data. `style_skeleton` reads the skeleton from
the Skate folder. `core_platform` and `skinned_mesh` (multiplayer tests) also cover code the style uses.

The editor's tests: `style_clips`, `style_recording`, `style_batch` and `style_editor` need no game data.
`trick_gestures` reads the flip tricks' stick gestures from the Skate folder. `held_settings` (multiplayer tests) and
`park_editor_transactions` (park editor tests) cover code the editor uses.
