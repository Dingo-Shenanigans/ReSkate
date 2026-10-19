# skate.10 validation and limits

## Reported failure and correction

Josh's skate.9 turn 3 swallowed a failed native Powerslide submission at 21:41:40.868. Subsequently captured Pop-Shuvits, 360 Flip, Kickflip and Heelflip waited for native eligibility until timeout. Kritter's recording contains a failed native submission followed by five captured Kickflips without another submission. Sam's stalled turn includes a completed zero-spin Ollie.

Those recordings do not contain a direct read of AttemptDone. The precise runtime latch value is therefore not claimed as proven. The verified integration error is that skate.9 discarded an already-produced native outcome and then expected the native flow to submit again. The fix removes this late workaround and classifies excluded movement before FirstValidTrick is initialized.

## Native verification

Supported graph: ClientSkateThrowdown.OnCompositeTrickScoringEvent, resource 7841abd7. Actual extracted RES SHA256: 5a0e7db9d5dbe0b4f4a6051c9ddf8346bf9d7809a3226ca9eeeac78dd94ba722.

The ignored membership call at PC 190 takes the value produced from DingoScorable.ScorableName (field hash 7f1e40dd). That field is a native CString. The ignored path branches through 31c before the existing first-valid gate at 454 and setter at 478. A default first-valid record cannot pass the later nonzero-scorable guard at 7ec/824. The area capture at 4d0/510 and eligibility predicate at 834–8f0 are not patched. The native successful/failure submissions and their completion state remain untouched.

The existing category adapter at 2cc remains; a second guarded adapter at 190 adjusts ignored-observation membership. Its native signature, producer operands, branch, graph header, call table and runtime CString type are checked. Original native membership executes first. Opponent turns preserve the original result. Invalid contracts trigger the existing bridge failure notice.

Native record ScorableId hashes the scoring asset name (verified against Kickflip's recorded ee4f9bb7). This is distinct from the ScorableName string compared by IgnoredTricks. The new classifier uses ScorableName, not a guessed ID conversion. Its catalog is reproducible with generate-native-tricks.py from extracted DingoScorable JSON; proprietary game assets are not distributed.

verify-native-skate.py walks the actual extracted classification instructions with modeled Boolean/field operations. It verifies ignored then core, repeated classifications, existing first-valid evidence, disabled/banned categories and unchanged inside/outside capture. This is asset-level control-flow validation, not execution inside the game.

## Automated results

Release build: ReSkate.dll and ReSkateLauncher.exe, product version 2.1.0-skate.10.

Five suites pass: trick_logger, skate_rules, throwdown_relay, multiplayer_protocol and server_activity. Existing spin/recovery, phase, payload, repeated-set, turn and relay regressions are retained.

New checks include all 351 native names through the CString adapter, unknown names, native-ignored Ollie admission, manual/trickless/powerslide exclusion, changed operands/type refusal and opponent isolation. A zero-spin Ollie normalizes, sets and matches. The obsolete regression that manually supplied a second native success after ignored failure is removed.

skate9-ignored-turns.json preserves 13 actual detector captures from Josh, Sam and Kritter. Replaying these confirms their recognized trick identities survive normalization, including Sam's plain Ollie. It does not pretend old logs contain the corrected native events.

## Pending gameplay acceptance

No live skate.10 two-client area/no-area match was run in this environment. The test build is supplied to obtain that confirmation. Verify ignored movement followed by a valid in-area trick on the same turn, plain Ollie sets/matches, Hospital/Underflip, native outside behavior and matching game consequences. Inspect native_skate_observation and native_skate_capture_trace alongside native submissions if it fails.

No new area reader, geometry, landing window, AttemptDone reset or server modification is introduced. Installed files are untouched. Protocol envelope remains 52; detector rules are 10 and participants must agree on that version.
