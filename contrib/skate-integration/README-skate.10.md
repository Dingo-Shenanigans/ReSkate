# Historical skate.10 instructions

**Superseded. See [README.md](README.md) for the current S.K.A.T.E. mode.**
The disabled-category failure policy described below is superseded by skate.11's retained omission policy.

## ReSkate 2.1.0 — skate.10 test client

Replace only ReSkate.dll and ReSkateLauncher.exe in your separate test installation. Every participant must use skate.10. Use an unchanged ReSkate 2.1.0 server; no server files are supplied or modified. Configure S.K.A.T.E. through its original menu.

## Change in this build

Excluded native observations are classified at the original IgnoredTricks membership call, before they can initialize FirstValidTrick. Powerslides, trickless air, pumping, ordinary manuals, stalls, native ground-only grabs and unrecognized observations cannot seed a native attempt. Reverts remain recovery input for the physical spin tracker.

The adapter uses the scoring definition's ScorableName string and verified native category metadata. It does not infer identity from localized text or the asset hash in the completed record. The catalog contains 351 unique native scoring names from 353 scoring definitions. Explicit core identities, including Ollie, can pass the ignored-trick stage; native category permissions, area checks, sequence checks and submission still decide eligibility. A single landed, zero-spin Ollie is a valid recognized trick when its category is allowed.

The skate.9 post-submission swallow-and-wait workaround was removed. No native AttemptDone value is reset, no turn is restarted and no eligibility is fabricated. Boned Ollie equivalence, independent grind spins, matching, ordinary-manual exclusion and the 450-degree absolute recovery limit remain in place. Native area operations and the relay envelope are unchanged.

## Logging

Full logging starts automatically in logs/tricks. A manual `trick-log off` stays respected for the rest of that run. The diagnostic overlay stays hidden by default.

New native_skate_observation records contain native ScorableName, native identity/category metadata, original ignored value and selected classification. native_skate_capture_trace records report verified scoring-graph local flags and the first-valid record ID; these are diagnostic reads, not area or AttemptDone modifications. Detector acceptance messages identify the physical capture, and genuine native rejections follow the existing failure flow.

## Validation and next playtest

The release DLL/launcher build and five existing CTest suites pass. The tests cover the native CString adapter, all catalog entries, unsupported signatures/types, opponent isolation, plain Ollie setter/defender behavior and 13 recorded captures from the reported stalled turns. The misleading test that supplied a second successful native outcome after swallowing failure is removed.

The actual extracted native RES was checked and its early classification control flow replayed. Ignored observations do not initialize FirstValidTrick; a following core observation does. Existing native area branches are unchanged. See VALIDATION-skate.10.md for evidence and limits.

Two-client skate.10 gameplay acceptance has not yet been performed. This ZIP is a test build for that confirmation, not a claim that live acceptance passed. Both players should test:

1. Inside an area, powerslide or perform trickless air, then land a Kickflip or a plain Ollie on the same turn. Repeat with manuals, accidental stalls and ground-only grabs.
2. Set and match a zero-spin Ollie. Set and match Hospital Flip and Underflip.
3. Repeat without an area, then switch between match types. Check that outside attempts follow native eligibility.
4. Check disabled categories, bails, timeout, recovery spins, turns and letters.

The test ZIP contains exactly ReSkate.dll and ReSkateLauncher.exe. Source, patch, instructions and validation material are separate. Installed game files remain untouched.
