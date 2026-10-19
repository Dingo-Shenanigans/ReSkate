# ReSkate 2.1.0 S.K.A.T.E. source audit - skate.10

Base: upstream v2.1.0, ba1245a5af51892e90f2be8637dd69a7e20f5b77. Server/ is unchanged. Existing 2.0.7 Server/server_activity.cpp SHA256 remains 03c8496b1035b5b5bddc29afcce97d0afdbfcde051868f7cd4200628bb05d73d. Transport protocol remains 52; detector rule version is 10. Installed files were not edited.

Historical sections below describe prior builds; the final skate.10 section and VALIDATION-skate.10.md describe the current adapter. Earlier statements that only PC 2cc is patched are superseded: skate.10 also adapts ignored membership at PC 190. Native area operations remain unchanged.

## Failure evidence

The latest skate.6 recording contains 39 native-circle-state records. All failed to resolve IsInsideTrickArea and UseTrickArea. The experimental reader and area caches were therefore unsuitable for eligibility and have been deleted, together with their mock-only tests. Earlier claims that those readers resolved the supported native layout are superseded by this evidence.

## Actual native path

Supported executable SHA256: fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.

ClientSkateThrowdown.OnCompositeTrickScoringEvent graph 0x7841abd7 has header [64,472,466,781,6,0,0,655414,5177355,2048], code at 0x3b8 and 54 native-call entries at 0xfec. Verification uses the extracted game RES/JSON, not the removed reflection model.

| Native bytecode PC | Operation | Treatment |
| --- | --- | --- |
| 0x190 | IgnoredTricks membership | Unchanged |
| 0x21c | Read native scorable TrickCategory | Unchanged; adapter input producer verified |
| 0x248 | BannedTrickCategories membership | Unchanged |
| 0x298 | Get AllowedTrickCategories reference | Unchanged; input producer verified |
| 0x2cc | AllowedTrickCategories membership | Only patched call |
| 0x330â€“0x454 | Combine ignored, banned, allowed and first-valid state | Unchanged |
| 0x478 | Store native FirstValidTrick | Unchanged |
| 0x4d0â€“0x510 | Read IsInsideTrickArea and store TrickPerformedInArea | Unchanged |
| 0x568 | Read SequenceEnded | Unchanged |
| 0x834â€“0x8f0 | Evaluate !UseTrickArea OR (UseTrickArea AND TrickPerformedInArea) | Unchanged |
| 0x900 onward | Construct successful native Skate_SubmitAttempt | Unchanged |

The three membership calls share hash 0xb0b4285a but have different inputs. Only PC 0x2cc is replaced. Its operands are [2,0x78,2,0x174,0,0x78,2,0x1bc]; the category producer at 0x21c and allowed-array producer at 0x298 are also checked. Both expression execution paths install the adapter before execution and retain the VM context for the native call. Native functions receive operand addresses. The original membership function is called before selectively overriding its output.

Runtime guards check graph hash, full header, instance/resource identity, first instruction, all three membership sites, producer instructions and operand locations. A mismatch invokes the existing bridge-failure path rather than granting eligibility. Unknown categories and opponent turns preserve the native membership result. The adapter groups category 1/16/18/19 under flips, 2/13/14/15 under grabs, 3/4 under grinds, 6 under ground tricks, and forbids ordinary manuals (5), consistently with detector metadata.

Setup's original Score Flips filter contains category 1 only. Hospital flip uses category 19; underflip-related metadata includes category 16. Widening this membership lets the original script reach its native first-trick and area checks. It does not fabricate an area decision.

Native successful submissions are accepted only for the current event handle while the local player is active. Eligibility and pending candidates clear at turn/match/world boundaries and new physical attempts. A completed detector result waits for that native acceptance. False native outcomes retain the native failure flow. Identity is never inferred from area state or the legacy compatibility record.

## Checks and limits

All five suites pass: trick_logger, skate_rules, server_activity, multiplayer_protocol and throwdown_relay. Checks retain recorded hospital/underflip identities, delayed-revert trajectories, signed cancellation, spins above 360, manual/revert distinction, 450-degree absolute grounded-travel rejection, reversal resistance, air reset, uncapped airborne 720, normalized matching, disabled categories, repeated sets, duplicate/stale payloads, bounds, timeout and disconnect behavior. New checks cover advanced category permissions, opponent isolation, native-eligibility ordering and turn/world reset.

verify-native-skate.py accepts the actual extracted RES and checks the patched call, its producers and the later area operations. Extracted proprietary game assets are not distributed.

Static inspection establishes the supported script path and patch contract. It does not establish runtime delivery or a successful two-client match on this build. Native boundary cases, no-area matches and extended-trick setter/defender behavior remain live acceptance items. No fake circle reflection test is used as proof.


## skate.8 capture and recovery correction

Ordinary manual/stall/ground-only-grab observations are ignored before category enforcement. Native category 4/5/15 observations do not seed FirstValidTrick. Eligible airborne grabs retain normal category enforcement. Canonical phase components drive both equality and text; runtime observation identifiers and timing gaps are not matching identities. Per-observation phase association survives revisions, and native stance revisions can update only their own source phase. Movement intervals use existing sample/transition evidence and add no landing timeout.

The area's Boolean predicate at PC 0x8cc is NOT replaced. Its operands and the native UseTrickArea/TrickPerformedInArea producers are guarded. During an outgoing native submission from this verified graph, the client reads evaluated locals 0x1cd/0x1cf/0x1d1, checks Boolean validity and the native expression, and records the outcome. The read is scoped to that expression execution and capture epoch. Unknown contexts do not receive an area-only exemption. Both success/failure submission-construction branches occur after this predicate; an already qualified client attempt retains eligibility only for its own revert recovery, not a new airborne phase. Native code, area functions, relay and server source are unchanged.

Tests assert that preparation leaves the native area function pointer untouched, calls the original category membership, preserves all four native area Boolean combinations, rejects changed operands, and invalidates old execution/turn evidence. They are contract tests, not proof of a live match. verify-native-skate.py separately verifies the actual extracted RES operands and branch instructions.

Grounded travel accrues only in native revert recovery. Ordinary manual motion is excluded. Grind contact closes entry rotation, release opens exit rotation, and rail motion is excluded. The 135-degree grind-adjacent threshold is applied to each signed phase independently; the original flatground rounding and uncapped airborne rotation remain.

The detected-attempt envelope remains unchanged, with optional compact diagnostic evidence added inside its existing JSON payload. Identity comparison ignores this evidence. Protocol 52, payload bounds, exactly-once forwarding and native consequences are retained. Both participants require detector rules 8.

See REJECTION-AUDIT.md for the complete recording audit. Missing opponent captures and missing completed trajectories remain explicitly unresolved; replayed identity agreement does not establish original native area eligibility or live acceptance.


## skate.9 — supersedes skate.8 area handling

The skate.8 native-area evidence reader and area-specific recovery exception have been removed. native_skate_filter.cpp and native_skate_filter.h are byte-for-byte equal to their test7 source-archive copies. native_category_permission is textually equal to test7's function. The category adapter again verifies and changes only the native allowed-category membership call; it does not interpret area predicates or gate on their layout. No native area function is patched.

The latest Josh recording at 20:48:39 shows Powerslide/Revert only, followed by No recognized trick and a native failed submission. That ignored movement consumed the setter turn before a core trick. Excluded movement now cannot start a ground continuation or consume a turn through its own legacy submission. The exemption ends on a new physical attempt or core observation; normal bail/offboard/reset and timeout paths remain. Trickless Air/Pumping and Powerslide are ignored before category enforcement. Empty air phases are excluded from matching requirements. Existing real trick identities, spins and normalized Boned Ollie behavior are unchanged.

Three new archives provide 50 deduplicated comparison events, including 19 matches. SKATE8-LOG-CONFIRMATION.md contains each target, defense and recorded difference. No new Boned Ollie change or redundant Boned Ollie regression was added after the user clarified the observation.

All five automated suites pass and the release binaries build. This establishes regression results and exact native-source restoration, not a completed live two-client area match. Installed files and server source remain untouched; both players require detector rules 9.


## skate.10 supersedes skate.9's late ignored-submission handling

The native ignored-observation stage (PC 190) is now adapted using verified ScorableName/CString metadata. Excluded movement cannot seed FirstValidTrick; known core identities, including a plain Ollie, proceed to the original category, area and sequence branches. The post-submission swallow-and-wait branch is removed. No AttemptDone reset or area predicate adapter is added. The expanded verifier replays the original extracted classification instructions. See VALIDATION-skate.10.md for the distinction between automated evidence and pending live acceptance.
