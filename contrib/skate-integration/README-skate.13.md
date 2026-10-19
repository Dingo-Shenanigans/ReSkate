# ReSkate 2.1.0 — skate.13 native test client

All participants need skate.13. Keep your current server and original S.K.A.T.E.
menu. Close the game and launcher, back up the test installation's ReSkate.dll and
ReSkateLauncher.exe, then replace those two files from the client ZIP beside
Skate.exe. Launch normally. Restore those backups to roll back.

## Changes

- Riding heading now comes from the verified body matrix's right axis. Unlike
  its forward axis, this stays horizontal through front/back somersaults. A
  landed grab with a front/backflip no longer loses rotation evidence merely
  because the forward axis passes vertically or reverses while upside down.
- Front/backflip direction remains part of the trick identity. Real bails,
  native eligibility and invalid pose evidence still prevent success.
- Consecutive equivalent trick entries collapse in both target text and matching,
  including duplicate One Foot/Front Foot, Kickflip and 360 Flip observations.
  Equivalent adjacent phases can collapse too; a real intervening trick/grind or
  a different independent spin requirement keeps them distinct.
- Underflip remains a modifier on one trick. Kickflip Underflip is distinct from
  plain Kickflip and Late Kickflip. Late flips, body flips, board rotations and
  other supported modifiers are not erased by duplicate removal.

Slappy Enter exclusion, the inclusive 115-degree grind minimum, bounded combos,
optional-category omission, landing recovery and automatic full logging remain.
No board-heading correction, area changes, relay redesign or server update is
included. This is the native build, not the standalone scripts/Custom mod.

## Playtest

Enable grabs and set/match a landed Front Flip and Back Flip, then repeat with a
horizontal spin. Confirm a real bail still fails. Set and match adjacent duplicate
Kickflips/360 Flips/One Foot observations; the target should show one equivalent
entry. Check plain, late and underflip variants remain different requirements.
Repeat in area and no-area matches and retain both clients' logs if anything fails.

Full recording still starts in logs/tricks; trick-log off remains respected for
that run. Diagnostics preserve raw duplicate rows and record the riding-heading
source and consecutive-equivalent exclusions. See VALIDATION-skate.13.md for
automated evidence and live-testing limitations.
