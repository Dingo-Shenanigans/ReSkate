# Native edit playback, first refactor

Current gameplay policy is documented under **Anatomical binding and source key timing** below. Earlier cross-frame/mirroring sections describe superseded attempts.

This replaces the gameplay path that replayed a saved skeleton/board snapshot over a later
native render pose. Studio source preview remains a separate path. It is the first runtime
stage of animation-native-architecture.md, not completion of native graph reverse engineering.

## Resulting behavior

Gameplay uses the current native body and physical board, with the preset's authored keys on
top. Opening or saving an untouched source clip does not replace the native trick. Source
motion can still be inspected in the studio. The PLAYBACK page states this distinction and
shows source provenance and entry binding. Full recorded-motion replacement is not enabled
by marking a source as a capture: source identity describes evidence, not permission to bypass
the native constraint stack.

Legacy files remain readable with source kind `unknown` and the historical ground-entry
assumption. New native-library documents have `native_asset` provenance and their asset/rig
identity; new manual/batch recordings have `render_capture` provenance. Captures still pair
separate render publications by timestamp proximity; they are not claimed to share a proven
native evaluation ID. Ground and rail-exit captures have distinct entry bindings and fixed
pack selection retains both. An unknown observed entry falls back to native motion.

## Runtime contracts

- `PlaybackEpisode` owns observed air, landing and rail-release state, contact confirmation,
  source clock, selection start and episode ID. These observations use the existing guarded
  physics state accessor. They are explicitly not native animation controller phases.
- Entry into a rail releases the body edit layer over 80 ms, with source time held only during
  that release. Native board ownership resumes immediately. A rail pop-out starts another
  episode with a rail-entry binding; the incoming trick cannot continue as an airborne grind.
  Missing state, bail and entity replacement release the custom path.
- An immutable gameplay packet carries generation, component/board holder, callback sequence,
  episode, source time, weight, mirror context, source clip/keys and board edit axes. It carries
  no absolute skeleton or board target. Studio snapshots cannot enter this gameplay path.
- Body/board pin one such schedule per existing presentation counter, even if the animation
  producer publishes another packet between them. This is a presentation cohort, not proof
  of a common native evaluation. Native base poses are read at their respective publications.
- Body edits convert authored deltas through source/native joint frames over the fresh native
  pose. Reference/placement, native scale, attachments, effectors and unedited joints survive.
  Board keys operate over the fresh physical mount in explicit takeoff-board axes relative to
  actor placement. The old independent recorded-board orientation rebase is gone.
- Selection changes blend edit deltas over 80 ms, including a late ollie-to-flip recognition.
  This retains no old absolute coordinates: native motion continues underneath. Board deltas
  relinquish ownership on rail contact; unknown/offboard state and disable reset the blend.
- Repeated publication unwraps only exact previous writes for the same generation, animation
  callback sequence and buffer, separately per rotation/position channel. New native changes
  survive. Network capture unwraps an already edited render pose before applying the most
  recently published edit deltas once; it does not overwrite a fresh network skeleton with a
  cached absolute pose. These identity guards do not establish an undocumented native tick ID.

## Diagnostics and validation

Diagnostic schema 2 adds observed phase, entry, source kind, episode, body/board edit schedule
sequence and presentation cohort. `render_after_edits` rows contain the transforms at our
publication hook, after successful writes; later engine/board-break/GPU processing is outside
that measurement. The old `desired` stage is a pre-physics prediction and can differ from the
fresh render result. All pose stages share the bounded 10 Hz/15 s sampling policy.

Regression coverage includes neutral-preset native identity, translation-only channel identity,
board edit axes, source/entry save-load, ground vs rail pack slots, episode contact bounce,
rail release/pop-out, unknown/bail recovery, presentation pinning/invalidation, repeated render
and network publication, independent native corrections, moving native bases, late selection
crossfades and respawn isolation. Existing saved/native clip compatibility remains covered.

Gameplay validation must still check regular/goofy and switch/fakie, arm/hip/foot/board keys,
late flips, 180 landings, rail entry/exit, bail, respawn, disabling playback and repeated render
callbacks. An untouched preset should preserve native motion; only authored keys should differ.

## Remaining native integration

Active controller/clip phase, branch windows, actual mirror signal and contact/IK weights are
still unmapped at runtime. The existing stance/switch interpretation and ballistic airtime
estimate remain fallbacks. Edits are applied after native posing; large leg/hip/board edits can
still violate contacts because native constraints are not re-solved after those edits. Correct
constrained preview, contact-aware authoring and full motion replacement require a verified
native graph/rig insertion point. No new offsets are guessed here. The new packet/episode/edit
interfaces isolate that work from file formats, UI and absolute pose replacement.


## Ground crouch selection regression

A gameplay diagnostic on 2026-10-20 reproduced nine pops as `100 -> 103 -> 200 ->
201 -> 100`, six named 360 hardflip. Custom playback was enabled and the saved
profile loaded, but every episode retained `entry=unknown`, no preset and zero edit
weight. The classifier recognized only 100/101 as grounded; native crouch/pop
preparation 103 was treated as unavailable and reset the episode immediately before
launch. The ground-entry predicate therefore excluded every saved ground preset.

The runtime and regression tests now share the explicit physics classifier, which
recognizes measured 103 as grounded. Other unverified values remain unknown; rail,
bail and offboard classifications are unchanged. A regression replays the observed
five crouch samples followed by 200/201, asserting one ground-entry episode and an
open trick-selection window. It does not move user keys, disable entry checks or
misclassify unknown state as riding. Authored-profile visibility still needs a user
retest with the corrected runtime; late keys may separately be attenuated by the
landing envelope.


## Switch body facing versus limb reflection

The next gameplay export (`dump-151258656-0`) confirms the high 360 hardflip is
selected with nonzero weight in goofy switch, using a canonical regular/non-switch
source. `mirror=0` in that case: the reported sign reversal is not caused by an
extra call to mirror_vector. Captured native hip orientation differs from the
source by approximately a vertical half turn relative to placement. The old edit
transfer expressed deltas in source placement axes, then converted straight into
the turned native joint frame, reversing anatomical X/Z rotation/offset directions.

Body transfer now carries two independent operations: the existing effective-lead
limb reflection, and a Y half turn when source/target switch metadata differs.
Both act on source placement-space edit deltas before conversion to target joint
bases. The switch relation is frozen at takeoff and shared by the diagnostic
prediction and render packet; timeline CSV adds `reverse_facing`. Native skeleton
poses, lengths and scales remain untouched except for explicit authored offsets.

Regression coverage constructs independently turned native bodies, with and without
limb reflection, and verifies anatomical edit rotation/offset signs under arbitrary
actor heading while all unauthored joints/scales remain unchanged. Matching switch
states retain the previous conversion. Unknown switch still prevents selection.
Physical board edits already use their separate captured takeoff-board frame and
are unchanged by this body-only correction; the observed profile has neutral board
keys. The bounded export contains only switch gameplay; normal playback comparison
and the corrected switch appearance still need the user's visual retest.


## Anatomical binding and source key timing

The user's next test still reported the wrong leg and inconsistent timing.
`dump-151927796-0` captures a goofy **normal**, not switch, high 360 hardflip with
`mirror=1`. The saved source authors LeftLeg/LeftKnee/LeftFoot, but desired changes
appear on RightLeg/RightKnee/RightFoot (indices 8/9/10), reaching 43 degrees at the
right foot. The facing-only adjustment could not correct this: automatic lead-foot
reflection itself contradicts the editor's anatomical Left/Right track labels.

Gameplay now calls `apply_authored_edit_layer`: the saved joint ID, joint-local
rotation and parent-local offset are applied to that same anatomical joint on the
fresh native rig. Source/target stance does not swap limbs or conjugate the edit
through a sampled source pose. Explicit mirrored-copy authoring remains available
separately. Cross-frame transfer helpers remain experimental and are not called by
gameplay. Neutral keys preserve the native transform; body scale and unauthored
joints are untouched. Board keys use the actual takeoff-board frame without an
extra lead-foot reflection.

Gameplay selects the source-timing path of PlaybackEpisode: `source_takeoff +
(now - observed_takeoff) * speed`. Recognition or name refinement does not restart
that clock. Ballistic airtime estimates no longer rescale it, and landing no longer
cuts reception keys off after 600 ms. The authored source continues to its duration
with a short entry/end blend; rail contact releases to native constraints and bails
still cancel playback. Explicit editor brackets still gate edits. Diagnostics
identify `anatomical_local`, `source_seconds_from_takeoff` and `source_timing`.

Regression tests compare gameplay directly against F8's local edit evaluation on
independently oriented native bodies for all base/switch combinations: left stays
left, offsets retain local signs, and only authored joints change. Source-clock
tests cover recognition delay, refined names, differing ballistic estimates, late
reception keys, and duration completion. This supersedes the earlier gameplay
mirror/facing conversion, not the standalone coordinate-math tests.

This is still an **additive edit backend**, not full source-motion replacement or
an active native-controller phase read. A source phase marker aligns takeoff, but
keys preceding takeoff cannot be applied before the future trick is identified.
Native base motion, pop variants and contacts can therefore differ from F8's source.
The offline native compiler has its own canonical-source/mirror-wrapper semantics;
it is not an automatic anatomical runtime variant backend. Exact whole-animation
parity requires verified native phase/branch bindings and stance-aware source
replacement. The updated UI states these limits; user keys are never silently
retimed, reassigned or rewritten. Visual retest of this policy is pending.
