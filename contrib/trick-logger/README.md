> Historical tricklog.12 diagnostic/practice documentation. For the 2.1.0 multiplayer test client, use [the integration README](../skate-integration/README.md). Practice commands here are optional and separate from automatic match detection.

# ReSkate client trick detector and local practice — 2.0.7-tricklog.12

Stock ReSkate 2.0.7 with client detection, on-screen feedback and optional local practice matching. No server protocol, physics or native scoring changes. Supports Skate.exe SHA256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9. Launcher auto-update is disabled.

## Install

Close Skate and the launcher. Replace ReSkate.dll and ReSkateLauncher.exe, or use Install-Diagnostic.ps1 to validate the executable and back up existing files first.

## Local practice

1. Run `trick-log on full`.
2. Run `trick-log practice set`.
3. Land your target trick or combination. The panel shows TARGET SET.
4. Repeat it. The panel shows MATCH or MISMATCH and a reason.
5. To choose a new target, run `trick-log practice set` again. `trick-log practice clear` removes the target, and `trick-log practice status` describes it.
6. Finish with `trick-log off` to flush both logs.

Arming applies to sets launched after the command. Failed, incomplete or insufficiently recognized attempts do not become targets; the panel explains why the target is pending. Existing targets remain after a mismatch or bail. The practice target is held in memory for this process; practice clear/off removes it. Stopping capture hides feedback and pauses comparisons. Starting capture again preserves practice state until cleared.

The noninteractive upper-left panel shows the current recognized trick, target/verdict and last three results. It captures no controls. Use `trick-log feedback off/on` to hide/show it. Full capture is required; logs are still saved automatically. Plain `trick-log on` is names-only.

## Spin acceptance

Use the full signed body rotation through the captured air/landing phase, rounded to the nearest 180 degrees. Exact 90-degree ties round upward. There is no airborne-based cap: 278/315 become 360, and 539 becomes 540. Signed partial turns that return to the starting orientation cancel. Board spin and native flip identity remain separate from body spin.

Natural footing comes from effective DefaultStanceIsGoofy. Native starting stance and signed rotation determine FS/BS; regular/goofy are equivalent for matching the same stance-relative trick. Unknown stance/direction cannot form a practice target. Native ticker stance labels and plain Nollie names are supported.

## Local matching rules, version 1

Compare landed sets using ordered spin segments and ordered native trick identities. Each spin compares accepted degrees, FS/BS and normal/switch/nollie/fakie. Trick identity compares the native base, stance, board/grind direction and available held-rotation, late, manual, underflip, dark and body-flip modifiers. Native body-rotation text is excluded because measured body spin replaces it. Revert/powerslide ticker rows are excluded; their measured landing motion is already included. Consecutive identical rows in the same segment collapse, to tolerate repeated held-trick rows. Score, row IDs, player footing, timestamps and cosmetic font tags do not affect equality. Comparisons are intended for one client/build/language at this stage.

All recognized grinds, grabs and manuals currently count. Changing a S.K.A.T.E. setup checkbox does NOT yet change local practice rules; the read-only native option mapping needs gameplay verification before category filtering is enabled. Different counts/order, spin, direction, stance or modifiers produce mismatch. JSONL practice_result records retain the normalized signature and rule version.

## Capture S.K.A.T.E. setup options

Run `trick-log on full`, then open native S.K.A.T.E. setup. ReSkate Settings > UI > TRICK SETUP FLAGS displays seven numbered read-only values: flips, grabs, grinds, ground tricks, slams, no leaving board and no session marker. Change switches in the actual game menu and verify the display follows. Unknown means the corresponding native option could not be validated. The unused force-cannonball row is not included.

Validated native selection changes emit `game_setup_snapshot` records with source `native_setup_menu`, a seven-bit mask and validity mask. Completed `trick_set` records include named `setup_rules`; unknown fields are omitted. These are the most recently observed menu choices, which can remain in native models after closing the page, not proof of server-accepted rules. Existing outgoing Create/Update/HostSet parameter snapshots remain read-only so we can cross-check submitted values. Nothing in this build changes native selections or transmitted settings. Local category filtering is still pending that cross-check.

## Landing and remaining limitations

Air/grind/manual continuations can stay in one set; separate airborne segments prevent turning along a rail from adding air spin. A landed result requires ordinary riding after touchdown, a later physics step, observed HUD sequence completion and no captured wipeout. There is no fixed landing or line timer. Wipeout ends a set as bailed; offboard/unsupported/tracking-loss/stopped/reset results are incomplete. Brief unavailable body samples preserve the set but mark rotation incomplete when they overlap a spin phase, preventing an unreliable practice match. State 701 continues a grind-exit airborne segment. State 700 or a position discontinuity ends the set as position_reset; the exact native names of 700/701 remain unverified.

Native WasSuccess and four-wheel contacts are not decoded, and local HUD/ticker association is temporal. State 101 is continuation evidence, not proof of auto-revert. Native detector omissions, late revisions or missing phase/stance evidence can cause inaccurate identities or mismatches. No Bail can suppress failure evidence; eligibility checks are separate future work. On-screen results are client classifications for gameplay verification, not authoritative multiplayer judgements.

TXT contains readable sets and practice verdicts; JSONL retains detailed evidence. Sessions are bounded to 256 MiB with drops/errors exposed by trick-log status. Summary/rotation-export helpers remain available; Summarize-Tricks exports native ticker observations rather than practice verdicts.

## Validation and source

Release build and CTest cover spin cancellation/wrapping, forgiving spin acceptance, stance-direction rules, grind/bail transitions, matching equality/spin/order/bail rejection, logging and feedback toggling. Real recording replay is also checked. In-game practice behavior and setup-menu field mappings await your next recording. Source patch is against upstream 0f8af627105c1079ef25f36ad83b159ae9d1df1f; GPL and third-party notices are included. Body-transform reference: https://github.com/Jish2637/JishsFirstPersonMod/blob/main/src/body_flip_runtime.cpp
