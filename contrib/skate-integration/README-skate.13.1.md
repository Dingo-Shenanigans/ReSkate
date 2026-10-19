# ReSkate 2.1.0 — skate.13.1 native test client

Close the game and launcher. Back up the test installation's ReSkate.dll and
ReSkateLauncher.exe, then replace those two files from the client ZIP beside
Skate.exe. Restore the backups to roll back. Install this hotfix on both clients.
The dedicated server is unchanged. Installed files were not changed by this build.

## Changes

Fixes a native result handoff failure that could end a match when an opponent's
attempt arrived shortly after a local submission. Suppressed original native
submissions no longer reserve sender-attribution entries. The detected replacement
is still delivered once through ReSkate's existing native handler and sender queue.
Native area checks, turn/letter progression, matching, HUD and relay are unchanged.
The unconsumed-result safeguard remains enabled.

Trick recording is now opt-in. Enter `trick-log on full` in the console to write
full diagnostics to `logs/tricks`; `trick-log status` checks recording and
`trick-log off` stops it. Gameplay detection runs without recording. Standard
ReSkate.log messages remain available even when full trick recording is off.

This is a native client hotfix, not a standalone DLL-loader mod. Matching rules
remain version 13; there is no signature or message-envelope change.

## Playtest

Run an area match and a no-area match with logging initially off. Confirm tricks
still set/match and no trick-recording files are created. Enable recording through
the console if needed. Have the opponent submit quickly after the setter, then
repeat through several turns and a repeated-set rejection. Check letters, turn
progression and match completion. Keep both clients' logs if a result fails.

See VALIDATION-skate.13.1.md for automated results and gameplay limitations.
