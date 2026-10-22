# BMX development

The `BMX` branch includes an in-game offline prototype and a separate synthetic controls lab.
Use [native-prototype.md](native-prototype.md) for the game setup, current evidence and limits,
and [controls-and-reference.md](controls-and-reference.md) for bindings and BMX Streets references.
The broader release scope remains in [the feature plan](../bmx-plan.md).

## In-game prototype

Current built and privately installed runtime `3FB0CCF935FAF14942BEE94996790CE7B271DAED02A8786C57E353BED31B7EED`
passes 62/62 Release tests in 4.45 seconds (attempt 21). Diagnostic ramp 244
preserves all recorded physics from 241 and captures two qualified fresh approach
witnesses under the unchanged ramp criteria. A separately default-off early normal
response passes private ramp 245 with less penetration and unexplained landing
impulse, but faster peak rotation and less route progress. It remains private. Rider
articulation has 64 successful native bone readbacks from 240; visible quality
remains below the requested acceptance target. Independent 244/245 visual review
finds hands floating above the grips during descent, outside the earlier pose
probe's captured window. Extended probe 247 covers that phase with all 128
CPU solves/writes/readbacks valid. Requested grip contact remains within 0.087 mm;
render consumption and the one-step historical frame need further investigation.

The independent ground-pump
adaptation adds bounded left-stick-down pumping and a landing pump window. Native
trick run 235 passes and exercises automatic landing pumping; virtual-controller
run 236 retains 18 direct-pump samples with strict support and submitted commands.
Both runs preserve 195 source pins and two teardown observations. See
[ground-pump-adaptation.md](ground-pump-adaptation.md) for the reference and force accounting.

The controller harness has 39 focused tests. Its revised timer completes the pump
sequence without concurrent screenshots, and a pump/brake/switch sequence completes
while NVIDIA records continuous footage. Other trials still exceed the unchanged
120 ms timing limit, including a later return-to-bike trial. Timing reliability
remains unresolved. Run 236 records the persistent floating skateboard stance.
The corrected private input bridge completes one guarded press/release sequence
in run 243 and visibly reaches the normal skateboard stance. Its complete
process-bound export is independently audited. This clean on-foot baseline does
not yet fix returning from BMX. Visual acceptance and 9.5 scores remain open.

Preceding privately installed runtime `F3D8D1CC4B09AC4A88736066F61397BA52A4BA02D33B9594189B7F54041BE951`
passes 59/59 Release tests in 4.33 seconds (attempt 11). Fresh private NativeOnly
sessions now bypass BMX lifecycle and interaction handling, including ordinary Y.
Session 229 exposed the prior isolation gap and was rejected before arming; the
earlier human report from 227 remains valid feedback but its input dispatch was
not observed. Run 230 passes the native trick rehearsal with unchanged source and
verified teardown. Its actual mid-tailwhip image is saved in
`artifacts/bmx-reddit/bmx-tailwhip-development-2026-10-20.jpg`; this is a sampled
development-build frame, not animation or physics acceptance.

The approved ViGEmBus driver is installed. A device-only XInput test verifies
neutral and disconnect, and virtual Y runs 232/233 show normal native skateboard
mounting in recorded pixels. Run 233 also records 406 native observer pairs and
161 client/pose rows, with BMX counters zero and sustained state 100 coverage.
Its normal Y path is 504→103→100 with a brief measured mount publication; the
software helper's 504→200→201→100 path lacks that measured publication. This
identifies a difference, not a proven corrective bit write. The Python harness
retains optional ready/go coordination. See
[virtual-gamepad.md](virtual-gamepad.md) for execution and evidence boundaries.

Preceding runtime `9B9F3E5702D07B9D25F380DF49214F8DC876B82F51609EFE4EF7AAC130541E5F`
passes 59/59 Release tests in 4.28 seconds (attempt 10). The private passive
NativeOnly comparison now supports explicit `-DeferNativeBoardArm`: startup does
not arm the trace, and the later console arm has a separate 30-second readiness
allowance for closing the console. Actual capture remains eight seconds. This is
a diagnostic timing correction; no physics or visible contact fix is claimed.

Preceding runtime `8DA149BB6FCEA0F40E2E939EC0D391BE5FC9E77D390730404E2D1DBA036C1145`
passes 59/59 Release tests in 4.45 seconds (attempt 9). Its new default-off private
observer measures stored bike-part entity matrices. Run 228 completes all four
synthetic switching routes with source/runtime identity and two teardown checks.
All 104 readbacks across 13 frames are coherent, finite, valid and exactly equal
to the requested matrix words. This short first-mount window has no native-clock
join and proves neither renderer consumption nor rider contact. The 33-frame
sampled recording includes a 35-second startup waiting gap; it is not full motion.

Preceding runtime `BFABDA578D292EB2DDBF66D835FC7C0621AFAD9127A0D15BBFDA3308ADAF0D4E`
passes 59/59 Release tests in 4.09 seconds. Its private observer records selector
and animation-publication boundaries without new native pose or physics writes.
Clean software-board run 225 passes strict CPU coverage, but all 151 measured
post-publication mount outputs remain zero and the visible rider floats above the
deck. This does not establish animation-graph consumption or a quality fix.

For comparison, the user reports a normal stance after ordinary controller Y in
session 227. Its initial eight-second trace expired during loading before that
action; no post-Y native values or complete post-Y recording are claimed. The
controller trace timing is being corrected before using it as an instrumented
baseline. See `build/bmx/judging/native-board225-independent-data/review.md`.

Preceding privately installed runtime `73EFF12E81A4BAB24020080AB39EE18F572976550805E9761262532E3921F773`
passes all 59 Release tests (4.21 seconds; attempt 7). Retained nearby-mount search
and strict dwell across all three board-publication streams are applied. The native
board animation experiment was rolled back after it failed to improve the visible
feet/deck gap; the retained search and strict dwell remain applied.

Native run 223 passes the short four-route functional regression with four mounts,
one allocation, one strict set-down and one guarded followup. Source/runtime identity
and two teardown absence observations verify. Its 32 sampled images have not yet
been judged; no extended board-pose diagnostic was requested. This is a narrow
functional pass, separate from visible quality or controller acceptance.

Run 224 also passes the extended four-route sequence on the unchanged runtime.
Its retained trace contains 136 rows; grounded board observations span 2.283 seconds
on the client and 2.300 seconds in each publication stream, with unchanged SDK
motion/pose/hide counters after release. Its independent pixel review still scores
bike/rider contact 4/10 and skateboard feet/deck contact 1/10. The recorded pass
does not resolve the intermittent fall in run 222 or establish smooth physics.

Before any BMX mount, native board control 219 passes the strict CPU baseline but
scores 1/10 for visible feet/deck coherence. Run 220 fails at native state 103 before
the retained phase. On the current runtime, run 222 visibly falls and shows Heavy
Slam before grounded board state 100; its blind visual demonstration scores 4/10.
Run 221's GPU startup failure is archived separately. The failed runs remain evidence;
the exact previously refused search point and sustained visible board stance still
require validation. Run 224's successful nearby route does not reproduce that refusal.
No 9.5 acceptance is claimed. See
`build/bmx/judging/transition222-blind-visual/review.md` and
`build/bmx/judging/native-board-animation-handoff/revision2/root-rollback.json`.

Earlier switching diagnostic 215:

Runtime `93AE2534216493B430F0151A5C35B5F5FA95C035AAA3C6F63472FBBB5ACC7030`
passes all 59 Release tests. Recorded diagnostic run 215 retains more than 2.25
native and wall seconds of grounded-board observations, with exact identity and
stable SDK pose/motion/hide counters after release. Native CPU bones keep evolving.
The independent footage review nevertheless confirms the upright rider above the
deck in four repeated samples. The later nearby-bike remount fails on wheel-support
readiness; this run is not a four-route pass. A native-board control before any BMX
mount and a retained-mount search correction are being prepared. See
`build/bmx/judging/board-handoff-214-independent-analysis/actual215/review.md` and
`build/bmx/judging/shortcut215-independent-visual/review.md`.

Preceding runtime `B5C49FF887403A2561828DE122C9CE206083975283DB116972963719474BE3DA`
passes all 59 Release tests and recorded native switching run 214. The BMX notice
now replaces its previous status instead of stacking cards. The independent 32-image
review confirms at most one card, but sees pose/text disagreement during transitions;
it cannot establish sustained rider contact or smooth motion. The requested longer
pose diagnostic did not activate, and its archive check also rejected a JSON-coerced
timestamp. Those diagnostic failures remain separate from the native scenario pass.

The preceding runtime `98F6DDAEE200252CA32364C0A2195192008233E80CE106445D8F5EE1D677CBC5`
passes all 58 Release tests and recorded native switching run 212. All four shortcut
routes complete with one retained allocation, four mounts and one guarded set-down
followup. Physical step-off now retains Skate's supplied body orientation. The SDK
records 69 exact basis-word matches with no mismatch; those counters describe returned
targets, not native consumption. Source/runtime identity and two teardown absence
observations verify. Repeat run 213 also passes all four routes on the unchanged
runtime, with the same 69 matches and no mismatch. Both test games are closed.

The preceding run 211 falls after board mounting states 200/201. Its failure is
preserved. Run 212 uses the same action schedule, all 54 initial riding samples and
the exact first shortcut pulse match; at native serial 89 it confirms grounded board
state 100. See `build/bmx/judging/step-off-native-comparison-211-212/comparison.md`.
This controlled comparison does not establish general fall recovery or visible pose quality.

The steering-only runtime `790500F76ED1F2F20537695DD0593DE4DE50EDC0766B205A39AEAB7F6C1A6E74`
passed matched native trick rehearsal 206. Coherent steering reduces abrupt
steer/release angular-rate steps. The same 541 raw input/time rows and initial state
match baseline 204; both land the caught barspin/tailwhip combo with no subsequent
strict-support loss. Release/brake RMS angular speed rises 9.3%, consistent with
slower centering. See
`build/bmx/judging/steering-native-comparison-204-206/comparison.md` for tradeoffs.
Ramp 207 also passes ascent, natural launch, landing and stopping. These precede
the latest switching instrumentation; production steering/contact laws are unchanged.

Use LB/L1 plus fresh Y/Triangle for the contextual bike/skateboard shortcut. Plain Y
tap/hold remains get on/off and carry/set-down. Stop on clear ground to switch out
of riding; approach a parked bike before mounting. F6 retains its existing route.
LS handlebars/steering, RS rear-wheel/hop/tricks and Skate D-pad mappings remain.
The shortcut remains experimental. Native shortcut automation uses a private virtual
input source through the real gesture handler. A source ordering gap can expose a
fresh physical LB+Y packet before its capture lease is published; actual native input
consumption is unmeasured. Hardware capture, controller feel and visual quality remain
separate acceptance checks.

The latest user video has an independent visible score of 5/10. Grip alignment, rigid
rider movement and fall motion still require work. The 9.5 target and near 1:1
BMX Streets goal remain unmet. The current refreshed Streets image differs from
the older reference. Read native-prototype.md for exact evidence and remaining gaps.

Earlier October20 damping iteration:

The earlier native build is `D8F79B702528E37EEE0A9652628B0F6E46AA859A08FB85CB2EE622E90202192F`
and passes all 58 Release tests, including eight physical steering-release regression
cases. Supported balance damping now reduces rocking after steering release. Stiffness,
force/torque caps, airborne input, collision and fall thresholds retain their values.
Left stick owns handlebars/steering, right stick owns rear-wheel/hop/tricks, and Skate's
D-pad mappings remain available.

Matched native trick runs 198/199 used identical initial states and all 541 input/time
samples, with only the damping header changed. Run 199 landed the caught barspin/tailwhip
combo; post-landing strict-support losses fell from ten samples to zero. Release/brake
RMS angular speed fell from .1788 to .1004 rad/s. Peak landing normal load increased,
so this is not a claim that every impact force decreased. See
`build/bmx/judging/upright-native-comparison-198-199/comparison.md`.

Trick run 199 and ramp run 200 passed on `1AFFC714...323C4B4`. The subsequent build adds
only the regression test and a genuine runtime relink; production physics source is
unchanged. Runs 201 and 202 passed the full equipment route and native fall/GetUp/standing/
retained-bike remount on the latest build. Every run preserved source hashes and verified
two process-absence observations. These automated rehearsals do not establish controller
feel, abrupt-reversal stability or general crash recovery.

The private smaller bike, coordinated continuous rider pose, coasting crank leveling
and matched collision envelope are installed. Native human bone lengths are retained.
The release bench reduces +8 m/s opposite lean from .1045 to .0254 rad and settling from
3.60 to 1.83 seconds; abrupt reversals can still briefly lose strict support and produce
sharper individual corrections. Strong damping does not remove the manual/preload target
step. Hard one-wheel landing kicks remain under investigation, including an additional
native contact reaction; no unsupported material/friction write has been applied.

Continuous run-196 footage is preserved at
`build/bmx/recorded-ramp-196/nvidia-original.mp4`. Its canonical 840-frame native-10-bit
active extract is byte-equal to the corresponding decoded source frames. Independent
visual scores range from 5 to 8.5, with hand/foot separation and a rigid rider stance.
That footage predates the new upright damping. Render publication/skin alignment,
physical-controller feel, exact game/display FPS and the requested 9.5 in every category
remain unverified or unmet. The sampled recorder does not establish continuous motion.

Earlier evidence:

Current iteration: the native rider now retains Skate's supplied physics rotation;
the articulated bike and rider presentation still use the bike rotation. A matched
comparison reduced the early horizontal skeleton/root separation from 17.39 m to
under 3 cm. Normal-pose run 183 confirmed this bound and passed native fall, typed
GetUp, standing, retained-bike recovery and completed remount. Its 116 nonstandable
trace samples all had bike interaction capture disabled. Transient released-body
observation suspension and recovery were also observed without replacing the bike.

Ramp run 182 passed ascent, flight, landing and stopping. Its independent visual
scores range from 8.0 to 9.0, with hands still separating during descent. The fall
and mount review of run 183 scores 6.8–8.0. These sampled images cannot establish
full-motion smoothness, controller feel or complete animation quality.

Equipment run 184 failed after carrying and setting down the bike: set-down accepted
ground whose two measured wheel-plane normals failed the stricter remount check.
Set-down now requires that same strict final support and clear approach, with a
bounded nearby search and carrying retained on unavailable proof. Run 187 passed the
full equipment route using one body and three mounts. Its accepted centre placement
did not exercise a rejected-centre nearby search; the captured 184 refusal has unit
coverage. Run 186 made no mount because the focus gate refused its requests.
The same-world
released-owner recreation handoff is built and source-reviewed but has not passed a
native recreation test. Controller session 30088's live far-fall route has not been
retested with the rotation correction. The 9.5-per-category target has not been met.

The runtime controls the verified native on-foot rider and renders eight articulated world
entities: frame, fork, handlebars, front/rear wheels, cranks and separate pedals. The bike uses native world
rendering; overlay bike drawing is disabled. Rider posing uses the existing native skeleton,
crank-synchronized feet, grip targets and bounded preload/weight/airborne posture cues.

Native equipment run 187 passed bike → skateboard → bike → dismount → carry →
grounded set-down → remount, using one body allocation. This internal route does
not establish physical-controller feel or complete transition animation quality.

For the prepared isolated game copy and profile:

```powershell
.\contrib\bmx-lab\Start-BmxGame.ps1 -Ride -ProfileName bmx-user-data-working-profile
```

The ordinary MPR load closes the startup Hub. Close menus, focus the game and return sticks
and buttons to neutral before riding. A/Cross pedals, B/Circle brakes, R3 quick-hops, and
Y/Triangle dismounts to walking. Left stick owns handlebars; right stick owns rear/hop/tricks.
F6 or `bmx toggle` requests a switch between BMX and the native skateboard; Y returns to walking.
Switch guards pass source fixtures and the internal round trip was verified in run 167.
Physical-controller transition quality remains to be verified.

## Standalone controls lab

The lab does not attach to Skate, load native bike entities or read/write game saves. Its flat
floor, diagram and trick rotations are synthetic. It shares the mapper/model and A/B/R3 pad
bindings, but has no native Y dismount or bike/skateboard lifecycle. It samples XInput only;
PlayStation pads need an XInput-compatible provider here. The in-game runtime also supports
ReSkate's native PlayStation reader.

| Lab control | Action |
| --- | --- |
| F2 | Switch XInput / keyboard emulation |
| G | Hold an air pose for inspecting gestures / return to flat floor |
| R | Reset the scene |
| WASD / IJKL | Emulate front / rear sticks |
| E / Q | Pedal / brake |
| A / B / R3 on pad | Pedal / brake / grounded quick hop |

Center both sticks and release buttons to rearm. Held input on takeoff does not become an air
trick. Menu/focus/device/time suppression clears pending gestures. Manual and trick requests
are diagnostics; the lab does not implement physical balance, catch or scoring.

## Build and fixtures

With CMake 3.24+, Ninja and a C++20 compiler on PATH:

```powershell
cmake -S contrib/bmx-lab -B build/bmx-lab -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/bmx-lab --parallel 4
ctest --test-dir build/bmx-lab --no-tests=error --output-on-failure
```

Run `build/bmx-lab/dingosdk_bmx_lab.exe` on Windows. It uses system XInput, User32 and GDI.
The harness registers the current BMX fixtures through `cmake/BmxTests.cmake`; list them with
`ctest --test-dir build/bmx-lab -N`. The native build enables them with
`-DDINGOSDK_BUILD_BMX_TESTS=ON`. See [msvc-setup-plan.md](msvc-setup-plan.md) for the approved
local toolchain setup. Building the lab alone does not build or validate ReSkate.dll.

Pure fixtures cover input ownership/rearm, native camera-relative steering, fixed physics time,
contact readiness, hop handoff, model/pose transforms and lifecycle guards. Native rendering,
clothing fit, surfaces, controller feel and switch transitions require the supported game.

## Serial native test suite

`Invoke-BmxTestSuite.ps1` runs the existing native rehearsals one at a time and
collects their reports and fresh ride/fall histories. Inspect the full plan first:

```powershell
.\contrib\bmx-lab\Invoke-BmxTestSuite.ps1 -PlanOnly -Scenario All
```

Planning only reads archive names. It does not query processes, start a game,
create output/profile folders, record the desktop or send input.

Run a scenario with the exact successful native build log for the current DLL:

```powershell
.\contrib\bmx-lab\Invoke-BmxTestSuite.ps1 -Scenario Flat,Ramp,Tricks,Equipment `
  -BuildLog build\your-current-successful-native-build.log
```

`-Repeat 3` repeats the selected sequence. `-Scenario All` also includes the
single Superman catalog sample and the separate side/inverted GetUp labs.
Each trial launches and closes the isolated `build\bmx-game\Skate.exe` with its
private probe or ramp profile. The working controller profile is not used.
Close any existing Skate session first. Run from a terminal that can query the
game's exact executable/PID/creation through CIM; a failed query stops the suite.

The runner currently depends on the prepared game, Release DLL/launcher and
existing supervisors in `build\bmx\run_native_chassis_ride.ps1`,
`run_native_ramp_probe.ps1` and `run_native_getup_test.ps1`. They are local
development prerequisites, not downloaded or recreated by this command.
`-PlanOnly` shows their full paths. A fresh checkout needs these prepared first.
The supplied build log is archived as provenance; its presence alone does not
prove compilation succeeded. The native supervisors also reject compiled source
newer than the DLL and archive matching source and artifact hashes.

Results go to a fresh `build\bmx\test-suites\suite-*` directory. `suite.json`
records every native and capture verdict, and `plan.json` shows the exact
scenario/profile/run IDs. Each scenario links its original native run archive
and stores a copy of `run.json`, supervisor output and PID-bound fresh traces.
The runner checks the scenario's expected terminal outcome, correctly typed
report flags, current artifact hashes, archived primary report PID, unchanged
source archive and two successful absence observations after teardown.
Supervisor shell exit code zero is insufficient to pass a trial.

Failures stop the sequence by default. `-ContinueOnFailure` preserves failed
trials and continues only after verified teardown and a fresh empty CIM query.
It cannot continue through unknown teardown, PID ambiguity or a running game.
No pre-existing archive is overwritten. The suite never stops another session.

These inputs run inside the BMX mapper on the native physics clock. They cover
the specified sequences, not physical controller delivery, the whole trick
catalog, arbitrary terrain, normal controller GetUp or a fall/pickup/remount
loop. Equipment uses console lifecycle requests with neutral synthetic BMX
input. Native thresholds and visual judge scores remain separate.

### Capture receipt interface

The suite command itself does not start a screen recorder. It can validate and
archive a receipt supplied by a separate supported recorder. A supported sampled
window recorder is described below. Without a receipt,
`capture.status` is `unavailable`; native tests can still pass.
`-RequireCapture` makes missing/rejected or temporally incomplete capture fail the suite's combined
verdict while preserving the separate `native_passed` result.

Pass `-CaptureReceiptDirectory build\bmx\capture-receipts`. For a planned
`Flat` trial numbered `148`, the recorder must finish and write
`Flat-148.capture.json` atomically after the video is finalized. By default
the receipt is checked as soon as the supervisor returns. Set
`-CaptureReceiptTimeoutSeconds 60` to leave a bounded window for a separate
recorder to stop/save after the native trial. The exact receipt name
is in `plan.json`. Run IDs shown by `-PlanOnly` are suggestions until execution;
existing archives are preserved and the next unused IDs are selected again.

```json
{
  "schema": 1,
  "scenario": "Flat",
  "run_number": 148,
  "process_id": 12345,
  "process_created_utc": "2026-10-19T12:00:01.0000000Z",
  "runtime_sha256": "64_HEX_DIGITS_FOR_THIS_RUNS_DLL",
  "game_exe": "C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-game\\Skate.exe",
  "capture_started_utc": "2026-10-19T12:00:02.0000000Z",
  "capture_ended_utc": "2026-10-19T12:01:00.0000000Z",
  "capture_method": "Name of the supported recorder",
  "video_path": "C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx\\capture-receipts\\Flat-148.mp4",
  "video_sha256": "64_HEX_DIGITS_FOR_THE_FINAL_VIDEO"
}
```

Identity must match the actual native `run.json`, including process creation
and runtime hash. All timestamps must explicitly use UTC. The nonempty video
must exist inside this repository, have a supported video extension and match
its hash. The interval must overlap that exact process lifetime, begin no more
than one second before process creation and end no more than five seconds after
supervisor teardown. Encoding may finish after teardown; encoded file mtime is
not used as a substitute for capture timestamps. The runner copies the validated receipt and video into its scenario
directory and checks the copied video hash.

`receipt_validated` means identity/time/file metadata checked. `-RequireCapture`
additionally requires a validated native gate release and capture spanning that
release through the terminal scenario evidence; ungated metadata-only receipts
cannot pass this requirement. The suite archives native gate status/control,
checks nonce, PID and exact native creation FILETIME, and requires a completed
trace-settle window. Temporal coverage does not
decode video, prove that the whole scenario was captured, exclude black or
loading-only footage, or establish visual quality. `visual_pixels_reviewed`
and `visual_acceptance` remain false until independent review.

`-WaitForRecording` additionally asks the prepared native supervisors to hold
their scripted actions until a separate recorder confirms it has started.
This requires the matching recording-gate helper/runtime support; the runner
checks that each selected supervisor exposes the gate parameters before launch.
Each plan step gives a fresh gate directory/nonce and the supervisor's live
`control.json` path. The runner creates the empty gate directory and forwards
`CaptureGateDirectory`, `CaptureGateNonce` and `CaptureGateTimeoutSeconds`.
Only the separate supported recorder should release that gate after its UI
confirms recording. The suite does not release it automatically or start a
recorder. The default gate timeout is 180 seconds, adjustable from 30 to 600.

Run the suite's offline policy fixtures without starting Skate:

```powershell
.\contrib\bmx-lab\Test-BmxTestSuite.ps1
```

They cover plans/IDs/profiles, stale or incorrectly typed native results,
teardown continuation, source/report acceptance and mismatched capture/trace
identity. Synthetic fixture files are created only under a dedicated fresh
`build\bmx\suite-fixtures` directory and removed after the checks.

### Supported sampled live window recorder

`Record-BmxWindow.mjs` uses the initialized `@oai/sky` client from `node_repl`.
It calls only `list_windows` and `get_window_state`; it sends no UI inputs,
releases no native gate and has no desktop fallback. It requires exactly one
returned window for `process:<repository>\build\bmx-game\Skate.exe` and checks
the returned app/window on every capture. PNG or JPEG bytes are saved unchanged
solely as source images for the requested video. Identity changes, ambiguous
transient screenshots and capture timeouts stop the capture and retain its
failure record. There is no independent sky-to-PID mapping: the exact native
PID/creation/runtime identity is separately supplied from supervisor evidence.

After initializing `sky` per the Computer Use skill, select the actual returned
game window and run these calls from `node_repl`. Supply the exact supervisor
owned identity as `ownedProcess`, and use a fresh output directory:

```javascript
globalThis.recorder = await import("file:///C:/Users/Jeffr/Desktop/reskate/contrib/bmx-lab/Record-BmxWindow.mjs");
globalThis.returned = await sky.list_windows();
globalThis.candidates = returned.filter(w => w.app === "process:C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-game\\Skate.exe");
if (candidates.length !== 1) throw new Error("Expected exactly one returned isolated game window");
// ownedProcess has process_id, process_created_utc, game_exe, runtime_sha256.
globalThis.recording = await recorder.beginBmxWindowRecording({
  sky, window: candidates[0], ownedProcess,
  workspaceRoot: "C:/Users/Jeffr/Desktop/reskate",
  outputDirectory: "C:/Users/Jeffr/Desktop/reskate/build/bmx/captures/fresh-run-name",
  durationMs: 180000, maxFrames: 180, minIntervalMs: 500
});
await recorder.stepBmxWindowRecording(recording, {frames: 1});
```

Inspect the automatically returned first game screenshot before a separate
authorized caller releases the recording gate. For manual inspection, bounded
chunks are available:

```javascript
await recorder.stepBmxWindowRecording(recording, {frames: 8, chunkDurationMs: 15000});
await recorder.endBmxWindowRecording(recording);
```

For the post-fall remount trial, capture the whole scripted sequence in **one
awaited cell** after acknowledgement. Model/tool reviews between chunks can
leave long holes in a recording. `Run-BmxPostFallRemount.ps1` (PowerShell 7+)
publishes an atomic `native-terminal.json` before its capture grace period and
owned teardown. Use sufficient recording limits, for example 300000 ms and
650 frames at a 250 ms minimum interval, then run:

```javascript
await recorder.recordBmxWindowUntilTerminal(recording, {
  terminalPath: "C:/Users/Jeffr/Desktop/reskate/build/bmx/native-postfall-remount-run152/native-terminal.json",
  scenario: "post_fall_remount"
});
```

The recorder checks the exact process creation, PID, runtime hash, scenario,
terminal time and phase. It requests a final live frame after observing the
receipt, then seals the capture. A failed native trial can have a valid recording;
the native result remains failed. Missing or mismatched terminal evidence and
capture limits fail closed. This terminal receipt is provisional native evidence:
final source, archived report and process teardown checks remain in `run.json`.

End after a completed chunk while the window still exists. If the game closes
mid-chunk the capture seals as failed; its frames remain available as failed
evidence. The manifest records every request/return UTC and monotonic interval,
source image hash, actual image dimensions, sample rate, maximum gap and capture
latency. Storage, frame count, duration, interval and per-capture timeout are
bounded. Source screenshot timing is known only within the sky request/return
interval, rather than at an exact native simulation frame.

`Finalize-BmxCapture.py` uses existing bundled Python with PyAV, Pillow and numpy
to encode the images into a video. Measured midpoint times become variable PTS;
the clip is not sped up to fit an invented frame rate. It decodes the final
video and checks frame count, dimensions and every presentation timestamp.
It does not record a screen or send inputs. Run it through the normal terminal:

```powershell
$env:PYTHONPATH=(Join-Path $PWD 'build/bmx-media-tools')+';'+(Join-Path $PWD 'build/bmx-unity-deps')
& 'C:\Users\Jeffr\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' `
  .\contrib\bmx-lab\Finalize-BmxCapture.py --workspace $PWD `
  --manifest build\bmx\captures\fresh-run-name\capture-manifest.json `
  --run-json build\bmx\chassis-ride-run149\run.json --scenario Flat --run-number 149 `
  --receipt-directory build\bmx\capture-receipts
```

The existing dependency directories are local prerequisites; this command does
not install packages. Without receipt options the script just encodes and
validates. Outputs are `sampled-live.mp4` and `sampled-live.validation.json` in
the capture directory. An optional receipt is written atomically only after
encoding and identity checks. Its capture bounds conservatively use the first
sample's return time and the last sample's request time, never the session's
later stop time. Existing output/video/receipt files are preserved.

This is a **sampled live recording**, not a 60 fps screen recording or a native
gameplay export. Gaps can hide brief tricks, collisions or pose defects.
Sampling rate and maximum gap travel with the video, and all visual acceptance
flags remain false. Full-motion capture and independent visual judging remain
separate validation work.

### Acknowledge and finish a recorded trial

After inspecting the first returned live frame, refresh one more frame and run
the data-only acknowledgement helper immediately through the normal terminal:

```powershell
.\contrib\bmx-lab\Confirm-BmxRecording.ps1 `
  -ControlPath <the-planned-supervisor-control.json> `
  -CaptureManifestPath <the-active-capture-manifest.json>
```

The latest frame must be at most 15 seconds old. The helper verifies its bytes,
window, live process, runtime and native gate identity before publishing the
acknowledgement. It sends no inputs and records no pixels. If inspection takes
longer, refresh a frame instead of relaxing this check. PowerShell commands stay
in the normal terminal; do not embed them in the Windows computer-use session.

Keep capturing while the native scenario runs. A recorder controller can read
the profile's terminal report before each single-frame capture, then end after
capturing a frame requested after that report exists and identifies the same
PID. This preserves evidence through completion and avoids capturing after the
supervisor closes the game. The supervisor allows a bounded trace-flush period.
Finalize the video and publish its receipt within the suite's explicit
`-CaptureReceiptTimeoutSeconds` window.

Live verification on 2026-10-19 used native run 150 and suite
`build/bmx/test-suites/suite-20261019T185715492Z-69f876c0`. The required-recording
flat trial passed: native pedal/steer/brake/hop, released gate, trace archive,
video coverage through terminal evidence, matching hashes and two teardown
absence observations. Its 93 captured images encoded and decoded successfully.
After gate release, 90 samples averaged 3.314 Hz with a maximum 1.296-second gap;
the complete clip also contains a 45.921-second idle gap before acknowledgement.
These rates describe recording, not game simulation speed. Runs 148 and 149
preserve failed-closed timeout evidence with no scripted riding started.

Recorded ramp run 157 also passed its native rehearsal, terminal coverage,
source hashes and teardown checks. Its 45 captured images encoded and decoded
successfully. After gate release, 43 samples averaged 1.291 Hz with a maximum
3.625-second gap; the earlier acknowledgement wait contains a 31.738-second
gap. Independent review found floating hands during the ramp passage and gave
partial scores of 7.0 for forward riding, 6.5 for ramps and 6.0 for rider
movement. These sampled trials are not visual acceptance or proof of physical
controller delivery. Equipment run 156 failed to complete its return mount on
uneven pavement; post-fall run 154 failed recovery after a rider displacement.

The next measured iterations preserve those failures. Private post-fall runs
159 and 165 completed native fall/GetUp/remount with the same bike allocation;
the intermittent far displacement is still unresolved. Equipment run 160
completed bike/board/dismount/carry/set-down/remount. Recorded ramp run 163
confirmed seven loaded recovery steps retained balance assistance without
changing the strict wheel-support predicate. Its judge still found a hand gap
after the lip and rated rider movement 6.0.

Recorded tricks run 162 caught a barspin and tailwhip but canceled their pending
landing during eleven loaded, negative-gap recovery steps. Run 164 retained
that already-caught combination only while fresh identity, contact, orientation
and neutral-input checks held. Recontact at serial 450 earned zero support time;
four subsequent strict intervals committed the pair at serial 454. All 35
compared physics/contact fields matched run 162 across 541 observations. This
changes pending trick settlement, not body motion. Run 164 passed with one landed
pair and zero failed/canceled combinations; its 51 genuine images have measured
timestamps, but the 1.139-second catch/landing gap prevents visual certification.

The placement coordinator built after run 164 admits a rider/board pose only
after all eight matching bike-part placement commands complete. It does not
prove final GPU synchronization. The build passed all 57 CTest checks, and
recorded ramp run 166 passed ascent/flight/landing with 541 completed rider render
writes. Equipment run 167 completed the full switch/carry/set-down/remount route
with three mounts and one bike allocation. Repeated ramp run 169 also passed,
but its sampled footage still misses the exact compression-phase hand-gap
moment. Blind review rated forward riding and ramp traversal 8.0 and rider
contact 7.5; impact timing remains unrated.

The passive first-mount producer trace build passed all 57 CTests. Private run
170 then aborted its first mount after 33 native updates because the chassis
snapshot velocity became invalid; it did not execute the planned native fall.
The failed trial is preserved with 31 producer records, 47 stored snapshots,
unchanged source hashes and two teardown absence observations. Its motion-target
derivatives remained small and matched the observed float32 calculation. A large
cached/physical rider discrepancy first appears between sampled pose callbacks;
custom-pose feedback and asynchronous native work remain under investigation.
The later successful console remount does not turn this trial into a pass.
An unchanged-build repetition, run 171, completed the full fall/GetUp/standing/
remount route. Both outcomes remain recorded while the callback data is compared;
a later pass does not establish that the intermittent fault is repaired.
Current machine-readable progress and judge reports are under
`build/bmx/judging/current-iteration-status.json`; no iteration has met the
requested 9.5-per-category visual acceptance target.
