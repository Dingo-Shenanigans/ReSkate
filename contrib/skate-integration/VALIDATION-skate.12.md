# skate.12 validation and limits

Base: skate.11 commit `02caf53`. Product version: `2.1.0-skate.12`.
Detector compatibility: **12**, requiring matching clients.

## Changes and invariants

Both native ScorableName variants (`slappyenter`, `slappyenter_stall`) are ignored
at the existing guarded native membership adapter. Their identities are also
omitted by normalization before category enforcement/continuation initialization.
Raw native/ticker observations remain in diagnostics with an exclusion reason.
Actual grind identities and physical contact/release transitions remain available.

The per-segment grind minimum is 115 degrees inclusive, followed by existing
nearest-180 rounding. A 0.000001-degree tolerance absorbs floating-point delta
summation error at the exact inclusive boundary. Ordinary airborne rounding is
unchanged. Entry and exit are independently measured; rail contact rotation is
excluded. The board-heading proposal remains separate.

No area code, native graph hook sites, AttemptDone state, native submission flow,
relay implementation, visual layout, server source or installed files were changed.
The wire shape is unchanged; its detector-version validation advances to 12.
skate.11's bounded combinations and optional-category behavior remain in force.

## Automated checks

MSVC x64 Release DLL and launcher build succeeded. All six suites passed:
`skate_rules`, `trick_logger`, `throwdown_relay`, `multiplayer_protocol`,
`multiplayer_native_layout`, and `server_activity` (test only, no server delivery).

New checks cover:

- Both Slappy identities alone, repeated, with matching normalization and followed
  by a landed Ollie, flip or actual grind. No successful native outcome is injected
  for the preceding ignored-only activity.
- Native CString classification for both Slappy names, actual FS/BS Tailslides,
  unchanged opponent behavior and existing graph/type/operand guards.
- Independent entry/exit pairs in both signed directions at 0, 90, 114.9, 115,
  115.1, 134.9, 135, 180, 360, 540 and 720 degrees. Checks include independent
  direction, rail-rotation exclusion and two 90-degree alignments.
- Ordinary airborne rounding around 90 and 115 degrees remains unchanged.
- Previous recorded replays, bounded routes, delayed revisions, recovery limits,
  ignored/disabled movement, repeated sets, turns and payload regressions remain.
- Detector versions 10 and 11 are rejected by the version-12 validator.

The actual extracted scoring RES is checked by `verify-native-skate.py`, including
Slappy-like ignored observations followed by a core observation and unchanged
native first-valid/inside/outside capture. RES SHA-256:
`5a0e7db9d5dbe0b4f4a6051c9ddf8346bf9d7809a3226ca9eeeac78dd94ba722`.
That script models the verified bytecode's early classification path; it does not
execute a live game. Proprietary extracted assets are not distributed.

## Acceptance and source reproduction

Two-client gameplay acceptance remains pending. Automated fixtures do not prove
live menu, area/no-area, letters or elimination behavior. Follow README-skate.12.md
for the test checklist and preserve both clients' logs if anything fails.

```
cmake -S . -B build/skate12 -G "Visual Studio 18 2026" -A x64 -DDINGOSDK_VERSION=2.1.0-skate.12 -DDINGOSDK_BUILD_TRICK_LOGGER_TESTS=ON -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build build/skate12 --config Release --target dingosdk_runtime dingosdk_launcher dingosdk_skate_rules_tests dingosdk_trick_logger_tests dingosdk_throwdown_relay_tests dingosdk_multiplayer_tests dingosdk_multiplayer_native_tests dingosdk_server_activity_tests --parallel 4
ctest --test-dir build/skate12 -C Release -R "^(skate_rules|trick_logger|throwdown_relay|multiplayer_protocol|multiplayer_native_layout|server_activity)$" --output-on-failure
```

The test ZIP contains exactly ReSkate.dll and ReSkateLauncher.exe. Separate source,
incremental skate.11-to-skate.12 patch, instructions and validation material accompany
it. The source ZIP includes upstream license/dependencies, but not Git history,
extracted assets, installed game data or build output.
