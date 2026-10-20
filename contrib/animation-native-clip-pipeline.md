# Native clip replacement experiment

The first native playback reference is `animation/dingo/c_proto_onb_trick_o_kickflip_high`.
This route replaces its source animation payload, so the existing native controller, tags,
mirror wrapper, graph layers and physics remain available to the game. It does not publish a
recorded skeleton over the final render buffer. Runtime branch execution and visual contact
correctness still require gameplay validation.

## Offline pipeline

Build with `DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS=ON`:

```
cmake --build --preset release --target dingosdk_anim_graph_probe dingosdk_anim_clip_channels
python contrib/animator-studio/native_clip_pipeline.py --cli PATH/reskate_cli.exe --game GAME --tools build/vs2022-x64/Release --asset animation/dingo/c_proto_onb_trick_o_kickflip_high --output NEW_DIRECTORY
```

No edit option makes a no-op reference package. `--arm-degrees 45` makes a deliberately visible
right-arm source change for gameplay comparison. `--input-fbx FILE` accepts a full edited
native source with the same hierarchy, timing and channel capabilities. `--studio-profile FILE`
bakes a saved native-asset profile using the editor's actual C++ quaternion/Hermite evaluator,
then compiles those edits into the native source motion. Body rotations/parent-local offsets
and board edits in the source board's initial axes are supported only where native channels
already exist. Unsupported extra channels are rejected rather than discarded. The canonical
395-joint skeleton is mapped by native name hashes; render captures/legacy sources, mirrored
profiles, retiming and differing clip durations are rejected. No capture is silently relabelled.

The resulting `native.fbproject`, `native.fbmod` and `staging/Patch` stay outside the game.
Deploying requires the separate CLI `deploy-mod GAME STAGING MOD_FOLDER`; the game then loads
the mod through its existing mod layers. Treat animation changes as requiring a restart until
their resource lifetime across level reloads is measured. Disable the runtime additive editor
layer when validating a baked saved profile, so its keys cannot be applied twice.

This is an offline native compilation experiment. Save in the current F8 editor does not yet
invoke it. Native replacement is controlled by the installed mod, independently of the
editor's Custom tricks switch. Fixed/alternate local packs are not connected to native asset
selection yet. The arm experiment affects only the high-pop native kickflip asset; other pop
variants and any alternate native source branches remain unchanged.

## Importer issue and adapter

The supplied Studio CLI expands high kickflip's 93 sparse DOFs (71 quaternion, 16 vector,
6 scalar) to 228, including additional rest translations/scales. Merely pruning FBX curves
does not stop this expansion. Matching the exported standalone pose is insufficient evidence
that these new channels compose correctly with native layers.

The adapter filters the generated channel mapping and PIEC descriptors back to the exact
original IDs/order/types, updates the three Eclipse channel counts, and retains all original
key-data bytes and scalar descriptors. Unreferenced expanded key data remains harmlessly in
the resource. The four header-pointer relocations and the resource metadata's relocation-table
offset are rebuilt. FBMOD v6 resource metadata/bundle membership comes from the CLI's own
template; payload hashes, decoded sizes, raw CAS blocks and the data table are rebuilt.

Only the observed project v2 / FBMOD v6 / PIEC q3,v2,f2 layout is accepted. Unknown layouts,
duplicate IDs, missing channels, truncated data, unsupported edits and changed native
controller/tag contracts fail before the package is compiled. This is an offline file adapter;
it introduces no native memory offsets or code hooks.

The exporter represents the six scalar DOFs (foot onboard, toe/board speed and landing Y) as
`RS_DOF_<id>` helper models with translation.x curves. Those must survive pruning. They are
not ordinary skeleton joints, and dropping them hides actual animation control data.

## Verification

The pipeline checks the exact original channel inventory, skeleton hierarchy, source timing
and all exported local transforms after decoding the new project. Separate error budgets are
10 micrometres for translation, 1e-5 for scale and 0.05 degrees for rotation. Euler/quaternion
float conversion near a gimbal singularity can cost hundredths of a degree; this angular
budget must not be applied to translations. Native controller JSON is compared field by field:
only Eclipse's resource and channel-map bindings may change. Tags, windows, timing, trajectory,
loop and channel-count fields must remain identical.

High kickflip no-op and the 45-degree arm experiment retain all 59 controller instances and
93 channels. The arm experiment's compiled controller is also decoded from the resulting CAS
archives and compared to the project; its compiled key resource is byte-identical. This proves
file/packaging integrity, not final rendered contacts. `test_native_clip_pipeline.py` covers
descriptor filtering, scalar/data identity, unknown-layout and bounds rejection, CAS blocks
crossing 64 KiB and native tag/timing mutation rejection.

Next integration: a native-build action in the editor, an explicit native playback backend,
pack ownership that prevents double edits, native branch/variant selection and a constrained
preview using the same graph. Full render-capture import needs its own rig/phase conversion.
The existing raw preview's board intersections are not corrected by this package.

## Negative gameplay result and stronger resource control

The user reported no visible exaggerated arm movement in gameplay with the initial high-only
arm envelope. This is a negative result, not validation of custom playback. Inspection of the
actual merged mod layer confirms that the modified high controller and key payload survived
the runtime merge unchanged. It does not prove the controller was evaluated during that trick.

The ground-trick route includes `PTM_DB_OnBoard_InAir_Trick`, its linked
`CDB_OnBoard_GroundTricks`, and 111 MotionClips entries referencing 108 distinct anonymous
MotionClip assets. All 108 resolve in bam_levelroot. Normal kickflip's CDB entries 4/5/6 point
to low/mid/high SequenceContainerAssets, which in turn reference their clip controllers.
The CDB's named fields are `EnumGS.Anim.CurrentFlipTrick1`, `EnumGS.PopHeight`,
`Bool.Anim.HipsSpinAddi.LowWeight` and `Bool.Anim.OnBoard.AnimEndsFakie`. The last two are
metadata entries in the inspected map, not established additional live input filters. None of
this supplies the active runtime pop enum or current PTM source frame. Ballistic pop estimates
and presentation names are insufficient to identify them. Mid's existing native export also
fails the CLI's key validation; it must not be silently replaced by another source clip.

`native_resource_marker.py` constructs a stronger **diagnostic-only** control for normal
kickflip low/mid/high. It overwrites only the existing compressed quaternion value ranges of
LeftArm, RightArm, LeftForeArm and RightForeArm. Every key in those tracks holds an already
decoded native Y90 quaternion copied from the original board sample. It does not implement
a guessed quaternion encoder. Headers, times, channel maps, controllers, original resource
IDs/metadata, board, feet and all bytes outside those four value ranges remain unchanged.
The marker deliberately produces an awkward frozen-arm pose, not a finished skate animation.

Its three compiled resources are byte-identical to the marked inputs. The high source decoded
through the CLI has exactly the marker rotations and zero difference in other rotations,
translations and scales. This controls for pop choice, a short source-time envelope and new
resource bindings simultaneously; a positive result would still need isolated follow-ups to
attribute the first test's failure. Gameplay is still pending. Do not claim it fixes preview
contacts or enables native pack alternation.

The graph probe now supports explicit patch/base inspection (`--layer-asset` and
`--layer-resource`), resource IDs (`0x...`), anonymous import GUIDs and `--motion-refs` for a
previously inspected PTM document. Patch TOCs can refer to manifests in base Data CAS files;
those must be read through their original layer, rather than treating a merged patch as a
standalone Data folder. The default GameData constructor retains its original behavior.


## Source decode correction

Before applying edits the pipeline now reads original native resource vectors and
scalar controls using their measured minimum/extent codecs. `original-corrected.fbx`
is the no-op comparison baseline. Comparing against the CLI's original export
would incorrectly count source-decoder corruption as authored motion. Saved native
profiles must carry `source.position_codec = native-piec-v1-extent` (reopen the
corrected TRICKS source and save); old personal bases are not silently rebaked.
Source-resource scope and exact controller/channel contracts are unchanged.

Imported uncompressed key resources can have up to 15 zero alignment bytes before
the four-pointer relocation table. The sparse adapter validates and preserves that
padding rather than rejecting valid source imports. Nonzero padding is rejected.
The corrected high-360 no-op package retains 89 channels and 62 controller instances;
maximum translation round-trip error is 0.36 micrometres. These are offline checks,
not deployment or gameplay validation.

The corrected-source authored-profile check also passes: three tracks (3 cm pelvis
offset, 2 cm board offset in board axes, 25 degree right-arm rotation) survive the
same bake/import/sparse/compile path. Final resource keeps 89 channels and all 62
controller instances, with maximum 0.36 micrometre translation and 0.000283 degree
rotation round-trip error. The generated package stays outside the game and is not
automatically deployed. This deliberately altered contact pose is a data-path test.
