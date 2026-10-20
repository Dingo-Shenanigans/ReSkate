# F8 board intersection investigation

Reference case: native library **360 Flip / high**,
`animation/dingo/c_proto_onb_trick_o_360flip_high`.

## Confirmed publication defect corrected

F8 previously published body and board under separate mutexes. A render callback could
observe a new body with an old board (or conversely), both accepted for up to 100 ms.
They now travel in one immutable preview packet, pinned by the first body/board callback
for each profiler presentation. Producer updates between callbacks cannot replace either
half. Invalidation releases the pinned packet through an epoch. This is a presentation
cohort, not a discovered native animation evaluation ID.

The regression fixture advances a board and foot together by 70 cm between producer
samples. It checks body-first, board-first, repeated publication, invalidation and a missing
first sample. Mixing the two samples would lose their 10 cm separation.

## Geometric problem remains under investigation

Original 360 flip high exports as 121 frames at 60 Hz. Evaluating the exported hierarchy
at frame 0 puts the left/right foot origins at Y = -0.140829 / -0.176181 metres in
`Skateboard_Root` space. At frame 60 those values are -0.042541 / -0.041859 metres.
These are raw FK coordinates, not final native poses or measured mesh intersections.
The source root-to-physical-board mount correspondence still needs validation.

The source root is under AITrajectory. F8 currently targets the physical board's mount
(bone 2), moving its parent root (bone 1). BoardBreak's deck model has Y up, Z along
the deck and a mount rotation; this alone does not establish the source/physical pivot
offset. Do not introduce another guessed rotation/translation to mask this discrepancy.

Existing preview board-to-foot correction also changes board Y while flat around inferred
takeoff/landing, then fades over 60 ms. Raw native previews do not run the game's foot/deck
IK, physics targets and procedural controller stack. Synchronization alone therefore
cannot be presented as a complete geometric fix.

## Diagnostic capture

The ordinary Export diagnostics button now includes an independent `preview/` history.
Opening F8 still preserves the preceding gameplay history. The preview history records:

- Loaded source pose and source board before relocation, with clip time and source kind.
- Desired pose and board; body/board render publications after writes, with each packet
  sequence and presentation token. Their callbacks retain separate timestamps.
- `board-rig.csv`: all 16 physical board bones, parent-local, before the current override.
  This can include writes surviving from previous publications; it is not a pristine
  native baseline. Bone 2's local mounting transform can be inspected independently.
- `loaded-source.clip` and `loaded-edits.json` for the currently loaded preset. The source
  is the loaded clip, including any existing loader sanitization, not the original EBX.

To collect the reference case, open 360 Flip high in F8, play or scrub through the
intersection, then export while it remains selected. Inspect matching sequence/presentation
tokens before comparing rendered body and board coordinates. A text export does not establish
GPU appearance or native constraint execution.

Release compilation, CTest and synthetic diagnostic export are required. In-game visual
validation and the remaining constraint/pivot diagnosis are pending.

## First live preview export

`dump-110530015-0` contains 900 preview callbacks, 136 full poses and the requested
360 flip high source. The native-asset portion contains 685 callbacks; the user scrubbed
from time 0 through 0.733333 s and held at 0.466667 s. Body/board publication sequences
match in all 900 rows. Five presentation tokens differ because the diagnostic callback
can read between the two render callbacks; their packet sequence still matches.

The physical mount's local transform is a +90 degree Y rotation and translation
(-0.0000532, 0.0856126, -0.0000335) metres. The published body/board relative geometry
agrees with the desired geometry, including the held frame. Source and desired geometry
also agree during that airborne frame. This rules out an independent board publication
lag as the explanation for that held source pose, not every possible temporal artifact.

Two additional implementation errors were established by reading the actual native
`animation/dingo/animbase_default_skeleton` AntSkeletonAsset:

- Studio `parent()` reused the 386-bone render hierarchy and returned -1 for joints
  386–394. Animation joint 376 is LeftAnkleEffectorAux under Ground (375), not the
  render skeleton's board root. Animation Skateboard_Root is 380 under placement;
  its deck effectors 390–394 must follow 380. The studio now uses all 395 native
  parent indices. Multiplayer's render hierarchy is unchanged.
- Clip loading and recording unconditionally froze attachment/effector translations
  at frame zero. Native clips contain authored target motion; treating these as
  immutable rest positions erased data. New loads/captures preserve those tracks.
  Previously saved files whose motion was already erased cannot recover it from
  their serialized bytes. Reloading the original native library file can recover it.

These fixes restore hierarchy and source fidelity; they do not execute the native IK
and procedural board stack. The first export's gameplay history contains only physics
state 504 with no trick name, so it provides no native 360 flip to compare against.
The next useful capture is a 360 flip performed outside F8, followed by Export diagnostics.
Full physical board-rig locals are now retained in gameplay history as well as preview.

Regression validation compares all 395 compiled parent entries directly against the
exported native asset, preserves moving targets through binary round-trip, and checks
world-space ground/deck target composition. All 101 installed library clips also load.

## Native 360 flip comparison

`dump-111022515-0` contains three observed 360 flip episodes, including two complete
ground/air/ground episodes. No custom edit keys were active in them. On the last complete
episode, the animation skeleton's joint 380 agrees with the physical mount within
13.51 mm and 0.00187 degrees across all 16 retained render samples around the trick.
Their callbacks remain separately timed; this is an empirical agreement bound, not an
assertion that the game writes both buffers simultaneously.

This validates `Skateboard_Root` as the mount frame on this supported native build.
It does not justify an extra fixed rotation/pivot offset. Native captures now take their
board directly from joint 380 of the same body publication, eliminating the old 50 ms
cross-callback pairing allowance from recorded clip geometry. F8 also preserves a
`render_capture` board as captured; the raw-clip foot-height fitting heuristic no longer
moves it independently from its resolved body contacts.

`contrib/animator-studio/diagnostic_reference.py DUMP DESTINATION` extracts a separate
sparse reference document from the last complete named ground trick, verifying physical
mount agreement and the animation hierarchy. The observed episode is 0.65023 s airborne;
this does not establish which native pop asset was active. The reference has 16 samples
over 1.64839 s, maximum spacing 117.11 ms. Missing frames are not recovered or rebaked
as 60 Hz motion. It is marked `render_capture`, keeps takeoff stance/switch, has no
gameplay trick binding, and does not replace the original native library clip.

The native capture reference was visually accepted by the user (no board penetration).
The earlier hypothesis that the bad raw preview mainly required missing procedural/IK
layers was superseded by the source-decoder finding below. The full graph still has
procedural/physics layers; their existence does not establish the cause of a given defect.


## Confirmed compressed-position decoding defect

Native PIEC vector codec 1 uses a minimum and an **extent**, not minimum/maximum.
The 48-byte descriptor has key count/time/value fields at 0/4/8, minimum xyz at 16
and extent xyz at 32. Descriptors are little endian; packed unsigned 16-bit xyz
samples are big endian. Decode each axis as `minimum + (sample / 65535) * extent`.
The supplied CLI export instead matches `minimum + u * (extent - minimum)` on all
16 position tracks of the high 360 flip. This corrupts pelvis/trajectory, board and
contact-target coordinates before any runtime rendering takes place.

On that source, the incorrect foot/target separation reaches about 0.83 m; after
reading original resource samples directly it is below 1.7 mm. The composed Ground
height stays within 0.67 mm of zero, and the initial board centre is 8.5 cm high.
A production C++ reader audit across all 101 corrected library clips (including
three grind phase previews) finds a maximum 2.04329 mm foot/target separation.
This is a consistency measurement, not visual validation of every trick or a claim
that arbitrary authored poses preserve contact.

The repair replaces only translation samples and the matching board translation.
Existing rotations, scales, sample times and phase markers are retained exactly.
Native DOF identities are resolved through the rig's DofIds/DofSets serialized
layouts; they must not be guessed from skeleton name hashes. Wide source times are
big-endian u16. The measured two-key constant vector form is supported; unmeasured
short encodings fail instead of inventing motion. Quantized scalar controls use the
same minimum/extent convention (u8 samples); native compilation corrects them too.

`contrib/animator-studio/repair_library_positions.py --game GAME --probe PROBE
--library LIBRARY --asset-index INDEX --output NEW_DIRECTORY` builds a staged
library and per-asset audit. It does not edit its inputs. `test_native_piecewise.py`
checks quantization, time endianness, unsupported/broken encodings, exporter
convention verification and binary pose preservation. Both library generators now
require native-resource decoding. Corrected catalog entries and saved source
metadata carry `position_codec: native-piec-v1-extent`; legacy personal presets are
preserved and identified in F8 rather than silently changing their base motion.

F8 skips the independent foot-height board fitting for verified native sources,
as well as render captures. It also respects the native controller's IsLooping:
finite tricks stop by default and the single grind-loop source repeats. Explicit
loop playback can still reset at the source boundary. Sampling diagnostics expose
pose count/rate and sparse gaps. Library takeoff/landing markers remain heuristics,
not recovered native event windows; the repair does not relabel them as verified.

The original high-360 F8 preview still needs the user's visual retest after this
repair. No game was launched during the offline repair, per the user's request.


### Preserve the full source trajectory

A fresh regeneration of all three 360 flip variants matches repaired stored poses
within 1.2e-7 (all transform lanes), but exposes unstable heuristic phase markers:
high takeoff is inferred as 0.85 s instead of the old 0.1833 s after correcting the
positions. This is further evidence that local board minima are not authoritative
native events. Verified-source F8 placement no longer depends on those markers at
all: it translates the complete native AITrajectory/Ground pair to the studio floor
using the first source Ground height, preserving all source preparation, airborne
and reception motion. It neither flattens the board outside the guessed interval
nor adds a synthetic jump parabola. Body and board receive the same placement.
Captures and legacy sources retain their existing placement path.

The remaining native gameplay timing work is to resolve and validate controller
tag link flags/event windows and branch transitions. The current additive gameplay
backend still uses episode/phase mapping; this preview correction must not be
presented as a completed replacement for native graph/physics evaluation.


## Maintainer rollout: runtime and library together

Replacing the runtime DLL alone does not repair stored `.clip` data. Old native
catalogs deliberately fail to load until rebuilt with verified position provenance.
No game data, generated library clips or experimental mods are committed here.

1. Build Release with `DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS=ON`, including
   `dingosdk_runtime`, `dingosdk_anim_graph_probe` and `dingosdk_anim_clip_channels`.
2. Produce a typed asset index using the probe (see
   `animation-native-architecture.md`):

   ```text
   dingosdk_anim_graph_probe GAME_ROOT INSPECTION_DIR animation/dingo/animbase_default_rig
   ```

   Use `INSPECTION_DIR/native-asset-types.json` below. The older name-only catalog
   from `index_native_assets.py` is not a substitute: this operation needs partition
   GUIDs from the typed index.
3. Stage the repaired existing library, outside the game:

   ```text
   python contrib/animator-studio/repair_library_positions.py --game GAME_ROOT --probe PROBE_EXE --library GAME_ROOT/ReSkate.Tricks/library --asset-index INSPECTION_DIR/native-asset-types.json --output NEW_STAGING_DIR
   ```

   Inspect `repair-report.json`; require every expected entry to succeed. The script
   retains only successful entries in its output catalog, so never install an
   incomplete result over a full library.
4. With the game closed, back up the runtime, native library and catalog. Install
   the new runtime and stage's `.clip` files plus `catalog.json` together. Keep the
   existing native inventory and all personal saved profiles, recordings and packs.
   `assemble_native_library.py` can install a complete staged catalog; its destination
   must be `ReSkate.Tricks/library`, not the parent user-profile folder.
5. Check the installed library with `dingosdk_animation_studio_tests --native-catalog
   GAME_ROOT/ReSkate.Tricks/library`. Reopen high 360 flip from F8/TRICKS and compare
   the whole preview, including preparation and landing. Old saved personal sources
   are warned about and preserved; reopen from the corrected library before making
   a new native-source profile. This does not automatically transfer old edit keys.

Future exports use the updated generators with `--probe PROBE_EXE --dof-names
NEW_STAGING_DIR/dof-names.json`. The measured encodings are build-specific; unknown
source layouts or exporter conventions must fail before deployment.
