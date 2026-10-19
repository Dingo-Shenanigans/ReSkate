# skate.13 validation and findings

Base: skate.12 commit dcdbd6e. Product version 2.1.0-skate.13; detector rules 13.

## Recorded findings

Source: tricks-20261019-174859-242-32336-1285061742939.jsonl from the user's last
test. The committed fixture contains selected body matrices, phase/sample counters
and trick identities only, without player names, process addresses or the full log.

- Observation 171, Nose Tailbone Back Flip: a native wipeout and transition from
  airborne 201 to offboard 504 accompany a bailed result. This remains a failure.
- Observation 175, Seatbelt Front Flip: riding touchdown and sequence completion
  were recorded with no wipeout. Forward-axis projection became almost vertical,
  losing valid yaw samples; its inverted direction also created a false horizontal
  rotation. The detector rejected incomplete rotation evidence. The lateral body
  axis remains well projected throughout the recorded somersault.
- The recorded Front Foot case has identical observations 213 and 214 followed
  by a Late Kickflip. Another recorded case has duplicate Heelflip and Front Foot
  rows. These duplicate components now collapse. The Front Foot interval does not
  contain an earlier Kickflip ticker row; this patch does not invent missing tricks.

## Implementation

Riding yaw is the horizontal perpendicular of the same verified body matrix's
right axis. The former forward-axis measurements remain diagnostic data. Matrix
finiteness, basis length and horizontal projection checks remain required; genuine
missing/invalid pose evidence is not accepted. Body-flip direction/count metadata
still determines the somersault's matching identity. Independent skateboard yaw
is not added, and ordinary shuvits cannot become body spins through this change.

Duplicate removal uses complete canonical trick identities, not localized names
or row IDs. Consecutive equivalents collapse in matching and display; repetitions
separated by another core action remain. Across phases, different spin, direction
or stance requirements remain separate. Raw ticker records are preserved and
normalization logs the consecutive-equivalent exclusion reason.

Underflip is preserved on one core trick; it is not converted into a late flip or
another component. Plain, late, underflip, body-flip and board-rotation differences
remain meaningful. Detector versions below 13 cannot compare against these rules.

No native area/AttemptDone/submission adapter, relay implementation, server source,
setup UI or installed game files were changed. skate.12's inclusive 115-degree
grind threshold and Slappy exclusion, and skate.11's bounded/optional rules, remain.

## Automated validation

Release client DLL/launcher build succeeded. All six suites passed: skate_rules, trick_logger, throwdown_relay, multiplayer_protocol,
multiplayer_native_layout and server_activity (test only, no server delivery).
The native-bytecode verification script checks the existing extracted supported
scoring RES; its area and early classification path remain unchanged.

New tests replay the actual landed frontflip with the old and corrected heading:
the old incomplete-evidence failure reproduces, corrected evidence completes with
zero horizontal spin, and the Front Flip modifier survives normalization. The
recorded backflip bail stays failed. Two recorded duplicate combinations normalize
to two eligible core components each. Synthetic front/back/double somersaults
combined with signed horizontal 0/180/360/720 rotations preserve horizontal yaw;
invalid matrices still fail. Duplicate, intervening-grind, different-spin, late and
underflip distinctions are tested alongside existing regressions.

Live two-client skate.13 acceptance has not been performed. Recorded-data and
synthetic tests do not establish live menu/area/turn/letter acceptance. Follow the
README-skate.13.md checklist and preserve both clients' logs for discrepancies.

## Build and delivery

```
cmake -S . -B build/skate13 -G "Visual Studio 18 2026" -A x64 -DDINGOSDK_VERSION=2.1.0-skate.13 -DDINGOSDK_BUILD_TRICK_LOGGER_TESTS=ON -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build build/skate13 --config Release --target dingosdk_runtime dingosdk_launcher dingosdk_skate_rules_tests dingosdk_trick_logger_tests dingosdk_throwdown_relay_tests dingosdk_multiplayer_tests dingosdk_multiplayer_native_tests dingosdk_server_activity_tests --parallel 4
ctest --test-dir build/skate13 -C Release -R "^(skate_rules|trick_logger|throwdown_relay|multiplayer_protocol|multiplayer_native_layout|server_activity)$" --output-on-failure
```

The client ZIP contains exactly ReSkate.dll and ReSkateLauncher.exe. Source,
incremental skate.12-to-skate.13 patch, instructions and validation are separate.
The source ZIP includes upstream license/dependencies and the selected recorded
fixture, without Git history, proprietary assets, installed game data or binaries.
