# ReSkate 2.1.0 — skate.14 native test client

Close the game and launcher, back up the test installation's ReSkate.dll and
ReSkateLauncher.exe, then replace those two files from the client ZIP beside
Skate.exe. All participants need skate.14; matching rules are version 14.
Keep the existing server. Restore the backups to roll back.

## Body flips and setup

Front/backflips now form an independent airborne flip component, using their
native direction/count modifiers. With Flips enabled and Grabs disabled, a body
flip is retained while its grab is omitted. With both enabled, the grab and body
flip are both requirements. With Flips disabled, the body flip is omitted and
an enabled grab remains. A plain disabled grab cannot become a body flip.

Ollie-carried front/backflips now display their actual body-flip name instead of
only Ollie. Repeated observations of the same phase's body-flip modifier collapse.
Front/back direction, body-flip count, board flips, underflips and measured
horizontal spins remain meaningful. Native landing and eligibility are required.

The skate.13.1 sender-attribution fix remains. Native area handling, relay,
turn flow, HUD layout and server source are unchanged. No installed files were
modified. Full recording remains off by default: use `trick-log on full`,
`trick-log status` and `trick-log off` in the console. Recording is not required
for gameplay detection.

## Playtest

With only Flips enabled, set and match a landed Front Flip and Back Flip, including
one whose ticker attaches the modifier to a grab. Confirm opposite directions do
not match. Repeat with a board flip and with multiple body flips. Enable Grabs and
check the grab becomes a requirement; disable Flips and check only an enabled grab
remains. Check plain disabled grabs and ground-only activity cannot set.

Run both area and no-area matches using the same native boundaries. If an attempt
does not set, collect both clients' standard logs and opt-in full trick recordings.
Live two-client acceptance remains pending; see VALIDATION-skate.14.md.
