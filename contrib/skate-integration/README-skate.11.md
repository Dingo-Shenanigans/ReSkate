# ReSkate 2.1.0 — skate.11 native test client

All participants must use skate.11. Keep your existing server; there are no server
files in this package. Configure S.K.A.T.E. with the original game menu.

## Install in a test copy

1. Close the game and launcher.
2. Back up the test copy's `ReSkate.dll` and `ReSkateLauncher.exe`.
3. Extract the client ZIP's two files beside Skate.exe, replacing those two files.
4. Start with ReSkateLauncher.exe. Every participant needs the same build.

This is the native client build, not a scripts/Custom mod. Keep your existing
loader and other installation files. Revert by restoring your two backups.

## Rules in this build

- Without grinds: one eligible jump, including spins, late flips and enabled grabs.
- With grinds: entry trick, chained grind changes and airborne rail transfers,
  followed by the full exit jump.
- The first normal-ground landing, including a manual landing, closes capture.
  Normal revert/manual-revert recovery may still complete that landing's spin.
- Later actions cannot add tricks or spin to the retained combination. Landing,
  native eligibility, bail and other genuine native failures still apply.
- Disabled flips, grabs, grinds and ground tricks are omitted throughout. They
  no longer fail the attempt. Disabled-only activity cannot set a trick and does
  not consume the opportunity to perform a later valid trick.
- Ordinary manuals, stalls, powerslides, pumping, trickless air and ground-only
  grabs remain excluded. Ordinary manual rotation adds no matching spin.

The same rules produce setter, defender and repeated-set signatures. A defender
must reproduce the retained trick identities, stance-relative direction and spin;
extra excluded activity cannot be used to find a different matching trick later.

Full logs start automatically in `logs/tricks`. `trick-log off` remains respected
for that run. The diagnostic overlay remains hidden by default. Capture-boundary
diagnostics identify retained phases, omitted categories and excluded continuation.

## Playtest priorities

Test both area and no-area games, switching between them:

1. Disable grinds/grabs. Set one flip with a late flip. Add a manual-linked second
   jump; the target must still show only the first jump.
2. Enable grabs without grinds. Set a spinning flip with a grab. Disable grabs
   and repeat; the same flip should remain, with no grab requirement.
3. Enable grinds. Change grind types on a rail, transfer to another rail and trick
   out. Land into a manual, then add another jump; that last jump must not extend
   the target.
4. Do disabled-only activity, then a valid trick on the same turn. Confirm the
   valid trick can set. A single zero-spin Ollie must still work when allowed.
5. Match using airborne spin, normal revert and manual-revert routes. Check
   recovery cancellation, the 450-degree absolute recovery limit, bails, repeated
   sets, turns and letters. Hospital flips and underflips remain distinct.

See `VALIDATION-skate.11.md` for automated evidence and limitations. Historical
skate.10 notes describe the old disabled-category failure policy; the skate.11
rules above supersede it.
