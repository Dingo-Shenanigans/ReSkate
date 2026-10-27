# Animator integration: d90078a

The integration joins the 14 upstream commits after 27a3880 with local animation packs,
native asset indexing, switch edit retargeting, paired render recording, and bounded diagnostic
exports. Local pre-merge work is preserved in f505fd6.

## Reconciliation choices

- Keep the upstream stage, real board recording, gesture scripts, loading/scrollbar fixes,
  takeoff-relative board orientation and incremental airborne clock. Landing continues through
  the previously measured source time with the local 80 ms rate ramp.
- Preserve base stance and optional switch state captured at takeoff. An incompatible stance
  uses additive authored edits over the native pose; it cannot replay a raw reflected skeleton.
- Preserve fixed/alternating animation packs, exact-pop selection, and temporary blank-name
  continuity. Fade-in begins at preset selection, including a late recognized trick.
- Merge batch recordings into their matching native trick/pop slots, retaining other native
  heights and grind previews. New batch takes include a JSON stance sidecar. Legacy clips keep
  the old regular/unspecified-switch assumption. Engine trick IDs retain underscores.
- Retain the native asset tree, refresh action and diagnostic export. Expose Record all tricks
  in the same RECORD block as single recording; the single-recording banner does not intercept
  batch cancellation. Only RESKATE_RECORD_ALL=1 enables automatic batch startup.
- Retime unlocked keys when either bracket moves, consolidate clipped key collisions, and
  anchor board keys as well as body keys. Preview-only grind clips remain editable/saveable.

## Validation and remaining limits

The regression suite covers slot precedence and retention, filename identity, stance metadata,
incremental time, rate changes, long callbacks, landing continuity and bracket bounds/locks.
A separate gesture inspection executable can validate the installed native input data without
driving the game; see animation-research.md.

On the isolated installed game, the decoder read 36 unique ground gestures (18 nollie) and
validated all three script speeds for each. IDs include `n_heelflip`, as seen in live captures.
Regression tests, all 101 library clips, 17,258 indexed assets, three saved recording fixtures
and a synthetic diagnostic export passed. Runtime and launcher compile in Release. Existing
MSB8064 asset-directory dependency warnings remain in the local build configuration.

Native gameplay still needs user validation for batch stage entry/return, successful trick/pop
coverage, map changes, stop requests, goofy/switch and rail transitions. The batch counts kept
takes, not unique verified coverage of every requested trick/height. A valid decoded gesture
is not proof of a successful scored trick. Two-file JSON/clip saves are not crash-atomic.
Grind graph phases and native constraints are not connected to custom phase replacement.
