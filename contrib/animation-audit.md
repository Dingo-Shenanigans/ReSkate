# Animation pipeline audit, 2026-10-19

Baseline: private repository `27a3880` plus the local packs/trace prototype. Scope: clip/key
math and formats, recording, studio preview, saved-variant loading, live body/board publication,
remote pose filtering, entity changes, stance selection, and the timeline editing paths. This is
not an assertion that every subsystem in ReSkate or every native animation state has been audited.

## Confirmed defects addressed

- **Unequal key intervals caused velocity discontinuities.** Position Catmull-Rom and rotation
  SQUAD tangents assumed equally spaced keys. A regression with timestamps 0, 0.25, 2, 3 fails
  before the change. Tangents now use actual time intervals; spherical Bezier controls share
  angular velocity at neighboring segments. Smooth curves can consequently look different.
- **Stance selection ignored switch and was stamped after landing.** Record base stance and the
  guarded switch flag at takeoff; freeze the playback stance at takeoff as well. New JSON stores
  optional `switch_stance`; legacy files remain readable but cannot recover missing historical
  stance information. The initial effective-lead mirroring change exposed a switch deformation
  regression: the old mirror function reflects parent-local bones without bind-frame retargeting.
  The first guard required identical base stance and switch state, which disabled switch edits.
  The next build prefers matching recordings and otherwise transfers authored edits through
  source/target joint frames onto native animation, reflecting placement-space deltas when needed.
  It preserves target bone translations/scales except authored offsets; board keys are applied
  over the native board in placement space, while rails retain native board ownership.
  This is additive edit transfer, not full recorded-motion conversion. Unknown live switch state
  still falls back. Fixed pack selection retains each stance separately. Tests cover identity
  preservation and world-delta conversion with asymmetric joint bases; gameplay appearance remains
  to be checked. Raw `mirror_clip` is no longer used by gameplay.
- **Old render samples were relabeled as new frames.** Accept each fresh, same-generation skater
  publication once, using its own timestamp. Only use a same-generation board publication within
  50 ms. Do not silently mix a current pre-physics board with an older rendered skater.
- **Preview discarded elapsed time after a stall.** Removed the 100 ms elapsed-time cap; advance
  by elapsed wall time, with finite-input guards and correct multi-loop wrapping.
- **Preview republished deep copies of all edit tracks every frame.** Cache immutable edits and
  reuse them when only the playhead moves.
- **Pack loading expanded inactive clips and their mirrored copies.** Choose candidates using
  JSON metadata first, then load selected payloads. A corrupt selected payload is logged and
  skipped; no claim is made that corrupt payloads automatically choose an unselected variation.
- **Respawn and board replacement retained animation-worker state.** Serialize the local worker
  state, track entity/board generations, and reset the recording, airborne episode, floor and
  chosen-clip state on generation changes. Remove the cross-thread write to `logged_layout`.
- **Pending board and sent poses lacked ownership checks.** Stamp pending output/capture with
  generation and the board holder where relevant. Reject stale output after replacement.
- **Gameplay wrote back joints outside the intended body mask.** Preserve those joints at render
  publication and remote pose filtering, rather than copying their earlier pre-physics values.
- **Stale output survived disable/error/no-override paths.** Add an immediate override gate;
  clear pending/sent output on error and inactive paths, and reset episode state when entering
  studio/recording or disabling custom playback.
- **Fresh recordings inherited old document metadata/history.** Clear old trick/pop/speed/keys,
  saved identity, undo and redo before constructing the new document. Unknown trick names can no
  longer silently save as the previous trick. Wait for an observed grounded state before takeoff.
- **Contact confirmation depended on callback count.** Use a 1/30-second contact window and keep
  the first contact as the landing marker. A brief contact interrupted by air resets the window.
- **A bail/off-board state could finish as a normal landed recording.** Cancel the episode for
  the build-pinned wipeout/off-board states, wait for a clean new trick, and release playback.
- **Brackets and keys became inconsistent.** Undo/redo now includes start/end. Brackets remain
  inside short clips; moved keys stay within the clip and colliding times consolidate. Moving
  the end no longer deletes an authored key at the previous endpoint. Locked tracks are respected.
  Explicit windows now limit edits and blend native/custom playback at their edges.
- **Dropping keys on the same timestamp introduced ambiguous duplicates.** The dragged pose
  replaces the existing pose at that timestamp.
- **Long variation names could loop forever.** My packs prototype appended uniqueness suffixes
  beyond `file_safe`'s 48-character limit. Reserve space for the suffix before checking filenames;
  use the same rule for automatic save names and low/mid/high copies.
- **Pop copies silently overwrote previous variations and ignored stream failures.** Create
  unique names and use checked temporary-file replacement. Existing animation files are preserved.
- **Nonfinite/incomplete data could reach samplers.** Reject invalid playhead/speed/window input;
  reject malformed metadata and incomplete clip payloads. NaN sampling no longer indexes past the
  last frame. Check payload sizes before allocating expanded poses, and check marker bounds.

## Actual saved-file evidence and recovery

The isolated test copy's `360flip.json` had duration **1.949419856 s** but a key at
**2.616086006 s**. Its start bracket was 0.5 and end 1.433333278. The old start-bracket shift could
produce exactly this class of unsaveable/reload-rejected file. The baseline loader rejects it
with `a key lies past the clip`.

Preserve the original pair. A separate `360flip recovered` pair clamps out-of-range keys to
the real clip duration and consolidates collisions, leaving the original untouched. It is a
loadability recovery, not a reconstruction of artistic intent. It and the existing kickflip
pass JSON/binary decoding, key-bound checks and finite pose evaluation through the clip.

## Validation and remaining work

The Release runtime and animation regression target build successfully. Regression coverage includes
the 16 base/switch stance combinations as a logical truth table, nonuniform position/angular
velocity, stalled/multi-loop preview timing, fresh versus duplicate/future render timestamps,
30/60/144 Hz contact confirmation, long filenames, profile metadata, bounded key shifting,
locked tracks, colliding key moves and bracket blending. This is not physical confirmation that
the game's stance bit or every joint's mirrored coordinate frame has been mapped correctly.

Trace schema 2 adds the raw switch flag, animation-update/body-publication/board-publication
counters, and publication ages. The trace is opt-in and also works with custom playback disabled.
Counters help separate missing updates, render cadence and stale body/board data; they do not
prove which native animation-graph branch or IK pass ran.

Still open:

1. **Native mirror convention.** Validate regular/goofy and switch/fakie recordings against each
   other, including board orientation and bind-pose axes. The inherited switch-bit evidence is
   strongest for a goofy skater. Base stance still comes from the configured grind-exit stance;
   an unset setting still follows the legacy regular assumption, not automatic game detection.
2. **Frame association.** Render capture is still a latest-sample handoff, not an all-frame queue
   with a shared body/board frame ID. Faster rendering can outrun animation consumption. A 50 ms
   board tolerance can yield a body-only recording at very low frame rates; report and measure it.
3. **Grind phase ownership.** The old airborne-extension/held-landing grind fallback remains.
   Native lock-in/loop/pop-out/landing phase mapping and rail/IK constraints need measured captures
   before a new transition policy is defensible. Pop-out episode segmentation is still a concern.
4. **Airtime/pop guessing.** Velocity-based airtime prediction assumes a ballistic return to the
   same height. Slopes, drops, rails and changed gravity need a better phase policy.
5. **Two-file transaction safety.** Normal overwrite-save still replaces JSON and clip separately;
   it is not a crash-atomic pair. A versioned immutable payload plus atomic manifest, or a tested
   recovery journal, is needed. File loading/cache refresh is synchronous on the client thread;
   very large active alternating packs can still hitch and consume substantial memory.
6. **Room lifecycle.** The room navigation/hiding code still restores scales to assumed 1 and
   uses fixed room bounds. Validate custom skaters/boards, failed navigation, map reload and exit.
7. **Settings isolation.** Profile/game-save paths honor the test launcher's LOCALAPPDATA override;
   some other features use SHGetKnownFolderPath directly. Do not equate isolated saves with complete
   isolation of every preference file.

Next gameplay matrix: regular/goofy x normal/switch/fakie; 30/60/144 FPS plus stalls; flat tricks,
180 landings, reverts, a rail/ledge entry, transfer, pop-out, no-pop exit, manual, bail and respawn.
Capture matching routes with custom playback OFF and ON. Old recordings need re-recording to gain
takeoff switch metadata. No gameplay claim should be inferred from the pure logic tests alone.

## First diagnostic dump: dump-84762296-0

900 consecutive callbacks, 136 sampled poses, same generation; maximum callback gap 27.536 ms.
Two switch Ollies selected the saved Ollie and entered the retarget/mirror path (135 callbacks).
Sampled desired body translations stayed native; peak local edit rotation was 55.232 degrees
on the right arm and 51.304 on the left foot, consistent in magnitude with authored keys. This
proves the calculation ran, not that the final GPU pose displayed it correctly. No grind or
custom-playback-OFF baseline is present in this dump.

A real source-time discontinuity remains at landing confirmation: switch Ollies jump forward
77.254 ms in a 16.358 ms callback and 76.978 ms in a 17.750 ms callback. Normal Ollie also jumps
75.127 ms; the recovered 360 flip jumps 93.560 ms. The old phase branch resets source time to
`clip.landing + contact_age`, losing continuity with the stretched airborne playhead.

`GameplayClipClock` now anchors landing to the previous airborne time and integrates an 80 ms
smooth rate ramp exactly. It preserves elapsed time across stalls, clamps to the clip, ignores
backwards/duplicate timestamps and resets on new attempts/preset selection. Regression cases
include the captured switch timing and a held airborne frame.

The largest position delta (1.834 game units) is on attachment joint 50 during normal-stance
playback, not a switch limb. Gameplay's hips-descendant mask incorrectly includes attachment /
effector subtrees even though clip loading pins their positions to recorded first-frame values.
Exclude those subtrees from animation blend, render writes and sent-pose overrides via the shared
body-ownership mask. Tests preserve all editable body joints and left/right symmetry. This fixes
that ownership error; it does not establish it as the cause of a visible skeleton explosion.
