# Native edit playback, first refactor

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
