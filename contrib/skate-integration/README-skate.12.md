# ReSkate 2.1.0 — skate.12 native test client

Every participant needs skate.12. Use the existing unchanged server and original
S.K.A.T.E. setup menu. This is a native client update, not a scripts/Custom DLL.

## Install

Close the game and launcher. Back up `ReSkate.dll` and `ReSkateLauncher.exe` in
your separate test installation. Extract the client ZIP's two replacement files
beside Skate.exe, then start ReSkateLauncher.exe. Restore the backups to roll back.
No server or loader files need replacing.

## Changes

- Slappy Enter and its stall variant are excluded before native attempt
  initialization and from matching/display. Slappy-only activity cannot set a
  trick. A subsequent real Ollie, flip or grind can still register.
- Independent grind-entry and grind-exit rotations below 115 degrees count as
  alignment. **115 degrees and above** uses forgiving nearest-180 rounding.
  A 115-degree entry or exit therefore counts as 180; two 90-degree alignments
  still count as no spin, rather than combining into 180.
- Ordinary airborne rounding, FS/BS, spin cancellation, high rotations,
  skate.11's bounded combinations, disabled-category omission and recovery
  limits remain in place. Board-heading correction is not part of this build.

Full logging still starts automatically in `logs/tricks`; a manual `trick-log off`
remains respected for that run. Native Slappy exclusion records include an explicit
reason. The spin-policy diagnostic states the inclusive 115-degree minimum.

## Test with both clients

In both area and no-area matches, repeat Slappy Enter, then set and defend an
Ollie, flip and actual grind. Check that Slappy Enter never becomes the target.
Try short grind entries/exits near 115 degrees in both directions. Check a 90-in /
90-out route, rail transfers, late flips, manual-revert recovery and repeated sets.

Automated results and limitations are in `VALIDATION-skate.12.md`. Live two-client
skate.12 acceptance has not been performed. Keep both clients' logs for failures.
