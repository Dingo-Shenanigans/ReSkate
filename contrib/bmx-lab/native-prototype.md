# Native BMX prototype

Current development status:

Attempt 30 builds successfully and passes all 66 Release tests (4.58 seconds).
Its runtime is `409C80FBFDF80CD16F4F8D11E5BFD0FDD29AA81A5B40AC0E99F83852FF509996`.
Ordinary Ride trials 260/261 use the separately opted-in human adapter and an external
virtual XInput controller in the isolated human test profile. The first LB+Y
bike-to-board switch completes native 103 then 100, with 2.266 seconds between
the retained first-100 and final-100 observations; the rider visibly stands on
the skateboard. Each input session delivers 301 reports and releases/disconnects.
Trial 260's same-process reverse switch fails: natural bike mounting begins, then the
existing native animation/ragdoll guard stops it after one retained ride sample.
Attempt 30 adds a pending-only settled-dismount gate. In trial 261 the reverse
request waits at that gate without beginning a new BMX mount, but the native rider
still falls and the pending mount times out. This prevents premature acquisition;
it does not fix the native dismount request. The full round trip still fails.
Clean NativeOnly trial 262 supplies the original-Y comparison: the sampled stream
reaches 100 between two external Y pulses, then returns to 504 with 90 normal
offboard/standable/motion-supported client samples over 5.567 native seconds.
All BMX counters remain zero and software control attempts remain zero. Native
edge storage has 448 capacity drops; the complete CPU export remains available.
This supports an input-based reverse handoff experiment, not a completed BMX fix.
Physical controller acceptance and complete
motion quality remain unverified. Sources match all 733 frozen pins after the run;
the continuous recordings and outcomes are preserved in `human-native-run260` and `261`.

Attempt 28 corrects the own-BMX protection admission conflict exposed by trial
257. Attempt 29 adds a bounded 250 ms wait for actual original-reader shortcut
proof when SDK input arrives first, as observed in failed trial 258. Trial 260
takes the immediate admission route, so it does not dynamically exercise that
new deferred branch. Attempt 30 preserves the original ragdoll and collision guards.

Trial 256's raw camera data identifies a separate measurement defect: the 12
used spatial values and FOV pass validation, while 20 receipts contain NaNs only
in the four ABI-unused SIMD lanes. Attempt 28 includes the reviewed local-copy
normalization. Trial 259 passes the original ramp criteria and exports 102
spatially usable HUD rows out of 120, compared with six in 256. Both visual axle
anchors still need qualification. It improves diagnostic availability, not the rider pose itself.
Independent review of three actual ramp phases scores still presentation 7.8/10:
registration 8.2, hands 8.0, obscured feet/pedals 7.0, stance 7.8. Motion, controls
and collisions remain unrated. The 9.5-per-category quality target is not met.

The following records describe earlier builds and private experiments.

Current built and privately installed runtime
`54FDA85C089AB2BBAEFF4846317F24CD6E02FB4253D0E0A66FC66FB790F0E803`
passes 65 Release tests in 5.02 seconds (attempt 25). The independently reviewed
rider stance raises the hip/chest requests by 4 cm and articulates the existing
clavicles while preserving native segment lengths and fixed grips/pedals.
Native ramp 240 passes the original ascent/flight/landing criteria. Its 64
captured bone readbacks match the solved pose; independently recomposed palms
and ankles remain within 0.1 mm of their targets. Actual skinning, motion and
controller feel still need broader visual judging; the rider remains fairly crouched.

Private mount trial 243 completes one guarded press/release sequence within
218 ms and visibly reaches the normal skateboard stance. Its complete 16,206-byte
process-bound export preserves all 56 ledger rows; independent review confirms
one press edge, one release edge and the native 504→103→100 sequence. The export
still marks native input consumption and ride quality unproved. This is a clean
on-foot baseline, not a verified return from BMX. The preceding trial 242 visibly
mounted but expired; its truncated log remains preserved separately. The reviewed
stateful release phase fixes that narrow private timing aperture without changing
normal equipment controls.
Normal equipment switching and the floating skateboard stance remain unresolved.
The separately selected private post-BMX trial 251 records one original-input
press/release and native 504→103→100 with three-stream dwell. Independent footage
review finds a normal skateboard stance across 22 samples over 10.48 seconds
(limited visible stance score 9/10). Its official result remains FAIL: compact
observations omit three required state flags and the supervisor checks the wrong
log path. Attempt 24 fixes both omissions, confirmed directly in fresh trial 253.
That trial remains FAIL: the completed native tuple falls between retained client
samples under the unchanged 50 ms minimum sampling policy. It exists in raw native
pairs, but not in the exact client row required by the strict checker. A narrow
terminal-row retention repair in attempt 25 now passes fresh trial 254's unchanged
strict native and final provenance/teardown checks. Independent audit finds the
exact terminal tuple retained once. The neutral fixture expires after the native
terminal result; it does not remain connected through teardown. Continuous footage
is archived separately, with visual review pending. Old failed receipts remain
preserved. Ordinary physical LB+Y switching is still unresolved; a separate
opt-in human-intent adapter is being developed.

Ramp 255 repeats the original route gates with attempt 25 and the same private
100 kN/m landing settings. Its 128 captured bone records are byte-identical to
252. All 21 new camera diagnostic receipts reach the existing camera validation
and report `camera_basis`; no camera is published across 113 HUD rows. The
rejected matrix/FOV subcheck is not yet known. This narrows the diagnostic failure
without proving the cause of the visible hand/bar gap or improving rider contact.

Ramp 241 repeats all 12,984 selected physics values from 237 exactly. Its new
540 producer successor witnesses include the first landing and confirm one
successful application per recorded command. The additional landing response
remains; native contact/accumulator telemetry is unavailable. Diagnostic ramp 244
preserves all recorded physics and supplies two fresh approach-plane witnesses.
The separately default-off normal response passes private ramp 245 under the
same ascent/flight/landing criteria. Peak penetration falls from 30.078 to
6.471 mm; the unexplained total-command residual falls from 156.802 Ns to
floating-point scale, with all 540 producer successors accounted for. Two-wheel
support settles sooner, but peak angular speed rises 27% and route progress
falls 17%. Keep this response private while tuning that tradeoff; ordinary
controller sessions do not select it.

Private compliant-contact run 248 passes the original physical route criteria,
but is rejected as a handling improvement: penetration increases from 6.471 to
22.174 mm, peak angular speed from 3.871 to 4.168 rad/s, and settling from 0.05
to 0.167 seconds. All 540 producer successors account for the submitted command;
the external residual remains at floating-point scale. Applying compliance
throughout the route changes takeoff and touchdown timing. Two limited contact
steps fall back to the original rigid response; no submitted compliant row
violates its declared linear 15 mm prediction. This remains default off while
the changed landing phase and capped fallback are investigated. The linear
prediction is not native collision or swept-clearance proof.

Landing-only trial 252 preserves the earlier takeoff response and passes the
original route gates. Peak angular speed falls from 3.871 to 3.487 rad/s, while
penetration rises from 6.471 to 13.873 mm and two-wheel settling from 0.05 to
0.067 seconds. Progress changes from 3.775 to 3.807 m. The accounted command
residual remains at floating-point scale. This mixed tradeoff stays private;
native route success is not handling acceptance. Independent audit confirms the
255-row physical prefix and the incident state match the old route exactly;
285 actual soft steps have nonpositive computed combined contact energy under
the declared model, with
one bounded rigid fallback. The old 6.471 mm route maximum occurs before landing:
its post-touch maximum is only 0.384 mm, versus 13.873 mm in this experiment.
Independent footage still shows a brief hand/bar gap (registration 6.5/10);
full handling and complete categories remain unrated.

The independent 244/245 visual comparison finds no defensible visible improvement.
Both hands float above the grips during descent. Scoped registration scores
7.5/10 and sampled ramp/landing geometry scores 9/10; full motion, controls and
complete categories remain unrated. Probe 240's 64-frame window ended before
the affected descent/touchdown, so its successful CPU readbacks do not resolve
this later visual mismatch. Extended probe 247 covers 128 actual records through
descent and touchdown: every solve/write/readback qualifies, and CPU palm-to-grip
error stays below 0.087 mm. Its 12,984 selected physics comparisons to 245 match.
The retained visual frame is one native step historical in every record. That
timing difference narrows the investigation but does not prove which frame the
rendered bike or rider consumes. No static grip-offset change is justified.
Independent frame review of 247 reproduces the hand gap during descent despite
the complete CPU readback match. A stamped CPU-request/camera diagnostic has
passed source review and compilation. Trial 252 exports 115 CPU request HUD rows,
but camera/spatial rows are zero, so axle-pixel qualification remains unavailable;
final GPU pose is unobserved. No hand-position offset fix is justified by this.
Attempt 246 timed out during recording preparation and supplied no ride evidence.

Preceding runtime
`127DD55FA22A83434EE405913CC614A6F44F74D6C9CB8646E6BA95AE025BA53F`
passes 60 Release tests in 4.63 seconds (attempt 13). Native trick run 235 passes
with an automatic landing pump window. Controller run 236 measures a 0.3-second
direct left-stick pump, matching submitted chassis commands and extra current
tire load on flat ground. All 195 source pins and both teardown observations verify.
The continuous recording also shows the unresolved rider/deck gap after switching.
Native ramp run 237 passes the same small-ramp fixture and terminal criteria,
with continuous recording retained. Two blind visual reviewers find coherent
upright travel, but flag a persistent crouch and weak landing differentiation.
Their visible-subset scores are below 9.5; every full category remains unrated
because required repetitions and scenarios are missing. Controller timing and
ramp feel still need further validation.

Preceding privately installed runtime
`F3D8D1CC4B09AC4A88736066F61397BA52A4BA02D33B9594189B7F54041BE951`
passes 59 Release tests in 4.33 seconds (attempt 11). Fresh NativeOnly sessions
bypass all BMX lifecycle/input handling, so ordinary Y can be compared without
BMX intercepting it. Session 229 was ineligible before arming; the dispatch of
the earlier human Y comparison in 227 was not captured. Native trick run 230
passes with unchanged source/runtime and two verified teardown observations.
Its real tailwhip screenshot is saved under `artifacts/bmx-reddit`; no visible
quality score or controller feel is inferred from that frame. The approved
virtual-controller driver is installed and neutral/disconnect are independently
verified; game input consumption must be checked separately.

Preceding runtime `9B9F3E5702D07B9D25F380DF49214F8DC876B82F51609EFE4EF7AAC130541E5F`
passes all 59 Release tests in 4.28 seconds (attempt 10). Explicit private passive
NativeOnly `-DeferNativeBoardArm` leaves startup unarmed and allows 30 seconds of
prearm readiness to close the console after a later arm request. The eight-second
capture, ordinary gameplay guards and default preparation budget are unchanged.
Queued is not proof of armed; compare recorded native timing afterward. This
diagnostic update does not fix physics or establish visible riding quality.

Preceding runtime `8DA149BB6FCEA0F40E2E939EC0D391BE5FC9E77D390730404E2D1DBA036C1145`
passes all 59 Release tests in 4.32 seconds (attempt 9). Run 228 passes the four
synthetic equipment-switch routes with source/runtime identity and two teardown
absence observations. Its opt-in stored-entity observer captures 104 qualified
part matrices over 13 frames; every matrix exactly matches the requested words.
This covers the first short mounted episode only, without a native-clock join or
renderer/contact proof. Evidence is under `build/bmx/chassis-ride-run228`; the
33 sampled images under `build/bmx/recorded-prop-transform228` include a 35-second
startup waiting gap and do not establish smooth motion or visual acceptance.

Preceding runtime `BFABDA578D292EB2DDBF66D835FC7C0621AFAD9127A0D15BBFDA3308ADAF0D4E`
passes 59 Release tests in 4.09 seconds (attempt 8). Its default-off private observer
retains paired selector/publication snapshots without changing native requests,
pose, physics, support or collision guards. Clean software-board run 225 passes
strict CPU coverage with zero BMX counters; all 151 measured post-publication
mount outputs remain zero while the visible stance still floats. No graph
consumption or visual acceptance follows. The native report and independent
analysis remain under `build/bmx/chassis-ride-run225` and
`build/bmx/judging/native-board225-independent-data`.

The user reports a normal skateboard stance after ordinary Y without LB in clean
session 227. Its initial trace expired before the requested action, so that result
is human feedback, not a captured post-Y native trace or complete post-Y recording.
The manual capture workflow is being corrected. No new four-route pass is claimed
for attempt 8: run 226 was intentionally cancelled before its recording gate was
released to prepare the authorized controller comparison.

Preceding privately installed runtime `73EFF12E81A4BAB24020080AB39EE18F572976550805E9761262532E3921F773`
passes 59/59 Release tests in 4.21 seconds (`build/bmx-retained-remount-build-attempt7.log`
and matching CTest log). Retained nearby search and strict 2.25-second dwell in all
three board-publication streams are applied. Animation experiment 6 is rolled back;
its exact restoration receipt is
`build/bmx/judging/native-board-animation-handoff/revision2/root-rollback.json`.

Run 223 passes the original short four-route native functional regression: four mounts,
one allocation, one strict set-down, one guarded followup, and 69 selected step-off
basis-word matches with zero mismatches. Source/runtime identity, report identity and
two teardown absence observations verify. This route has no extended board-pose trace.
Its 32 sampled frames are not yet independently judged. The previously failing
retained-search branch, sustained visible board stance, hardware feel and 9.5 quality
remain unaccepted.

Extended run 224 passes all four routes on this unchanged runtime, with the same
four mounts, one allocation and guarded set-down followup. Its exact-owned trace
retains 136 rows and strict board100 dwell in all three streams: client 2.283333197
native seconds / 2328 ms, each publication 2.299999863 native seconds / 2343 ms.
Post-release SDK counters stay fixed. See
`build/bmx/chassis-ride-run224/root-verdict-check.json`. The independent 32-image
pixel review rates bike/rider contact 4/10 and feet/deck contact 1/10; its manifest
incidentally exposed the native receipt, so it is not fully verdict-blind.
The intermittent fall in 222 remains unresolved. These are functional and CPU
observations, not animation-graph, hardware, continuous-motion or quality acceptance.

Native control 219 occurs before any BMX mount and passes strict CPU evidence,
including the experiment's SDK request/publication delivery; the blind review still
scores feet/deck coherence 1/10. The animation experiment did not fix visible contact.
Run 220 fails at native state 103 before the retained-remount phase. Current-runtime
run 222 fails before grounded board100, with a visible fall and Heavy Slam (blind
visible demonstration 4/10). Run 221 is an archived GPU startup failure. These failures
remain preserved separately from run 223's narrow functional pass.

Earlier switching diagnostic 215:

Runtime `93AE2534216493B430F0151A5C35B5F5FA95C035AAA3C6F63472FBBB5ACC7030`
passes 59 Release tests. The repaired diagnostic activates in recorded run 215 and
archives 136 exact-owned rows: 38 client and 37 of each CPU publication kind in
grounded board state 100, each kind spanning more than 2.25 native and wall seconds.
Post-release SDK motion/pose/render/hide counters remain fixed while native CPU bones
evolve. This excludes continuing counted BMX writes or a wholly frozen CPU pose as
simple explanations, but does not identify the remaining cause. Blind visual review
confirms repeated visible sole/deck separation. The later nearby-bike remount fails
on support readiness, so the complete route remains failed. Its original report and
48-frame sampled recording are preserved. Neither physics quality nor full motion
is accepted. A clean native-board control is the next diagnostic.

Preceding runtime `B5C49FF887403A2561828DE122C9CE206083975283DB116972963719474BE3DA`
passes 59 Release tests and recorded native shortcut run 214. All four routes,
source/runtime identity and two teardown absence observations verify. BMX status
uses one replaceable card; the blind review confirms no stacks but still sees
transition pose/text disagreement. Its 32-frame capture is sampled live, not full
motion. No sustained rider-contact verdict is supported. The opt-in pose trace did
not activate and the diagnostic archive separately failed a timestamp conversion;
no successful trace coverage is claimed. These diagnostic setup repairs are pending.

Preceding runtime `98F6DDAEE200252CA32364C0A2195192008233E80CE106445D8F5EE1D677CBC5`
passes 58 Release tests and recorded native shortcut run 212. The four contextual
routes complete: mounted bike to native board, board to bike, nearby parked bike,
and carried bike through guarded set-down to mount. Nine ordered observations,
one allocation, four mounts, four one-shot chords with no ordinary-Y events, strict
grounded board confirmation and two teardown absence observations verify. Repeat
run 213 also passes all four routes on this unchanged runtime, with 69 basis-word
matches and zero mismatches; its 44-frame sampled recording is retained separately.

Physical step-off now preserves the supplied native body orientation. Its 69 selected
SDK target paths retain all nine basis words, with no mismatch. This is target selection
evidence, not native-consumption proof. Run 211 and 212 share all 54 initial riding samples
and their first shortcut pulse exactly; only the four expected compiled sources/tests
change across 167 verified archived files per run. Run 212 reaches board100 at the same
serial/time where 211 falls. The comparison remains one controlled route, not universal
recovery or physical-controller acceptance. The sampled video is
`build/bmx/recorded-quick-switch212/sampled-live.mp4` (64 frames); full motion is unproved.

Preserved preceding failure: recorded native shortcut run 211 fails after one released
LB+North pulse: an accepted native board request progresses through owned states
200/201, then the rider falls in off-board state 504 without standability or supported
motion. No grounded board completion or subsequent shortcut route is claimed.
Source/runtime identity and two teardown absence observations verify. The 53-frame
sampled recording is `build/bmx/recorded-quick-switch211/sampled-live.mp4`; it cannot
establish continuous motion. The failed receipt is in `build/bmx/chassis-ride-run211`.

The test now handles an initially cold native clock using two advancing observations,
and waits neutrally through only proven, owned native board-mount states 200/201.
Fall and terminal grounded-board checks remain strict. Its first gesture produces
one shortcut event and zero ordinary-Y events. This synthetic game-side rehearsal
does not test a physical controller's first packet. A separate visual review of
run 210 finds shoe/deck separation after logged board state 100; actual pose handoff
and the new fall require bounded passive tracing, not a universal foot offset.

Steering iteration preceding the switching instrumentation:

Runtime `790500F76ED1F2F20537695DD0593DE4DE50EDC0766B205A39AEAB7F6C1A6E74`
passes all 58 Release tests. Supported steering now shares a rate-limited achieved
angle between wheel heading and banking, with native-time joint response and
post-envelope history. Air rotation and contact/fall/force limits retain their values.
Matched native runs204/206 have identical initial state and541 input/time rows.
Run206 passes the caught barspin/tailwhip landing with zero post-contact support losses.
Ramp207 passes ascent, natural launch, landing and stopping, with .516667s flight,
.450000s qualified natural flight and27 passive angular intervals. Its source hashes,
report identity and two teardown absence observations are verified; no video is attached.
Steer angular-rate steps fall .12873 to .01772rad/s; release/brake steps fall .17837
to .03817rad/s. Release/brake RMS rises .10042 to .10977rad/s, so response smoothness
trades against slower centering. Full-bank native reversal and controller feel remain
unverified. See `build/bmx/judging/steering-native-comparison-204-206/comparison.md`.

LB/L1 plus a fresh Y/Triangle press now requests the contextual equipment switch;
plain Y tap/hold retains mount/step-off and carry/set-down. Board-to-bike waits for
observed native dismount and strict ground instead of requiring another Y press.
A held Y followed by LB is not a shortcut. Focus/menu/device/owner interruptions
quarantine a claimed chord through release. D-pad bindings retain Skate behavior.
F6 retains its existing explicit skateboard route. Stop and use clear ground for
bike-to-board. A far parked bike still requires approaching it. Equipment203 and
Tricks204 pass on the preceding35D96 runtime; the new physical shortcut packet and
its game-side route still require separate acceptance.

The latest user clip `skate-2026-10-20_14-46-23.mp4` was independently reviewed across
826 decoded frames. Overall visible quality is5/10: hand/grip separation, rigid body
response and abrupt fall motion remain. Feet do not justify one global translation.
No visual9.5, physical controller acceptance or near1:1 Streets parity is established.
Current Streets GameAssembly is0DAECE89...F8C2; old image RVAs are not current evidence.
The refreshed input asset confirms BMX/Tricking bindings, while runtime prefab overrides,
ground pump/slider mechanics and per-shape collision friction remain unresolved.

Earlier October20 damping iteration:

Earlier runtime `D8F79B702528E37EEE0A9652628B0F6E46AA859A08FB85CB2EE622E90202192F`
passes all 58 Release tests (3.94 seconds). The physical steering-release regression
queries actual smaller-bike hubs at the default controller's 2 cm gap, integrates force, gravity
and world inertia, and checks eight forward/fakie, mirrored and world-heading cases.
It fails the original damping on excessive opposite lean and passes the new damping.
No repeated orientation/height repair or relaxed contact/fall criterion is involved.
The native bridge overrides contact admission to 3 cm; the original 3 cm bench and the
separate 2 cm default bench retain their results. Earlier reports calling 2 cm the live
production gap were mislabeled; see `build/bmx/judging/contact-gap-bridge-clarification.md`.

Supported balance damping changed from 160 to 360 Nm per rad/s. Stiffness 1600,
balance torque cap 1200, global force/torque caps, supported admission and airborne laws
retain their values. The default-gap bench cuts +8 m/s release overshoot from
.10448 to .02539 rad and settling from 3.60 to 1.83 seconds. It is an analytical plane/contact
bench, not proof of native constraints. Abrupt reversals can have brief strict-support
losses and larger individual angular corrections. Manual/preload feel remains unproved.

Matched native trick runs 198/199 bind identical initial COM/quaternion/velocities and
all 541 raw-input/time/serial rows; the source manifests differ only in the damping header.
Run 199 earns the caught barspin/tailwhip combo, with strict post-landing support losses
10→0 and release/brake RMS angular rate .1788→.1004 rad/s. Peak landing angular rate
falls 1.996→1.780 rad/s and total torque 4000→2891 Nm, while peak summed normal load rises
16.54→23.38 kN. Hop rise and intentional air-pitch response remain close. This single
paired rehearsal does not establish controller feel, hard-impact safety or visual quality.
See `build/bmx/judging/upright-native-comparison-198-199/comparison.md`.

Run 200 passes natural ramp launch, landing and stopping: .516667 s flight, .450000 s
qualified natural flight, 27 passive angular intervals and 1.05957 m maximum COM rise.
Runs 199/200 use `1AFFC714...323C4B4`; the subsequent regression-only build/relink leaves
production physics unchanged. Latest-build run 201 passes bike→board→bike→dismount→carry→
set-down→remount, one allocation/three mounts. Run 202 passes native fall, typed GetUp,
standing and retained-bike remount. Source hashes and two teardown absence observations
are verified for every run. Original failures 196/198 remain intact.

The smaller private bike (radius .2630808 m), continuous coordinated pose, coasting crank
leveling and matching bike/human collision envelope are installed. Human bone lengths,
mass 106 and principal inertia 26/32/10 retain their values. The envelope evidence covers
named joint centers, not all visible skin or extended trick limbs. CPU grip/ankle agreement
does not prove rendered contact. Run 195's remap tail differs only on unused skateboard
indices; the active rider's final render publication remains unresolved.

Run 196's preserved continuous HDR recording has a canonical 840-frame native-10-bit active
extract verified against every decoded source sample. Independent visible scores range
5–8.5, including hand/foot gaps and rigid landing stance. That video predates this damping
change. Hard single-wheel impacts still show additional native contact impulses; friction
from rigid backup wheel shapes is a hypothesis requiring a supported API and controlled
native evidence. No guessed physics-material write, air upright lock or fall bypass is
installed. The 9.5 target and controller/animation acceptance remain unmet.

Earlier evidence follows. Read-only feature gates retain startup validation and sticky
revocation; runtime filesystem validation was removed from their repeated hot path.

Earlier runtime `C48A83E5B22C501A1AAAB9B55AE45F96FDD0607700EC8358772637F53630B40B`
preserved Skate's supplied native rider rotation
instead of imposing bike pitch and roll on the on-foot physical skeleton. Matched
private runs 180/181 measured early horizontal physical-skeleton/root gaps below
3 cm with the correction versus 17.39 m with the previous target. Normal-pose run
183 confirmed a maximum below 3 cm with the custom rider presentation enabled.
This measures two native landmarks horizontally, not complete visual alignment.

Run 183 passed native fall, typed GetUp, standing, retained-bike recovery and
completed remount, reusing one body allocation and advancing mount token1 to token2.
A separate capture gate now waits for the matching fall handshake and two fresh
standing observations. All 116 nonstandable trace samples had interaction capture
disabled. The earlier first-ragdoll capture failure in run 180 remains recorded.
Released-snapshot suspension on clock_changed was exercised in runs 180 and 183;
each later requalified the same retained body. Sources stayed unchanged during
these runs and both owned-process absence observations passed after teardown.

Equipment run 184 failed: carrying/set-down succeeded, but remount timed out.
Set-down had accepted two gentle wheel patches whose normals differ by 0.02418 rad,
outside the unchanged 0.02-rad remount coherence gate. Set-down now requires the
same measured final support as mounting and a clear approach. It searches at most
one of 17 bounded candidates per native step; unknown or exhausted proof retains
carrying. Ordinary walking step-off destinations and crash recovery are unchanged.
Run 187 passed the complete equipment route, reusing one body and completing its
third mount with 0.126 mm endpoint error. Its centre candidate was valid; the actual
184 rejected centre remains rejected in unit coverage, while selecting a later
candidate has not yet been observed natively. Run 186 made no mount because the
focus gate refused its requests. No support, clearance or slope limit was widened.

Private ramp run 185 passed and exported 64 owner/clock-qualified render-buffer
observations. All 395 bones in each solved/readback pair were byte-exact; maximum
composed palm/ankle target error was 0.053 mm. This does not establish the final
drawn pose or bike transform. The requested crank phase itself produced 475.5 mm
ankle-height stagger; retained toe animation also affects the shoe. These results
do not justify blindly changing the IK targets. Native render consumption remains
under investigation. The diagnostic stays disabled in the ordinary riding launch.

Ramp run 182 passed and its independent visual scores are 8.0–9.0; descending
hands still separate from the grips. Run 183's visual fall/mount scores are
6.8–8.0, with gaps in pickup/step-over coverage. Neither sampled capture establishes
normal-speed motion or physical-controller feel. The live far-fall controller route,
native owner-recreation handoff and all-category 9.5 acceptance remain unverified.
The game is closed after these private tests.

Earlier October 20 evidence:

Runtime `AEEBE24EEF418AB69A395F41C4AD9F1B13E4E4F6614A97FCAC6B631C8FA1D1FF`
passed all 57 Release tests and 94 producer evidence fixtures. Native run 178 passed
the strict fall / typed GetUp / standing / retained-bike recovery / completed remount
route with advancing force, velocity, animation and render counters. The same body
allocation was reused, token1 became token2, and the completed mount endpoint error
was 0.276 mm. Both owned-process absence checks passed and sources stayed unchanged.

Run 177 remains failed: synchronous diagnostic export preceded loss of the crashed
bike observation; a later fresh mount did not count as crash recovery. Released bikes
now withdraw control and presentation on narrowly classified transient snapshot
failures, then require fresh same-owner/world/body/configuration proof and quiet input
rearm. Invalid numeric data, ownership changes, configuration failures and producer
failures retain their strict rejection. Run 178 did not exercise this new suspension
branch, so its native interruption coverage remains outstanding.

The source-reviewed released-owner recreation handoff is built, but no native test has
yet witnessed the required actor context/rig replacement and later genuine remount.
It must not be inferred from the ordinary successful recovery route.

Stronger uphill presentation increases bounded forward chest/hip adaptation and
counterpitch toward a level world gaze, preserving the existing strict contact,
anatomy and fallback guards. Native ramp run 179 passed ascent, natural flight,
landing and stopping on this DLL, with 541 native render-pose updates and 100 genuine
sampled images. Hands still visibly separate from the bars during descent; the
independent pixel review is pending. The live fall displacement and all-category
9.5 acceptance remain unresolved. The game is closed after these private tests.

October 19 grounded F6 step-off and repeated equipment regression:

Runtime `BCF738A17690940735DB2E8C763850142DFF44B7430DC62B6EE7E98A3FC897AC`
passes all 53 Release tests. While riding, F6 now uses the existing stopped,
ground-supported step-off before requesting the skateboard. A bounded intent
pins rider/world/body token and requires a later offboard, standable observation
within 0.15 m of the endpoint, with parked ownership proved and motion released.
The normal native skateboard selector still owns equipping the board.

Moving, airborne, obstructed or incomplete-mount refusal continues the ordinary
validated ride/mount update; it does not hide the bike. Interruptions cancel the
intent and its step motion. An early cancellation proves the complete current
parked lifetime before republishing its model. No native physics state is forced.
Skate Dpad mappings and the stick mappings were not changed.

Runs 145, 146 and 147 each passed the eleven ordered equipment observations and
pause/resume on this same frozen DLL and source. Each used one pool/world and
three mount tokens, with a grounded step-off before verified skateboard state100
and the completed BMX return. Step-off endpoint errors were 0.011669373,
0.011552586 and 0.011552586 m. All test processes closed with two verified CIM
absences. Negative moving/air/cancel cases have source and pure-policy coverage;
these three native runs test the accepted route only.

Source review: `build/bmx/judging/clip-20261019-121602/grounded-f6-peer-review.json`.
Native aggregate: `build/bmx/judging/clip-20261019-121602/native145-147-review.json`.
Earlier failed runs 141 and 144 remain failed; the successes do not identify
their exact causes. The private GetUp pass below belongs to the preceding 8C
runtime. The live rider launch, grip/pedal gaps and same-world rider replacement
remain unresolved. Current-build controller footage and all-category 9.5 visual
acceptance are still outstanding. The game is closed after the regression runs.

October 19 controller recording review and mount/fall diagnostic iteration:

The controller-session recording `skate-2026-10-19_12-16-02.mp4` is reviewed in
`build/bmx/judging/clip-20261019-121602/scorecard.json`. Two independent judges
gave partial visual scores of riding 5/7, ramps 3/5.5, rider movement 3.5/5.5,
falls 2.5/4, and equipment transitions 4/4; tricks remain unrated. These sampled
stills do not provide normal-speed controller or complete scenario coverage.
The owner confirms that the large rider launch also happened during live play.
The one saved passive fall is local and reaches standing; three later histories
were dropped, so the live launch remains unresolved. Replay/event alignment is
also unverified. No category meets the requested two-judge 9.5 acceptance.

Runtime `8C6537765F049856D552C8E6C7FF6D021CE26CB20A13BEB05CF95FDBAB96232C`
passes 53 Release tests. It rejects terminal native-pool mount requests before
dismount/protection/model effects, reports getting-on until mount completion,
and returns the rider qualification from the same accepted body observation.
One new complete observation is allowed only for typed clock or exhausted ring
publication races; ownership, standability and capture checks remain required.
This corrects a sampling gap without establishing the cause of failed run 141.
Same-world rider replacement still requires restarting the isolated game.

Eight heap-owned FIFO slots now retain separate sealed fall histories and their
request/body/generation metadata. Full queues preserve existing reports; each
head has three write attempts. Lab analysis uses its own matched history.
Native run 142 passes the inverted private GetUp experiment with a 17.35-second
standing suffix and a 315-row, 30.02-native-second FIFO report, with no drops.
It does not test ordinary controller GetUp mapping or repeated native falls.

Native run 144 remains failed: initial mount/pause and skateboard state100
completed, but the return request stayed offboard/unstandable in state504 and
timed out before a second acquisition. Earlier run 141 failed its second mount
capture with an unclassified predicate and exit; run 143 passed the full ordered
equipment cycle on the preceding diagnostic build. Failures are preserved.
Normal switch/step-off behavior needs further work; this is not release approval.

October 19, reject changed native parent transforms before rider pose commits:

Runtime `D4C1784B6236C23F3F85578C70D459A5AEF9323EB581F5B46B658B0EDC7AC7DC`
passes all 51 Release tests. Riding pose commits now re-read the native hips
ancestors immediately before the first write. A changed or unreadable XYZ
translation/scale or quaternion refuses the complete scratch solution before
hips, rotations or entry-contact publication. Metadata W lanes remain outside
that comparison. The check uses bounded reads and validates the ancestor chain.

This closes a detected-change gap between solving and committing. It cannot
reject an already-stale unchanged parent, lock later native animation updates,
or make sequential pose writes atomic. No distance threshold or physical
alignment write was added. A separate diagnostic records the largest completed
riding hips-local XYZ norm in native local units. Low contact errors alone do
not establish a reasonable local hips offset.

Run 139 passed an inverted native fall/GetUp experiment on this DLL: one true
setter/readback at serial 720, one false reset/readback at 721, and a 16.80 s
final standing suffix within 30.02 native seconds. Run 140 passed the normal
ordered equipment/carry/set-down/remount and pause regression. Both runs had
zero guard refusals, so they verify the accepted path without demonstrating a
natural refusal. The maximum completed local hips norm was 0.667963 in run 139
and 0.352012 in run 140. Refusal behavior is covered by changed-ancestor,
invalid-chain and failed-read fixtures. Both test processes closed with two
verified CIM absences and unchanged source hashes.

Source review: `build/bmx/judging/riding-parent-guard-peer-review.json`.
Native evidence: `build/bmx/judging/native-getup-run139-review.json` and the
independently reassessed `build/bmx/judging/native-runs139-140-review.json`.
The archived `build/bmx/chassis-ride-run140` retains the equipment evidence. The preceding
three GetUp passes on runtime `7C1DEFAF...` remain a separate baseline below.

The native handoff investigation also established that the SDK clock listener
runs before native SkeletonResponse work; that serial is not a synchronization
completion fence. The native physical-rig copy is conditional. Context+830's
producer and the cause of run 132's large displacement remain unresolved; no
frame delay or synchronization setter is justified by the current evidence.
See `build/bmx/judging/handoff-after138-static-review.json`.

October 19, repeated native GetUp lab and report-stack correction:

Runtime `7C1DEFAFBD467FB93D66811268F7C92627EFC997A94D87367AC11681D16A5A51`
passes all 51 Release tests. Runs 135 and 136 (side impact) and 137 (inverted
impact) independently passed the private GetUp experiment on this exact DLL.
Each recorded a fresh allowed getter, one GetUp true setter entry/readback,
one false reset/readback on the next native clock, no pending reset or owner
loss, and a final native standing suffix of 16.98, 17.07 and 16.80 seconds.
The complete observation window was 30.02 native seconds in each process.
This verifies the explicit diagnostic action; ordinary controller recovery
mapping, full native script guard equivalence, and post-fall remounting in the
same process remain unverified.

The getter-only callback audit captured 184 candidates per run without drops.
Run 135's candidates used one binding across 16 nodes, and all reported GetUp
allowed. No binding-selection behavior was changed. Dispatch and subsequent
diagnostic getter clocks are measured separately; a later getter does not
establish what was available before action dispatch. Slot presence is unknown.
Evidence: `build/bmx/judging/native-getup-runs135-137-review.json` and
`build/bmx/judging/native135-getup-binding-evidence.json`.

Run 134 exposed a stack overflow while flushing the new large audit report.
Its three crash dumps and matching PDB are preserved under
`build/bmx/native-getup-run134`. The trace copy now fills a heap-allocated caller
buffer instead of returning a large object by value. Runs 135–137 completed
that report path without a crash. Run 134 remains a failed trial despite its
saved trace containing standing observations.

Early handoff telemetry now retains raw uint32 words and finite XYZ/translation
separately. This recovered context+830 positions even when the fourth lane
contained `0xFFC00000`; the fourth lane's semantics remain unknown. Read,
owner, protection and native-clock revalidation still apply. No synchronization
routine, transform correction, release delay or mass change was introduced.

Run 138 passed the separate normal ordered bike/skateboard/park/carry/grounded
set-down/remount and pause regression on the same DLL. This is separate from
the GetUp trials. Every process was closed with two verified CIM absence
observations, and source hashes remained unchanged during each test.

Run 132's 14.635 m initial ragdoll displacement remains unresolved. It did not
recur in the recent dense traces, which does not establish a fix. Its missing
early serials prevent causal ordering. Current visual judge scores remain
unavailable; the requested 9.5 in every category has not been established.

October 19, explicit GetUp lab and early ragdoll discontinuity:

Runtime `86501463B644897429381542412574BC1D38085727ED8E4D6487DD8F41D0E452`
passes all 50 Release tests. A separate private `-GetUpPulse` mode can request
one typed GetUp intent, with fresh settled-down preflight, current callback
readiness and later-clock reset requirements. Ordinary controller mappings do
not enable this diagnostic. The SDK ownership mirror and clock reader used by
the action avoid waiting for their writers; the existing NoBail owner inspection
still briefly locks its SDK lease copy, before any native call.

Run 132 failed the GetUp experiment. Its fresh preflight armed request 1 at
serial 720, but the selected current callbacks did not return IsGetUpAllowed=true
within the 250 ms start window. No GetUp setter was entered (`true_calls=0`),
there was no reset obligation, and no standing was observed in the full 30-second
trace. The exact owned process was closed with two CIM absence observations and
unchanged archived source. The independent failure review is
`build/bmx/judging/native-getup-run132-review.json`.

This does not prove every native binding had a closed readiness gate. Source
review found that generic pulse serial reservation precedes gate qualification;
an unbound/default-false first callback could suppress another binding on that
same native edge. Existing semantic rows omit binding/node identity. That
selection gap is uncorrected and remains distinct from the observed failure.

More directly, the raw rider root displaced 14.635 m by the first post-fall
sample, serial 40, only 0.100 native seconds after the initial sample. Run 130
displaced 0.167 m over that interval. All 301 bike-body rows match numerically
between the two separate processes, while the final rider roots differ by about
16.9 m. The discontinuity precedes the GetUp request. Its native handoff cause,
physical-rig alignment and effect on recovery are under investigation; it is
not attributed to GetUp writes or bike mass on this evidence.

October 19, fallen-bike switching and passive recovery trace:

Runtime `339503E98EAE0D578EF0AB51A1FEBC5A88970C6E348B5AA8FE2F83404E89E480`
passes all 48 Release tests. F6 now checks a fresh canonical rider, matching
full retained body identity and native clock before removing a crashed bike.
While the rider is down, the request keeps the bike visible, releases BMX
capture/motion and starts no skateboard timer. An accepted request pins its
rider owner across cleanup and the subsequent native skateboard transition.
Changed owners or body lifetimes cancel rather than becoming a new bike spawn.

Run 130 passed ordinary native fall entry and the separate `RecoveryGuard`
scenario. The refused F6 retained eight parts, one allocation/mount and the same
body token. Its 301 passive samples cover 30.016665 native seconds with zero
snapshot failures or overwritten rows. BMX drive, motion, interaction and full
input leases and the board timer remained off; the completed velocity, force,
pose and render counters did not advance. This is a guarded-switch pass;
`recovery_passed` remains null. The rider stayed nonstandable in native ragdoll.

A separate canonical callback trace read both exact GetUp assets in 300 rows.
GetUp was false throughout; IsGetUpAllowed was true in 295 rows and false in
five. Binding-slot presence is unknown. These rows retain their own clocks;
they are not fabricated same-tick joins with the body samples. No native get-up
action was sent, so the run cannot establish automatic recovery or a blocked
get-up gate. See `build/bmx/judging/native130-getup-semantic-evidence.json`.

Run 131 passed the ordinary ordered bike/skateboard/park/carry/grounded-set-down/
remount regression on that same runtime, including the same-pool pause test.
Both owned processes were closed with two CIM absence observations and unchanged
archived source. The independent technical reviews remain separate from visual
acceptance: no current category has a new 9.5 score from these runs.

October 19, mounting transition update:

Runtime `7365B251B68B9F153BF4FFA6F8361AB3A0DEED806E0E601E85D0BB221DE6F401`
adds gradual native-root rotation and phased grip/foot placement. It captures
the standing rider only after chassis acquisition, with matching owner, body
handle and native clock. Duration uses approach distance and angle, with a
0.8-second minimum. World-space contact anchors and native segment lengths are
preserved; intermediate reach projection may adjust the presentation pelvis by
up to 0.35 m. Partial mounts cancel on interruption, and publication revisions
guard competing animation/render callbacks. Returning to a parked bike verifies
the previous handle and a fresh mount token instead of demanding the old token.

Mounting now requires standing within 0.70 m; pickup keeps its 1.8 m reach.
Refusing a distant mount preserves the parked bike so the player can move closer
and retry. The fixture uses the real root timing and tests both sides, three
headings, 0/90/180-degree turns and 0.12/0.65/0.70 m approaches. Maximum sampled
intermediate palm/ankle error is 0.164 mm. These geometry checks are not a visual
score or proof of whole-leg/shoe/saddle clearance or natural entry joint angles.
The transition starts from rest; arbitrary walking-to-mount velocity continuity
is unverified.

The Release build and all 46 tests pass. Independent source review is preserved
at `build/bmx/judging/mount-transition-peer-review.json`. Current native results
are recorded separately below; source fixtures do not prove native entry quality.

Run 128 passed the ordered native bike -> skateboard -> bike -> park -> carry ->
grounded set-down -> remount sequence, including the same-pool pause test. All
three ordinary mounts logged completion. The final approach measured 0.650027 m,
used 1.250052 seconds and completed at serial 2622 with 0.000143 m observed root
error. Three fresh mount tokens reused one chassis allocation. The exact owned
process was closed with two CIM absence observations and unchanged source. This
proves a bounded equipment workflow and target following, not natural live
anatomy, absence of clipping or controller feel.

Run 129 on the same runtime passed the ordinary native fall handoff: an accepted,
unprotected request 1/generation 1 wrote true at serial 34, reset at 35, observed
typed ragdoll, and retained eight detached bike parts with drive off. It recorded
zero snapshot retries. Its recovery scenario failed. The one explicit interaction
at 11.333 native seconds found `standable=false`, `offboard=true`; the later
skateboard switch timed out. The zero distance/height/speed fields in that early
rejection are defaults, not measured geometry. No native get-up input was issued
and no second fall was attempted. The supervisor reached its 200-second bound,
closed the exact process and verified two absence observations and unchanged
source. This is a recovery failure, not a game-process crash or proof of the
cause of missing get-up. Fresh controller get-up footage remains necessary.
The independent bounded native review is
`build/bmx/judging/current-mount-native-review.json`; it explicitly retains the
recovery failure and leaves all current visual acceptance scores null.
The source cleanup audit, `build/bmx/judging/post-fall-control-release-review.json`,
found no evidence of sustained BMX rider actuation, IK or capture after release.
It does not establish native get-up input semantics or the cause of nonstandability;
body flags and native animation states were not changed speculatively.

October 19, controller recording and fall bootstrap correction:

The new two-minute recording, `skate-2026-10-19_00-04-10.mp4`, belongs to manual
Run 122 on runtime `33010B793D77B116537476C08F13C883143AFCD7B22F9B3C55798E4D4BB4412F`.
The previously attached October 18 file is unchanged historical footage. Two
independent judges reviewed bounded timestamp samples of the new file. Their
partial diagnostic scores are 4-5.5 for ramps/landings, 6.5 for rider movement,
3.5-4 for falls/recovery and 6 for equipment transitions. Steering has one 8 and
one unrated result; the full trick category is unrated. These are incomplete
coverage scores, not formal acceptance. The scorecard and original independent
reports are preserved under `build/bmx/judging/current-clip-122-*`. Full-speed
playback, synchronized inputs and live-versus-replay equivalence were not proved.

All three ordinary crash requests in that session were unavailable. Its isolated
working profile had saved `ReSkate.NoBail=true`, restored by the manual protection
owner after startup. This is a high-confidence explanation, not proof of the
protection bit at each old request. Ordinary `-Ride` startup now runs the normal
`nobail 0` command after saved settings restore, queries the result, then mounts.
Run 123 proved saved true -> normal command acknowledgement -> inactive
protection -> eight-part active mount -> saved false. The runtime continues to
respect manual or flight protection subsequently enabled by the player.

Runtime `E895DCCBF96DB3DA048E966DFF245E8B090C158E2D5932B594E5BD1B6D4A98BF`
passed 45 Release tests and native Runs 125-127. Run 125 verified a fresh,
unprotected accepted fall request, matching request/generation receipt, one true
write at serial 34, reset at 35, both readbacks, native ragdoll and all eight bike
parts detached with drive off. Runs 126 and 127 passed bounded flat-ground and
prepared-ramp rehearsals. Each owned process was closed with two successful CIM
absence observations and unchanged archived source. Independent evidence review
is `build/bmx/judging/native-profile-and-snapshot-fix-review.json`.

Run 124 remains a failed attempt: the old combined snapshot error stopped the
ride before impact, and its exact cause was not classified. Snapshot reads now
report typed failures and retry only a fully readable, valid ring publication
change, at most three reads in the same native observation. Owner, configuration
and native clock are revalidated; invalid state still fails immediately. Run
125 recorded 34 reads and zero retries, so recovery from a real native publication
race has not been demonstrated. Terminal trace records now distinguish unsampled
wheel contacts and retain measured native timing and fall-request diagnostics.

These functional checks do not establish sustained falls, natural get-up,
repeated recovery, smoother mounting or controller feel. All visual acceptance
scores for this latest runtime remain null; the 9.5 gate has not been met.

The BMX quality gate is now defined in `judging/rubric-v1.json`: two independent
gameplay judges must each award at least 9.5 in all six categories. Missing
coverage stays unrated; averaging and source/test-based visual scores are not
allowed. Controller feel still needs the owner's physical-controller check.
The independent historical scorecard under `build/bmx/judging` gives the old
recording partial ranges of 3-4 for its landing, 4-5 for posture and 2-4 for fall
presentation. All current-build scores remain unrated pending current footage.

Run 118 (`19C2EF1BAFD65D07F08DF9DA58CBA19A32574E0626A8ED3E1C6859677EB3B124`)
verified the ordinary-riding native fall adapter with the passive observer and
private intent activation flags both off: generation 1 wrote once at serial 34,
reset once at 35, verified both readbacks and observed native ragdoll. All eight
bike parts remained detached with drive disabled. A deferred 34-step pre-impact
trace was archived. This proves the first ordinary fall, not repeated falls,
get-up/remount, visual quality or controller feel. The test closed its exact
owned process and recorded two successful CIM absence observations.

The latest steering controller bounds ground fork angle using measured speed
and projected tire heading within its supported lean range. This fixes a
reproduced high-speed equilibrium mismatch without capping speed or changing
air controls. Portable integration fixtures passed; broad native handling and
current gameplay judging remain pending.

The previous integrated runtime
`33010B793D77B116537476C08F13C883143AFCD7B22F9B3C55798E4D4BB4412F`
passed all 44 Release tests and native Runs 119-121. Run 119 repeated the first
ordinary fall with an accepted request ID and matching generation receipt;
rejected/stale results cannot satisfy that report. Run 120 completed the measured
ramp route (0.467 seconds airborne, 0.999 m rise, 0.067 seconds to supported
landing, then stopped). Run 121 passed the flat acceleration/steering/braking/hop
rehearsal. Each exact owned test process was closed with two absence observations
and unchanged archived source. These are functional regressions, not visual
ratings, broad terrain coverage or a repeated-fall/get-up acceptance.

An optional isolated ride trace retains the last 240 accepted native steps per
mount in fixed storage and freezes on termination. Serialization is deferred
while another BMX interaction is active. Captured adapter input, private
rehearsal/switch overrides and neutralization gates are labeled separately.
No per-step JSON or file writes occur during riding. Independent review repaired
active-remount IO, input-provenance and stale-request test-result defects before
the final native checks.

Upstream synchronization (October 18): `BMX` now includes all 79 commits through
`fffd34ca8035582fa6307e8104c6a7cab350db18` from `origin/main`. Six textual
conflicts were resolved by preserving both upstream functionality and BMX/profile
isolation additions. The pre-update files and binary patch are retained under
`build/bmx/upstream-sync-20261018`, together with the recoverable Git autostash.
The new replay-keyframe settings and GPU breadcrumb counter also use the existing
development-root helper.
The new native build and validation follow below; older run hashes identify the
pre-update source and must not be mistaken for validation of this upstream state.

Run 116 on the updated upstream source verified the explicit native fall for the
first time: the typed ragdoll state was observed, the Bool request was written
once at native serial 34 and reset once at serial 35, with both readbacks
verified. The old Offboard request leaf was not called. Eight bike parts remained
after the crash. The independent recovery sequence did not pass: the rider was
still non-standable when the scripted interaction ran. Native Y/Triangle had
also been suppressed by crashed-bike interaction capture. That capture now waits
for a fresh standable offboard rider, leaving native get-up input available.
That run used the explicitly enabled private pulse. Run 118 above subsequently
verified the ordinary-riding adapter; repeated falls and get-up/recovery
validation remain outstanding.
Manual controller Run 117 contains the upstream, posture, landing, persistent
crashed-bike and input-pass-through changes, without synthetic input or the
experimental pulse. The build and all 42 BMX tests passed before the final
capture-gating change; its four focused input/interaction/recovery tests passed
again afterward.

The player's October 18 recording showed a cramped riding pose, a sideways
stair landing and the bike disappearing during the subsequent fall/recovery.
The posture now uses the source rig's actual proportions: neutral torso lean
is about 33 degrees instead of 56, with roughly 10 degrees of wrist bend instead
of 57. Grip, pedal and trick-release targets remain unchanged. The real-rig
fixture includes contact accuracy and anatomical angle checks; GCC and MSVC
checks pass.

Landing assistance now uses the measured surface face normal independently of
the radial tire-to-edge collision normal. Physical contact forces still use the
radial normal. This removes an erroneous bank command at stair/lip edges without
changing contact eligibility, force limits or bail thresholds. The original
recording has no synchronized input/force trace, so this is a reproduced control
defect, not proof of that recording's exact cause.

Run 114 on runtime `63372F0A7D53BD27CB0F52780C602D2B023874F1F48645FA27F4DD4B71D5CB73`
verified the new detached-bike crash path: all eight native parts remained,
riding force applications stopped at 33, and the dynamic bike tumbled and settled
while the native rider recovered to a standable state. The rider's pelvis/torso/
head collision shapes are disabled on the detached bike; its five bike shapes
retain native world collision. Recovery is an explicit nearby interaction after
both rider and bike settle, followed by fresh ground/clearance checks. The
detached body currently retains the mounted mass, inertia and COM approximation;
it is not yet a separately calibrated bike-only rigid body.

The overall Run 114 native-fall check still failed: the explicit ragdoll trigger
has not been established. Its read-only impact probe did capture fresh canonical
Bool-action binding callbacks throughout the crash window. It made zero intent
writes or activation calls. This evidence must not be presented as a completed
fall-animation implementation. All 42 current BMX tests pass; native pixels,
controller feel and recovery interaction still require their own verification.
See [the impact evidence](../../build/bmx/native-impact-run114/bmx-impact-test.json).

Run 115 passed the ramp route on the same build, including 19 edge-feature wheel
observations, 541 measured steps and rider render updates, 0.467 s flight, 0.999 m
maximum rise and a final speed of 0.00045 m/s after braking. The new measured
surface-face and separated torque fields are present in its samples. This is
native ramp/edge regression evidence; it does not reproduce the player's exact
stair approach or establish controller feel.

Current replacement-model acceptance: Run 103 passed riding and a Superman
landing, Run 106 passed the ramp route, and Run 107 passed the complete equipment
sequence. Run 107 observed all 11 ordered phases, three mounts with one allocated
physics body, quiet off-bike force counters and successful pause/resume. These
are automated native-state and pose-write checks. Player visual inspection,
controller feel and complete native fall/recovery animations remain outstanding.
The [independent equipment review](../../build/bmx/chassis-ride-run107/independent-interaction-review.json)
cross-checked every phase against the owned process's log. Its board interval was
2.054 seconds; carrying lasted 2.049 seconds with 122 render updates and no new
riding force applications. The final mount resumed native force applications.

The supplied `bmx-free` replacement passed its first isolated native ride and
catalog rehearsal in Run 103 on runtime
`CCA0787320266894AA61EF9BCBF99D263B3A7BAB80392B5AC4AC89FE3650E463`.
The mount succeeded on its first attempt using fresh wheel contacts on the actual
gently curved ground. Final front/rear contact gaps were 0.030/0.009 mm. The
supervisor verified the report's process identity, unchanged sources and two
process-absence observations after teardown. This establishes the tested native
movement/landing path; visible appearance still needs player inspection.
See [the native report](../../build/bmx/chassis-ride-run103/bmx-selftest.json).
The [independent review](../../build/bmx/chassis-ride-run103/independent-catalog-review.json)
passed all 24 checks: 541 accepted native steps over 9.0167 native seconds, 541
rider render writes, eight peak Superman pose observations and clean credit after
66.7 ms of actual landing support.

Run 104 could not exercise equipment switching because its ordinary input gate
required a focused window. It timed out with zero chassis allocations. The private
switch rehearsal now uses explicitly selected, neutral synthetic BMX input with a
nonrenewable 200-second wall lease. Menus, capture ownership, solo eligibility and
native ownership checks still apply. Ordinary controller play retains its focus
requirement; native walking and Skate's D-pad handling remain unchanged.

Run 106 passed the replacement bike's ramp ascent, natural launch, landing and
braking rehearsal on runtime
`68A787240BAB3CAB08812021D8D2CB9C7527D85327D32D49BD1076198543DBDE`.
It measured 0.450 seconds of flight, 0.066667 seconds of verified landing support,
0.999 m maximum chassis rise, 541 accepted force applications, 541 rider render
writes, eight live parts and zero backend rejections. The supervisor verified
report identity, unchanged sources and two process-absence observations.
See [the ramp report](../../build/bmx/native-ramp-run106/bmx-selftest.json).

Run 105's pause test and native skateboard handoff passed, but the full equipment
sequence did not. Its first remount rejected a front-wheel penetration candidate;
later retries rejected a 0.3076-radian floor slope. The native board had moved about
19.8 m from the initial spawn during the long observation interval. The cause of
that travel was not established. The next fixture uses two board observations
two seconds apart instead of eight, retaining every native state/contact check.
See [the failure review](../../build/bmx/chassis-ride-run105/independent-switch-review.json).

The replacement is installed in the isolated test copy as
eight parts, including separately articulated pedals. Its wheelbase and rider contact
anchors share the measured source geometry. Run 100 on runtime
`A20880B2C0FEC2E2398F04043062EF3430FDB0271893BDA5EA874EADA89AE5B0`
recorded eight live parts, 2,616 placements, 326 native force applications and
326 rider render updates at its first status snapshot. Its subsequent automated
rehearsal was rejected because BMX was already mounted; the supervisor timed out
and verified process teardown with two absence observations. This is a native
load/pose checkpoint, not a passed riding scenario or visual acceptance.
See [model conversion and limitations](../../assets/models/bmx/README.md).

Runs 101/102 exercised the new terrain-aware mount guard and stopped before body
allocation. Run 102 measured valid local planes under both wheels, but their
cross-wheel plane residual was 6.16 mm; the initial helper incorrectly required
global coplanarity within 2.5 mm. Its diagnostic report preserves the actual points,
normals, root, world/context and native clock. This is a rejected mount, not a crash
or accepted ride. The revised mount solver fits each wheel's own local plane and
then queries both final hub positions again. It retains the final gap, world,
native-clock and total-tilt guards. Run 103 accepted this corrected solve. The
previous 39 regression tests passed before these native runs, and the revised
mount fixture passed GCC, MSVC and CTest before Run 103.

Run 101 also captured the native wipeout-intent registration through a bounded,
read-only observer. Both control and target descriptors were stable in all 32 stored
samples per key. Static inspection of the observed target found a runtime-node
configuration adapter, not a direct ragdoll trigger. Exact native registrations
now establish its Begin/Update/End lifecycle: Update writes the resolved Bool
property, while End applies authored reset rules. The concrete property setter's
static implementation is now identified; the canonical current-rider context
and observed binding lifetime still require proof before activation.
See [the lifecycle checkpoint](../../build/bmx/bail-lifecycle-v13-checkpoint.md).

Run 108 retested the inverted impact with the replacement bike and an opt-in,
passive observer of the native Bool-intent Begin/Update adapters. The body guard
detected an upside-down rider contact at 4.57 m/s closing speed and deactivated
the chassis. Native ragdoll activation still failed because the rig remained
animated; the adapter made zero activation calls. The observer forwarded 1,496
Begin and 9,736 Update calls, but all snapshots failed its node-coherence checks.
These counts prove intercepted execution, not a validated rider/Bool binding.
The exact mismatch needs a bounded rejected-header diagnostic before any native
activation change. See [the impact result](../../build/bmx/native-impact-run108/bmx-impact-test.json).

Run 109 isolated that observer failure: all 32 retained rejected headers had a
readable node with vtable `0x6178cf0`, rather than the analyst-selected candidate
`0x6178d40`. The observed adapters still use a node pointer through their first
argument; changing that ABI is unsupported. The matching original constructor
and destructor establish a 120-byte node, and the passive reader now uses that
exact type and extent. A regression fixture makes the following eight bytes
unreadable, catching the old 128-byte read. The impact again stopped the chassis without
activating ragdoll, and the supervisor verified teardown and unchanged sources.
See [the captured headers](../../build/bmx/native-impact-run109/bmx-intent-observation.json).
The correction's [independent review](../../build/bmx/native-impact-run109/independent-observer-correction-review.json)
and focused MSVC/CTest checks passed before the next native capture.
The [native caller review](../../build/bmx/bail-animated-guard-review.md) also
confirms that the reviewed ragdoll-state Entry methods do not activate the rig;
the animated-mode guard remains necessary on the proven pending-state path.

The expanded interaction/catalog checkpoint is Runs 95/96 on runtime SHA-256
`B0936B896B11C926764FBF3D2F83D76BE4967299C2F2039C0B9B761085434DC4`.
Run 95 passed all 11 sampled switch/park/carry/set-down/remount phases, same-body pause/resume
and three mounts with one allocation/world. The fresh mount tokens advance 1 → 2 → 3;
parking, carrying and set-down retain the current token. Off-bike chassis force/follow counters
remain unchanged while six native parts stay present. Its carried interval produced 244
successful pose writes and 122 render writes over 2.032 seconds, with sampled hand-target
error at most 0.000031 m. See the [independent switch review](../../build/bmx/chassis-ride-run95/independent-switch-lifecycle-review.json),
[carry review](../../build/bmx/chassis-ride-run95/carry-pose-review.json) and
[identity/teardown record](../../build/bmx/chassis-ride-run95/run.json).

Run 96 passed the first expanded generic catalog rehearsal: Superman, authored slot 7.
LB + RT + right-stick up-left selected it after the ordinary physical hop; neutral recovery
completed the catch at 6.900 native seconds, before actual wheel contact at 7.283 seconds.
Clean credit followed at 7.350 seconds after 0.066667 seconds of support: one Superman and
one combo, zero failures. The runtime measured eight distinct successful owner-matched native
render observations at presentation phase ≥ 0.9; the complete 9.0167-native-second rehearsal
recorded 541 render writes, 541 native force applications and zero backend rejections.
See the [native report](../../build/bmx/chassis-ride-run96/bmx-selftest.json),
[independent temporal/contact review](../../build/bmx/chassis-ride-run96/independent-catalog-review.json)
and [identity/teardown record](../../build/bmx/chassis-ride-run96/run.json).
The peak observations are instrumented native render writes, not a separate pixel capture.

Both runs have unchanged archived sources and two confirmed process-absence observations.
All 58 nonempty catalog selections now have source-tested request/pose/recovery/credit paths;
native acceptance covers this Superman and the earlier barspin/whip paths, not every slot.
These earlier runs used the previous six-part bike. Visible rider/bike quality,
physical controller/D-pad input, inverted-bike clipping and complete native BMX fall/recovery
remain open. The following ramp/input records are earlier checkpoints, retained with their
original runtime identities.

Run 97 repeated the original barspin/whip/pitch rehearsal on runtime
`F3971A887608330394D20080385F2897C88232788739F1DB1013279610A25100`: one caught
barspin, one caught tailwhip, one landed combo, zero failed combos, and a next-step left-stick
pitch response of 0.035322 rad/s. Its 9.0167 native seconds took 9.109 wall seconds.
See the [Run 97 report](../../build/bmx/chassis-ride-run97/bmx-selftest.json).

Runs 98/99 on that same runtime are **failed native-fall diagnostics**, not clipping or fall
acceptance. The inverted test detected a head-region guard contact at serial 30 with
4.57189 m/s closing speed; the side test detected a frame-region contact at serial 34 with
5.22502 m/s closing speed. Reconstructed native-response residuals were 424.195 and
101.730 N·s respectively. Both recorded `chassis_active=false`, but the native rider remained
animated: zero native calls, no consumed fall request and no observed ragdoll in either test.
The exact rejection was “Native rig remains animated; activation contract unavailable.”
See the [combined independent review](../../build/bmx/native-impact-runs98-99-independent-review.json)
and [inverted](../../build/bmx/native-impact-run98/bmx-impact-test.json)/
[side](../../build/bmx/native-impact-run99/bmx-impact-test.json) terminal reports.
Fresh contact classification, collision response and released bike control do not establish
a complete swept rider collider, no clipping, visible fall animation or recovery.

All three runs preserve unchanged archived sources and two confirmed process absences.
Their original interim “released to native fall handling” text overstates the rejected handoff;
the final unavailable message and zero native calls govern the result. The later build
`70B42E7CF56D1241B56098BDE1729A805CC17EF373E0E309FECAD6F405EF5B8C` changes that
interim text only to “bike control released”; it has no new native acceptance run.

Run 87 passed one isolated, measured native ramp route: supported ascent, .483333 seconds of
independently checked gravity-driven flight, a landing with both wheels settled for .066667
seconds, then braking to .001615 m/s. The actual bike root stayed within the 5 m route limit,
reaching 4.871706 m. Root braking distance fell from Run 86's 1.331261 m to 1.044715 m with
the bounded joint brake/contact solve; the observed chassis state and commands before braking
matched the previous run. Its 9.0167 native seconds took 9.093 wall seconds, and the mounted
performance snapshot measured 59 client updates and 60 rendered frames per second.
See the [Run 87 report](../../build/bmx/native-ramp-run87/bmx-selftest.json),
[independent motion review](../../build/bmx/native-ramp-run87/independent-final-motion-review.json),
[braking comparison](../../build/bmx/native-ramp-run87/independent-run86-run87-braking-comparison.json)
and [identity/source/teardown record](../../build/bmx/native-ramp-run87/run.json).

The same runtime passed Run 88's Streets-binding rehearsal: one barspin, one tailwhip and one
landed combo, with no failed tricks. Left-stick Y pitch increased angular speed by .035322
rad/s on the immediately following native step. Its 9.0167 native seconds took 9.313 wall
seconds. See the [Run 88 report](../../build/bmx/chassis-ride-run88/bmx-selftest.json),
[independent controls review](../../build/bmx/chassis-ride-run88/independent-streets-control-review.json)
and [identity/source/teardown record](../../build/bmx/chassis-ride-run88/run.json).

Run 89 also passed guarded pause/resume and the ordered BMX → native skateboard → BMX →
walking → BMX cycle. Pause/resume measured zero position/quaternion drift; all three mounts
reused one native allocation. Two board snapshots 8.05 seconds apart had zero bike parts,
released capture, a parked chassis and the same 578 force applications. See the
[lifecycle report](../../build/bmx/chassis-ride-run89/bmx-chassis-lifecycle.json),
[sampled switch evidence](../../build/bmx/chassis-ride-run89/switch-evidence.json) and
[identity/source/teardown record](../../build/bmx/chassis-ride-run89/run.json).
These are ordered sampled states, not continuous ownership or physical F6 verification.

All three archives identify runtime SHA-256
`B9328A21E53C857BE4ED9E2E8596D1CCA6770C3B9F7E29E962A84D80CF8B9E5D`, unchanged
sources and verified process teardown. These were synthetic rehearsals, not physical
controller, D-pad, rider-appearance or full BMX Streets parity acceptance.

`Start-BmxGame.ps1 -Ride` selects the physical backend automatically; other diagnostics can
opt in with `-NativeChassis`. Run 95 verifies the current same-body pause/resume and expanded interaction cycle.
Run 87 establishes that one measured ramp ascent/flight/landing/stop route; broad terrain,
landing, controller-feel and collision acceptance remains open.
Tire appearance remains postponed. See [physics evidence](physics-backend-investigation.md)
for the implementation and measurement boundaries.

Branch `BMX`; supported Steam build `25414733`. Updated 2026-10-18.
Run 54 passed a native mount/render smoke with the corrected grip anchors and compact-basis
encoder. It did not run synthetic riding or equipment switching. Run 53 separately passed
pedal/steer/brake/hop and BMX → native skateboard → BMX → walking → BMX. The user's Run 52 screenshot and recording rejected both bike and
rider appearance. After Run 53, the user confirmed hand holding works but clarified that the
grips themselves float below the actual bars; tire shading also remains wrong. Functional
native measurements are not visual acceptance. Complete physical-control acceptance remains open.

The [preserved still](../../build/bmx-visual-feedback-20261017/before.png) and
[video contact sheet](../../build/bmx-user-video/contact-sheet.jpg) show both wheels, pale tire
bands, a compressed riding crouch, high/outward elbows and open hands hanging past the grips.
Rider source corrections now target palm contact, mirrored wrist orientation, real finger
chains, lower elbows and a higher neutral stance. The bike appearance source now supplies flat
interior atlas UVs and directly encoded compact tangent bases. Run 53 did not resolve the
floating-grip geometry or tire appearance.

The attachment correction now derives both grip sleeves and hand targets from
[shared authored anchors](../../Extension/BMX/bmx_grip_anchors.inc), about 44 mm above the old
guessed placement. Python uses the same outward endpoint/inward axis to build each 100 mm
sleeve, and hand orientation follows the actual swept tube axis. The
[triangle-slice check](../../build/bmx-native-grip-attachment-tests.json) finds the source bar
inside the grip at 10, 50 and 90 mm along both sleeves, and rejects the old floating placement,
a wrong axis and detached grips. Run 54 mounted and rendered the corrected pack. This is
geometry/callback evidence; actual user images remain required.

The earlier material comparison kept an A pack with new atlas UVs and Run 52's tangent bytes, and a B
pack that also selects existing native basis codes by decoded normals and smooths wheel
corners within a 50-degree crease. Both retain mesh positions, indices and section/category
layout. The B decoder matches the published [Frosty axis-angle shader](https://raw.githubusercontent.com/CadeEvs/FrostyToolsuite/1.0.6.3/Shaders/UnpackNormals.hlsl)
and its [UByte4N reader](https://raw.githubusercontent.com/CadeEvs/FrostyToolsuite/1.0.6.3/Plugins/MeshSetPlugin/FrostyMeshSetEditor.cs).
The [authored-normal comparison](../../build/bmx-native-assets/material-audit/donor-source-normal-comparison.json)
resolves the earlier decoder uncertainty: all 2,386 Frame vertices match source positions and
exact half-float UVs, with decoded/source normal dot minimum 0.999852 and mean 0.999994.
The source normals themselves have weak face-normal alignment. Donor normal decoding is
confirmed. Run 54 replaces coarse donor-code selection with an inverse encoder, independently
checked against literal shader decoding for 6,224 inputs and all 125,820 generated vertices.
The generated maximum normal/tangent errors are 0.1748°/0.1765°, with handedness and reverse-face
bases checked. The earlier A/B packs remain historical controls; they predate the new grip fit.
Shaders and masks are unchanged, and mask-channel meanings/rubber roughness remain unresolved.
Precise normal encoding does not establish accepted tire appearance.

## Current implementation

- One independent native dynamic body is configured for 106 kg and local inertia
  `{26, 32, 10}` kg·m². Its measured COM, quaternion and velocities drive the bike;
  the bridge does not integrate a replacement pose or clamp its heading/speed.
  These are prototype choices, not recovered effective BMX Streets parameters.
- Two query-driven rolling-wheel constraints use .34 m radii and measured terrain patches,
  including bounded finite-edge/radial witnesses. Pedal, brake, steering/balance and hop
  outputs become native forces/torques. Proposed torque limits are 75 N·m rear pedal,
  60 N·m front brake and 45 N·m rear brake. Forward pedal torque fades from 6 to 8 m/s using
  COM/rim speed; this is a propulsion taper, not a 6 m/s velocity cap. Gravity, momentum,
  rotation and backup collision response remain native.
- The [controller](../../Engine/Game/BMX/bmx_chassis_control.h) couples brake, rotor and
  longitudinal/lateral traction in a [bounded constraint block](../../Engine/Game/BMX/bmx_coupled_brake.h).
  Normal rows and the joint tangent/brake block use 96 brake-active sweeps; coasting
  retains the original 12 sweeps. This is numerical convergence, not multiplied actuation:
  motor torque is applied once, each brake impulse stays within torque × native step, and
  one shared cap fraction scales the submitted brake/contact body and rotor response.
  The solve does not clamp measured speed or add artificial braking drag.
- Force commands are leased and consumed at most once per fresh native physics serial in
  the verified local retail `SynchronizeBody` path. The adapter is calibrated for the
  observed 60 Hz step only; other timing, stale ownership or unavailable queries stop/park
  it. This does not establish a global physics-worker barrier.
- Three attached capsule shapes provide coarse wheel/frame impact backup. They are not
  freely spinning native wheel bodies or a full bike collision mesh. Logical wheel support
  is separate from the smaller backup capsules; excessive penetration fails closed.
  Queries exclude the verified chassis/local rider, but moving-surface velocity currently
  assumes zero and broader collision/filter behavior remains unvalidated.
- The verified on-foot Skate actor follows the measured chassis through the guarded native
  motion path. Identity, physics-state, native-thread and ownership checks remain mandatory;
  this rider-follow path is separate from the chassis force solver.
- Six client-owned object blueprints render the frame, fork, handlebars, wheels and cranks in
  the native world. Creation, placement and cleanup run on the verified client context; stale
  generations and wrong-realm cleanup are guarded. Missing model content blocks movement.
  Overlay bike drawing is disabled.
- Rider IK uses the actual Skate skeleton and preserves its proportions.
  Feet follow crank phase, palms/finger chains target grips, and bounded posture cues respond to preload,
  front/rear weight and airborne state. Pose edits run through animation/render handoffs with
  ownership checks and expiry. This is an approximation informed by recovered clip cues,
  not a full Unity humanoid retarget or copied bike physics.
- Xbox and native PlayStation input, plus keyboard fallback when no pad is available.
  Both render-part matrices and rider targets follow the full measured chassis quaternion.
  BMX stays unavailable in multiplayer sessions. Native D-pad actions pass through BMX-only
  capture; menu/freecam/prompt capture still takes priority. Physical D-pad testing remains open.

The earlier 6 m/s-forward / 3 m/s-reverse kinematic adapter remains available for legacy
diagnostics without the native-chassis option. It is not the backend selected by `-Ride`.

Native world rendering provides the engine's normal depth/render path. Actual mesh appearance,
occlusion, materials, shoe-sole orientation, pose quality and clothing fit still require visual
inspection in game.
The six render parts are separate from the native physics body and its coarse collision
shapes; rendered spokes, pedals, bars and pegs are not independently simulated colliders.

## Isolated setup

Use the prepared physical game copy at `build/bmx-game`, matching Skate.exe SHA-256
`FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9`.
Keep mutable files separate from the normal installation and production profile. The copy
uses the private generated model pack at `Mods/BMX_Prototype`; the launch script does not build
or validate its geometry content. Missing `reskate/bmx/*` assets appear in `bmx status` and
prevent riding. Run 40 deliberately used a control pack without those bike models.

Build the native runtime and fixtures from the repository root:

```powershell
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_BMX_TESTS=ON
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R '^bmx_' --no-tests=error --output-on-failure
```

Close the existing Skate process, then launch the prepared test profile:

```powershell
.\contrib\bmx-lab\Start-BmxGame.ps1 -Ride -ProfileName bmx-user-data-working-profile
```

`-Ride` uses the ordinary `load MPR` route, which was verified to close the startup Hub, and
queues `bmx mount`. It does not pedal automatically. Close menus, focus the game and center
sticks/release buttons. `bmx mount` can request the game's guarded grounded skateboard
step-off and waits for a genuine supported on-foot state. Stepping off normally also works.

The script copies the built launcher/DLL only into the scratch game, launches offline/windowed
with updates, Discord and loose files disabled, and supplies `--dev-data-root` beneath `build/`.
The validated root contains test profiles, native settings/saves, objects, logs, caches and
launcher preferences; invalid overrides fail without falling back to production data. The
launcher removes its development flag before forwarding game arguments. This isolates known
writers; it is not an operating-system filesystem sandbox.

## Controls and switching

| Input | Current native action |
| --- | --- |
| Left stick X | Handlebar steering; airborne yaw; physical right/left maps to rider right/left |
| Left stick Y | Front weight/posture; airborne pitch after centering the stick |
| LB/L1 or RB/R1 + right stick down in air | Left or right barspin |
| Right stick down then flick up on ground | Preload/release one charged bunny hop |
| Both triggers + right stick left/right in air | Left or right tailwhip |
| Partial right stick down/up | Manual/nose balance requests; sustained physical manuals remain unvalidated |
| A / Cross | Pedal while supported |
| B / Circle | Brake |
| R3 | One grounded quick hop; holding cannot repeat on landing |
| Y / Triangle tap | Get off into native walking with the bike parked; mount the nearby parked bike |
| Y / Triangle hold (0.5 s) | Pick up or set down the nearby bike |
| F7 / `bmx interact`; F8 / `bmx carry` | Tap / hold interaction equivalents |
| F6 / `bmx toggle` | Request BMX / native skateboard switching |
| D-pad | Preserve Skate's native actions; menus retain input priority |
| RT / LT | Trick-set modifiers; lean/lateral behavior remains unfinished |
| Start / Back / Escape | Release riding capture for menus |
| Keyboard WASD / IJKL | Front / rear sticks; K then I is the hop flick |
| Keyboard E / Q | Pedal / brake |
| Keyboard Z / C; 1 / 3 | LB / RB; LT / RT equivalents |

`bmx mount`, `bmx dismount`, `bmx interact`, `bmx carry`, `bmx status`, `bmx controls` and `bmx diagnose` remain available.
F6/`bmx toggle` switching is implemented and its guard fixtures pass. Runs 52/53 verified the full
six-part cycle using queued commands; Run 89 repeats it with the current native chassis and
one pooled allocation. Run 95 adds guarded parking/carrying/set-down and remounting. Physical F6 input remains untested. Switching to the native skateboard releases
pose/motion/input/protection, waits for the six model parts
and capture to clear, then requests the game's natural skateboard transition and waits for
actual grounded skateboard state. From normal skating/idle it uses the natural step-off path
before mounting BMX. Requests retain focus/menu/solo/fresh-owner checks and a 15-second deadline;
they can wait or reject rather than forcing a native state. A pending BMX mount keeps its
original owner and deadline while native wheel support becomes ready. Release F6 before pressing again,
including after focus/menu changes. Y deliberately returns to native walking with a parked bike;
nearby interaction owns only the North/Y input while other walking controls remain native.
Carrying uses a guarded right-arm target and moved visual entities, not a separately simulated
carried rigid body. Obstructed/unverified set-down can reject or put the bike away rather than
leave it floating. Native manual/flight protection continues to take precedence over a BMX fall request.

`Start-BmxGame.ps1 -NativeChassis -SwitchTest -ProfileName bmx-user-data-chassis-probe` queues mount,
pause/resume, two toggles, a parked walking dismount, carry, set-down, nearby remount and status
snapshots, leaving BMX mounted. The private `-CatalogTest -NativeChassis -UnattendedTest -TrickTest`
variant on that same probe profile selects only the Superman rehearsal; ordinary trick tests remain unchanged.
Combine `-UnattendedTest -SwitchTest` to run the riding rehearsal first. Keep the
window focused and inspect the actual native states; the queued script alone is not a pass.

Return controls to neutral to rearm. Focus/menu/device loss and simulation gaps cancel pending
input. Ordinary riding requires focus; Freecam, First Person, Noclip and Park Editor must
release conflicting ownership. Plain sticks do not orbit the camera. Air trick requests do
not establish a valid catch, landing or score. Return the right stick to neutral for barspin
and tailwhip catches; changing modifiers while held does not select a second trick.
See [controls-and-reference.md](controls-and-reference.md).

The explicit private `-ImpactTest` launch clears its saved manual NoBail toggle through
`nobail 0`, waits one second for the lease to expire, then starts the diagnostic. This affects
only the validated isolated probe profile. Ordinary Ride and other rehearsals retain their
existing protection settings; a queued request or passive fall capture does not prove a
working BMX fall animation.

## Rehearsal and evidence

A focused test uses `-SmokeTest -Map MPR` or the isolated `bmx selftest` command. An explicit
`-UnattendedTest` launch queues `bmx selftest unattended`, permitting synthetic input while
unfocused in the validated test profile. Both use native simulation time and preserve actual
native menu, solo, collision, actor and ownership guards; neither forces menu state. Synthetic
input expires after 60 wall seconds and the test dismounts on completion/failure.

Inspect `<test root>/bmx-selftest.json`. New reports require actual motion, braking and a hop,
negative native yaw during the positive left-stick steering phase, six live bike parts/placements,
and at least 15 completed rider render-pose writes per native simulation second. The final pose
must belong to the current rider and be at most 250 ms old, with computed contact targets.
Run 52 measured wrist/ankle joint proxies, which did not detect its open hands or poor posture.
The revised source uses a palm offset, mirrored hand orientation and finger curls around the
authored cylindrical grips; fixtures also compare contact with independent rendered grip
endpoints transformed by the handlebar matrix. Those checks still do not measure actual
skinned palms, knuckles, shoe soles, clothing or GPU pixels. Counters show execution, not visual
quality. `unattended=true` alone does not prove background operation;
`background_native_seconds > 0` is required for that claim.

| Evidence | What it establishes | Boundary |
| --- | --- | --- |
| Runs 33/35 saved reports | Native rider moved 11.14 m, hopped 0.56 m, turned 0.244 rad; speed fell from 4.91 m/s to zero | Earlier overlay presentation and bindings |
| Run 35 unattended request | Rehearsal completed without human input | Window stayed focused; background time was zero |
| Run 36 | Ordinary `-Ride` startup mounted and remained grounded | Does not validate the latest world pack or switching |
| Run 40 control | Verified native board state 100, return to on-foot state 504/BMX control, then walking; 962 render-pose writes | Bike blueprints absent: 0/6 parts, zero placements and velocity updates; no complete bike ride or appearance proof |
| Run 48 real bike pack | 11.1404 m travel, 0.5634 m hop, right yaw -0.2438 rad, braking 4.91 to 0 m/s; six ready parts and 3,252 placements | Synthetic commands; no physical pad, appearance or six-part switching acceptance |
| Run 48 rider handoff | 540 render-pose writes during 9.0167 native seconds; final pose 16 ms old with matching owner and 0.000031 m hand/foot errors | Solver/contact and callback evidence; not visual anatomy/clothing quality |
| Run 52 focused fixtures/build checks | 23 BMX CTest fixtures recorded passing, including pending-mount support recovery/owner/deadline guards | Historical count; passing fixtures do not approve visible appearance or terrain behavior |
| Runs 49/50 revised geometry | Native riding repeated with fork/saddle/pedal changes; Run 50 also included the aligned crank pivot | Appearance, fit and physical controller QA remain |
| Run 50 six-part switch | Six live parts/capture held became zero parts/capture released with native board state 100 | That run's return blocked in grounded ollie-crouch 103; its full-cycle report remains false |
| Run 51 switch repeat | Board cleanup repeated; two board snapshots 8.034 seconds apart had unchanged motion/placement/pose counters; natural step-off reached 504 | Timed BMX remount stopped on a transient wheel query. A later separate mount is not a completed cycle |
| Run 52 final riding | 11.1404 m travel, 0.5634 m hop, right yaw -0.2438 rad, braking 4.91 to 0 m/s; six parts, 541 velocity updates and 541 fresh render-pose writes | Synthetic commands, focused window; no physical controller or pixel-quality acceptance |
| Run 52 full switch cycle | Six parts → native board 100/zero parts → six parts/on-foot 504 → walking/zero parts → six parts; capture released between modes | One ordered isolated cycle; not repeated unload or continuous all-frame verification |
| Run 52 stock board scales | 93 coherent passive samples retained unit roots; 16 native board samples had all 16 scales unit after BMX release | Pose-buffer values at observed endpoints, not GPU pixels/materials |
| Run 53 corrected native integration | Riding and the complete scripted cycle passed again with the palm/finger/posture changes and B material experiment; 539 fresh rider render poses | User reports holding works but grips float and tire shading remains wrong; synthetic input does not test physical controls |
| Run 54 grip/normal correction | Native grounded mount at 504, six parts, 1,848 placements, 614 pose writes and 307 render writes | Mount/render smoke only; no new riding/switch sequence or accepted user pixels |
| Run 62 independent chassis | 8.65 m COM displacement, .996 m hop rise, 541 force applications and landed barspin/tailwhip combo | Historical bindings used rear-stick air pitch; it does not validate the current Streets selector |
| Run 82 pose-write performance | 9.0167 native seconds in 9.109 wall seconds, about 60 client/render FPS; rider callback .1964 ms average | Removes measured pose-write latency in this run; timing varied across earlier launches, and appearance is not established |
| Run 83 current physical controls | Landed one barspin, one tailwhip and one combo; one catch each, left-stick pitch response on serial 402 after command 401; 541 force applications and fresh rider render poses | Synthetic focused input, zero background native time; physical pad/D-pad and visual acceptance remain open |
| Run 87 measured ramp route | .483333 s continuous gravity flight, .066667 s settled two-wheel landing, actual root maximum 4.871706 m within the 5 m bound; 1.044715 m braking distance and .001615 m/s final speed | One isolated synthetic route; not broad ramp, collision or controller-feel acceptance |
| Run 88 current Streets controls | Caught and landed one barspin and one tailwhip as one combo, zero failed tricks; left-stick pitch produced .035322 rad/s next-step angular response | Synthetic selector/catch/landing evidence; physical pad/D-pad, opposite directions and visible rider quality remain unverified |
| Run 89 current chassis lifecycle/switch | Zero measured pause drift; BMX → board 100 → BMX → walking → BMX, three mounts/one allocation; board snapshots 8.05 s apart retained zero parts/released capture/578 force applications | Ordered sampled states with native activity resumed; not continuous ownership, physical F6 or visual acceptance |
| Run 82/83 focused fixtures/build checks | 31 BMX CTest suites passed for that runtime | Historical count; includes Windows pose-write protection/bounds fixtures, not broad collision, user-control or pixel-quality acceptance |

The earlier [Run 62 report](../../build/bmx/chassis-ride-run62/bmx-selftest.json) remains
historical evidence for the physical chassis with its previous rear-stick pitch binding:
8.65 m COM displacement, initial braking from 2.71 to .41 m/s, final speed .097 m/s, and a
477 Ns hop with .996 m COM rise. It recorded 541 force commands, rider tracking error below
.073 m, and airborne catches for a barspin/tailwhip combo. Runs 83 and 88 supply separate
current-binding evidence; these synthetic runs do not establish physical-controller acceptance.

Run 54's [validation record](../../build/bmx-native-run54-validation.json) and
[archive](../../build/bmx-native-run54.log) identify runtime
`b50864bd1a59b9654134b95a9453089e96e5987b0b89c763bcb3f9641577fd4e` and pack
`generated-29fef719c8594fd1a9f9c5c1d1293a07`. At 21:23:11.815 the rider was grounded in native
504 with six parts/capture held, palm-proxy error rounded to 0.000031 m and ankle-target error
zero. All 23 fixtures passed; 18 freshly generated part files matched the reviewed outputs
byte for byte. This run used `-Ride` mount/status only. Run 53's riding and switch reports below
remain separate evidence; Run 54's visible result is still pending user inspection.

Run 53's [ride report](../../build/bmx-native-run53-selftest.json) passed over 9.0167 native
seconds and 9.078 wall seconds, with 11.1404 m travel, a 0.5634 m hop, correct right turn and
4.91-to-zero braking. It recorded six parts, 3,252 placements, 541 velocity updates and 539
fresh render-pose writes; final pose age was 15 ms, palm-proxy error below 0.000031 m and ankle
error zero. Those are computed contacts, not measurements of skinned hands or soles. Background
time and overlay draws were zero.

Its [switch report](../../build/bmx-native-run53-switch-evidence.json) passed all 11 checks,
including the five ordered modes, renewed native activity after remounts and unchanged counters
across board snapshots 8.047 seconds apart. Final status at 20:04:42.556 was six parts, on-foot
504, capture held, 12,900 placements and 2,147 render writes. The
[corrective validation record](../../build/bmx-native-visual-fix-validation.json) links the
matching 23-test run and unchanged geometry/resource-scope audits. It identifies runtime
`01f53e54a988b551d0727a880f16536ebb50197e1aa027e8c78a4d1bb919fc10` and B pack
`generated-56d88d4c6b76428ab6ce4e70322b4f6b`. New visual acceptance remains pending.

Run 52's [ride report](../../build/bmx-native-run52-selftest.json) passed over 9.0167 native
seconds and 14.250 wall seconds. It recorded 3,252 placements, a matching rider owner, final
pose age 16 ms and hand/foot errors below 0.000031 m. Background time and overlay draw frames
were zero. Those old errors are wrist/ankle proxy distances, not evidence of a closed hand grip.
The [switch report](../../build/bmx-native-run52-switch-evidence.json) passed with
these ordered status endpoints:

| Local time | Mode | Native parts | State / capture | Placements / render writes |
| --- | --- | --- | --- | --- |
| 17:02:19.754 | Initial BMX | 6 | 504 / held | 5,046 / 840 |
| 17:02:27.844 | Native skateboard | 0 | 100 / released | 5,052 / 841 |
| 17:02:51.952 | Returned BMX | 6 | 504 / held | 9,156 / 1,525 |
| 17:02:54.011 | Walking | 0 | 504 / released | 9,162 / 1,526 |
| 17:02:59.108 | Final BMX | 6 | 504 / held | 9,960 / 1,659 |

The two board snapshots, 8.050 seconds apart, had identical motion/placement/pose counters;
both BMX remounts resumed native activity. Native board state was freshly verified by the
transition-completion guard and independent passive samples; idle status alone may cache its
last physics state. The [board-scale evidence](../../build/bmx-native-run52-board-scale-evidence.json)
records unit roots in all 93 coherent samples and all 16 unit scales in 12 native state-100
and four state-103 samples. This confirms the observed scale recovery, not visible board quality.

The [archive](../../build/bmx-native-run52.log) identifies runtime SHA-256
`cdc3dce0f9f9705fa803eece72e16c04fb809f2e5fe1afa8d5bf2dc397919ad3`.
The private pack remained `generated-d66cafd0969043b6a807afeda34154bd`.
The matching [CTest log](../../build/bmx-native-handoff-ctest.log) records all 23 tests passing.
The [combined validation record](../../build/bmx-native-final-validation.json) links the exact
artifacts. The [production-profile audit](../../build/bmx-production-data-after-run52-audit.json)
found zero path/size/mtime differences across 919 before/after files.

Run 50's [ride report](../../build/bmx-native-run50-selftest.json) passed with 541 velocity
updates, 541 render-pose writes and the same distance/hop/braking measurements as Run 48.
Its [switch evidence](../../build/bmx-native-run50-switch-evidence.json) correctly reports
`passed=false`: it observed the initial six-part bike and native skateboard, but no successful
return to six parts. State 103 is grounded ollie-crouch, not an airborne classification.

Run 51's earlier [ride report](../../build/bmx-native-run51-selftest.json) also passed, with 541 fresh
render-pose writes and a final pose age of zero milliseconds. Its
[switch report](../../build/bmx-native-run51-switch-evidence.json) remains `passed=false`:
the timed return failed before the script's walking/final-remount phases. The separate later
six-part mount does not establish that ordered cycle. Native board state was freshly verified
at transition completion; idle status samples establish counter inactivity, not continuous
fresh physics-state or owner observation. Both ride requests stayed focused, with zero recorded
background native time.

Run 48's [saved report](../../build/bmx-native-run48-selftest.json) and
[log](../../build/bmx-native-run48.log) identify runtime SHA-256
`cace2ca1267c2c663bd467cb15744e587c22b3dbd726becef0810719a3f745ac`.
It recorded 541 velocity updates and zero overlay draw frames; post-test status showed zero
bike parts, released capture and on-foot state 504. The request was unattended, but stayed
focused: background native time was zero. Its 9.0167 native seconds took 48.875 wall seconds.

The content comparison matters: Run 46 failed in native ray-tracing acceleration-structure
setup; the unchanged donor control in Run 47 passed. Run 48 completed after repairing the
mesh's visible-subset list to select section 2 alone, retaining section 5 for depth/shadow.
The [independent artifact audit](../../build/bmx-native-assets/category-fix-independent-audit.json)
found exactly two changed resource bytes per part and unchanged geometry. This supports that
specific repair; it does not establish visual quality, performance or broad unload stability.

Run 40's [captured evidence](../../build/bmx-native-run40-switching-evidence.json) retains exact
log lines and runtime hash. Board state 100 was confirmed at 15:59:43.697; BMX control returned
at 15:59:51.680. The final 16:00:01.776 status showed walking state 504, released capture,
1,924 pose writes and 962 render writes. These counts establish callback execution, not visible
pose quality, sustained movement or a selftest pass.

Old loading failures, profile investigation and detailed startup contracts are preserved in
[native-loading-history.md](native-loading-history.md). They describe earlier runs and are not
current startup blockers. Production data comparisons from the earlier isolated runs found no
path/size/mtime changes; repeat that audit when testing new writers or launch configurations.

## Remaining scope

The user's Run 52 visual feedback is a failed appearance check; Run 53 improved holding but
still has floating grips and incorrect tire shading. A repeat needs actual side/front
and rear views showing sleeves attached to bars, closed hands centered on grips, natural
neutral/preload posture, continuous tire appearance and plausible shoe/pedal contact; a repeated motion or pose-counter pass is
insufficient. Run 54 mounted/rendered the grip/normal correction; new visual acceptance remains pending.

The physical backend uses an independent native body, coarse attached backup capsules and
query-driven rolling-wheel contacts. Ramp-normal transport and bounded finite-edge refinement
replace the old one-sided support estimate; actual support is distinct from the speculative
ray skin and balance-only contact continuation. Run 88 verifies the current left-stick air
pitch and modifier/right-stick trick selection with airborne catches and delayed stable
landing credit; Run 87 verifies one measured ramp route with a bounded landing and stop.
Run 62/63 results used the earlier bindings. See [physics evidence](physics-backend-investigation.md) for the exact
portable and native boundaries. Thin obstacles, streaming loss, self/carried-board collision,
moving platforms and broad ramp feel still need real-world tests. Physical manuals,
pumping, peg grinds, bike/rider ragdoll separation, replay and multiplayer remain unfinished.
Mount/switch pause uses the same parked native body; per-body allocation destruction remains
unproven, so isolated process/world teardown is still the cleanup boundary.
Pedal platform normals rotate with the crank; shoe-sole
alignment is approximate. Broader repeated mount/switch/reset/unload stress coverage remains
beyond these isolated scripted cycles.

Geometry, input definitions and animation references extracted from the user's BMX Streets
installation stay local and ignored by Git. The private generated content pack is not a
redistribution package. Pose cues are fresh approximations using the native Skate rig;
reference clip offsets are not physics trajectories. See the [reference ledger](controls-and-reference.md).
