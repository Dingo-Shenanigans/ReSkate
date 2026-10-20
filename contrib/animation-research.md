# Grind animation research

Investigation dated 2026-10-19. Source baseline: `27a3880`. Target game SHA-256:
`fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9`.
This is an evidence map and a local prototype, not a claim of full grind editing support.

## What the editor actually controls

`anim_studio.cpp` captures body and board render poses; physics can overwrite an earlier animation
buffer write, so overrides are published at rendering. `body_joints()` limits in-game replacement
to the Hips descendants, excluding placement and the board's skater-rig bones. The UI exposes 25
editable joints, including Board, rather than arbitrary control of all 395 skeleton joints.
`anim_core.cpp` also holds effector positions. Root motion, locomotion, collision, rail constraints,
foot placement and graph transitions are different systems; exposing their bones would not
automatically expose or safely replace those systems.

The recorder is built around takeoff and landing, with 0.7 seconds before/after. Saved playback
requires a nonnegative takeoff. Selection occurs in the first 0.35 seconds of airtime, retaining
the current selection while the game temporarily reports no name or no matching preset.
During a grind the current fallback extends the airborne episode, holds the clip near landing,
and disables the custom board target so the native rail motion survives. A direct grind entry
and an exit pop have not been designed as independently selectable custom episodes. This can
explain problematic transitions, but captures are needed to attribute any specific visual fault.

## Verified static game assets

Inspected read-only with `reskate_cli ebx-values <game-root> <asset>`:

| Asset under `animation/dingo/` | Observed data | What is not established |
| --- | --- | --- |
| `onboard.grinding.node` | `StateFlowNodeControllerAsset`; 10 direct transitions, 75 flattened transitions; subject `superlayer_onboard_grind` | Live instance layout, active branch and weights |
| `superlayer_onboard_grind` | `SuperLayersControllerAsset`, base controller plus an extra enabled layer | The live contributions of the layers |
| `grind.lockin.node`, `grind.loop.node` | Separate state-flow nodes; 3 and 2 direct transitions respectively | Runtime entry/exit events and normalized phase |
| `enum.skategrindtype` | 78 enum entries, including Invalid; e.g. BS_Feeble=27, FS_50_50=3 | Where current values are stored in the running game |
| `enumgs.cas.current.grindtrick` | `GameStateEnumerationAsset` referencing that enum | Its layout metadata is not a live memory offset |
| `transition_onboard.trickinair.node_to_onboard.grinding.node` | `TransitionAsset`, `InertializationBlendAsset`, raw BlendTime=8, branch-window asset | Units and runtime overrides of BlendTime |
| `transition_onboard.grinding.node_to_onboard.inair.conduit1` | Transition conditioned on `bool.anim.isgrindexittoair`, inertialization raw BlendTime=20 | Which of the several grind-to-air branches is used on each exit |
| `c_proto_onb_grind_5050_rail_loop` | Looping `ClipControllerAsset`, FPS=60, delta trajectory enabled | How native rail/IK corrections alter its final pose |

Asset names also distinguish `rail_into`, `rail_loop`, `rail_popout_fs/bs`, `_inair`, `_land`,
fakie variants, and no-pop exits. These are evidence of separate authored phases, not proof that
one clip family alone reproduces the live result. Do not interpret any asset `Entry.Offset=0` as
an address in the live skater, nor equate physics IDs (401 etc.) with `SkateGrindType` values.

The pre-existing `Engine/Game/Build/20260929/grind_exit.h` reports earlier measurements: 401
through most grinds, brief 402/403/404 states, and 401->200 for pop exits. Those are inherited
measurements, not new captures from this investigation, and cover a limited set of scenarios.

## Measurements to do next

1. Capture the same route with Custom tricks in game OFF, then ON: flat ollie, kickflip,
   ollie into 50-50, kickflip into 50-50, boardslide, pop out, ride off without pop, transfer,
   bail, and grind-to-manual. Repeat regular/goofy, switch/fakie, both approach directions,
   a flat rail, sloped rail, and ledge. Label each capture with the exact build and test.
2. Correlate state/name changes with video. Check whether a grind name ever replaces the
   airborne trick-name string and whether leaving a rail starts a new episode. The current
   trace cannot identify the live grind enum or graph phase.
3. Resolve live game-state enumeration storage from the controller's table/layout machinery;
   verify `CAS.Current.GrindTrick` against several known grinds. Add build-pinned, guarded
   read-only accessors only after validating the layout and failure paths.
4. Resolve active controller/clip, normalized time, branch windows and inertialization weights.
   Correlate native feet/body/board poses at animation and render publication. If an engine
   tick misses brief states, extend measurement at an already verified physics-step hook.
5. Design separate entry, constrained loop, exit-pop, exit-flight and landing stages. Retain
   rail/contact control until a deliberate physics-editing design exists. Define body/board
   masks, phase synchronization, reachable foot contacts and blend-out behavior per stage.

Acceptance before enabling grind overrides: correct native trick scoring and rail trajectory,
feet stay in contact, no board penetration, no one-frame pose jumps on entry/exit or transfer,
predictable bail recovery, and no stale override after the local skater/entity changes. Validate
local and remote rendering separately. A useful first feature is editing the body over the
native constrained grind; arbitrary rail motion is a separate, later feature.

## Ground gesture inspection after upstream integration

The upstream batch recorder decodes ground flip gesture EBX files and generates stick scripts
instead of fabricating board motion. It excludes `fromgrind` and `underflip` variants. This is
input recognition data, not the active animation graph or a complete movement inventory.

With `DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS=ON`, Windows builds can run:

```
dingosdk_trick_gesture_tests <game-root> <output.json>
```

This reads the installed data using the same decoder as the batch recorder. It exports each
gesture's enum, nollie flag, hold bounds, ordered arcs, and generated low/mid/high stick scripts.
It checks finite arc fields, magnitude bounds, unique enum IDs, ordered ticks, bounded scripts
and neutral release. It sends no gameplay input. Enum/asset identity must still be correlated
with actual recorded trick strings and animation phase captures; a valid script does not prove
that the game scored the intended trick or pop height. `ctest -R animation_gestures` runs the
synthetic boundary cases without game data.

The isolated installation yielded 36 ground gestures, split 18 ollie / 18 nollie. The gesture
suffixes include `n_heelflip` (also observed in a live diagnostic), while the static clip export
table currently calls that slot `nollieheelflip`. Several other nollie/impossible slots use
different spellings too. Batch metadata preserves actual runtime names; a complete catalog
binding migration still needs verified enum-to-runtime-name evidence, rather than assuming
every gesture asset suffix is identical to the runtime string.
