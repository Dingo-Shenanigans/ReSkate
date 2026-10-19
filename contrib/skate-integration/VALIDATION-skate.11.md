# skate.11 validation and limits

Base: published skate.10 snapshot `e26c596a1ef194a143dfad61026f27c68ff6eef2`.
Client product version: `2.1.0-skate.11`; detector rules: **11**.

## Implementation

The gameplay tracker opts into bounded capture; its independent diagnostic mode
keeps the existing full physical recording behavior. The first eligible core
observation selects the start phase. Without grinds only that phase is retained.
With grinds, consecutive air/grind phases remain until the first ground phase.
That includes grind changes and airborne transfers, but prevents jumps linked
through a manual landing from extending the target.

Each airborne segment closes recovery at a subsequent takeoff or grind contact.
Only retained segments contribute completion evidence and recovery-limit failures.
Ordinary manual rotation remains excluded. Missing rotation evidence in a later,
excluded jump cannot invalidate an otherwise complete retained phase. Bail,
tracking/reset interruption, timeout and native eligibility remain authoritative.

Disabled categories are classified as ignored at skate.10's existing native
ScorableName membership adapter, before FirstValidTrick/category evaluation.
Normalization omits them too. No native graph hook sites, area membership logic,
AttemptDone state, sequence/submission logic or relay implementation was changed.
The wire validator changes only the accepted detector version from 10 to 11.
No files under Server were edited. No installed game files were modified.

Delayed ticker revisions retain their original phase association. Excluded later
observations are diagnosed without entering the retained identity. Ignored rows
cannot occupy all storage ahead of an eligible observation. Retained-identity
overflow fails without truncation. Network evidence includes retained movement
only; full physical samples remain available in the automatic logs.

## Automated validation

MSVC x64 Release build of the client DLL and launcher. Six regression suites:

- `trick_logger`: native CString hook contract, all catalog entries, ignored
  observations, disabled categories before first-valid evaluation, operand/type
  refusal, opponent isolation and unchanged area predicate behavior.
- `skate_rules`: bounded flat/late-flip/grab combinations, grind changes and rail
  transfers, ground/manual boundaries, excluded recovery, delayed revisions,
  disabled-only activity followed by an eligible grab, exactly-once delivery and
  retained native failures; existing spin, identity, comparison and version tests.
- `throwdown_relay`: existing native attempt/turn/relay regressions with version 11.
- `multiplayer_protocol`: existing wire/protocol regressions.
- `multiplayer_native_layout`: existing native layout regressions.
- `server_activity`: existing stock activity-message regression; no server binary
  is delivered and server source is unchanged.

Historical recording coverage remains: 13 landed rejected defenses with paired
evidence and 13 post-ignored-movement captures, including the zero-spin Ollie.
Three additional recorded boundary assertions use target recordings 35, 36 and
38 from `skate7-rejections.json`: their entry/grind/exit phases remain with grinds
enabled and normalize to their entry phase with grinds disabled. These are
offline recorded-data assertions, not new gameplay recordings.

`verify-native-skate.py` passes against the actual extracted supported scoring
RES, SHA-256 `5a0e7db9d5dbe0b4f4a6051c9ddf8346bf9d7809a3226ca9eeeac78dd94ba722`.
It verifies graph operands and replays the original early classification path,
including disabled observations followed by eligible core observations and
unchanged inside/outside capture. Proprietary extracted assets are not shipped.

## Acceptance limitation

Two-client skate.11 gameplay acceptance has **not** been performed. Automated
fixtures and extracted-bytecode replay do not prove live area/no-area behavior,
menu setup, target wording or end-to-end letters/elimination. Use the playtest
checklist in README-skate.11.md and retain both clients' logs if a discrepancy occurs.

The test client ZIP contains exactly `ReSkate.dll` and `ReSkateLauncher.exe`.
Source, patch, instructions and validation material are separate. Standalone DLL
loader work is outside this native-client branch.

## Reproduce the source build

From a Visual Studio 2026 x64 environment with CMake:

```
cmake -S . -B build/skate11 -G "Visual Studio 18 2026" -A x64 -DDINGOSDK_VERSION=2.1.0-skate.11 -DDINGOSDK_BUILD_TRICK_LOGGER_TESTS=ON -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build build/skate11 --config Release --target dingosdk_runtime dingosdk_launcher dingosdk_skate_rules_tests dingosdk_trick_logger_tests dingosdk_throwdown_relay_tests dingosdk_multiplayer_tests dingosdk_multiplayer_native_tests dingosdk_server_activity_tests --parallel 4
ctest --test-dir build/skate11 -C Release -R "^(skate_rules|trick_logger|throwdown_relay|multiplayer_protocol|multiplayer_native_layout|server_activity)$" --output-on-failure
```

The source patch applies on top of skate.10 commit e26c596. The source ZIP is a
complete source snapshot with upstream license and dependencies; it does not
include Git history, proprietary extracted assets or local build output.
