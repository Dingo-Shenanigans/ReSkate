# skate.13.1 native handoff hotfix

Base: skate.13 commit 955b876. Product version: 2.1.0-skate.13.1.
Detector matching/wire rules remain version 13 because their semantics did not change.

## Source comparison and recorded failure

Compared with the local upstream ReSkate 2.1.0 source at ba1245a, before detector
integration. Upstream observes outgoing native events, queues their sender, and
removes the first graph-matching sender entry when the native handler returns.
Those observed submissions are delivered to the native handler.

The detector integration added suppression after observation: a local native
Skate_SubmitAttempt was still queued with sender 0 although it was never delivered.
The later detected replacement queued another entry with the real player ID.
The handler could consume sender 0 using native local-player resolution, succeed,
and leave the injected local-player entry behind. A quick opponent submission
would then inherit that entry, fail the native guard and remain unconsumed.
The unchanged safety path would leave the match.

The user's TestReSkate/ReSkate.log records local player 0x4 accepted at
20:04:34.970 on 19 October, opponent 0x301 submitted at 20:04:37.523, then a handler
attributed to 0x4 with guard 0 at 20:04:37.536. This fits the queue defect within
its native five-second expiry. The logs do not expose individual queued event IDs;
the specific runtime queue contents are inferred from source and timing.

## Fix

Compute the existing hold decision once in the native send hook. Always observe
the original event for detector eligibility and diagnostics, but pass its delivery
decision to sender bookkeeping. Only delivered events enter the queue. Injected
replacements and ordinary stock events retain native first-handler ordering,
five-second expiry and the existing 256-entry cap. The queue operations are
extracted into a shared, game-independent helper for regression coverage.

No native area/eligibility graph, native guard, result-comparison adapter,
unconsumed-result safeguard, relay implementation or server source was changed.
No installed game files were modified. Automatic trick recording was removed;
the existing console command controls recording independently of gameplay capture.

## Validation status

Release DLL and launcher build passed. All six suites passed: skate_rules,
trick_logger, throwdown_relay, multiplayer_protocol, multiplayer_native_layout
and server_activity (test target only; no server executable delivered).
Native-bytecode verification also passed against the supported extracted RES,
SHA256 5a0e7db9d5dbe0b4f4a6051c9ddf8346bf9d7809a3226ca9eeeac78dd94ba722.
MSBuild emitted existing MSB8064 missing assets/assets-emotes dependency warnings;
compilation and linking succeeded. Live two-client gameplay acceptance has not
been performed.

The new native sender-queue regression reproduces stale attribution with the old
double-entry sequence, then exercises the corrected production queue and hold
policy using the recorded player IDs and sub-five-second timing. It checks 50
fast handoffs, held turn-end events, normal stock sends, unrelated events and
unchanged expiry. This does not execute the live game's graph or prove live
two-client acceptance.

Logger regressions check that gameplay capture works while recording is off,
does not create logs/tricks, and explicit console on/off still controls recording.
