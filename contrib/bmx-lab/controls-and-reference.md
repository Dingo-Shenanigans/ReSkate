# BMX controls and local reference evidence

Latest test: attempt 30 passes 66 Release checks. Ordinary Ride trials 260/261 use
the opt-in human adapter and an external virtual XInput controller in a separate
test profile. LB+Y completes the first bike-to-board switch and visibly reaches
a normal skateboard stance. The same-process return to BMX still fails. Attempt 30
waits for a settled dismount before acquisition; trial 261 falls during the native
dismount request before any new BMX mount begins, then times out. Clean NativeOnly
trial 262 uses ordinary Y twice: the sampled rider reaches board 100 then normal
standing 504, with all BMX activity zero. An input-based reverse adapter is now
being developed. Physical-controller acceptance remains unverified.
Attempt 29 also includes the bounded original-input timing repair prompted by
failed trial 258, but trial 260 takes immediate admission rather than the new
deferred branch. Earlier build-specific results below remain historical evidence.

The current native layout keeps the requested split: left stick owns handlebars/front end;
right stick owns rear/hop/tricks. A/Cross pedals, B/Circle brakes and R3 quick-hops. Tap
Y/Triangle to get off or mount the nearby bike; hold it for half a second to pick up or set
down the bike. F7/`bmx interact` and F8/`bmx carry` provide keyboard/console equivalents.
Hold LB/L1 and press Y/Triangle (or press them together) for quick equipment switching:
mounted BMX goes to the skateboard; the native skateboard or ordinary walking equips BMX
through the existing automatic native dismount/mount lifecycle. While beside a parked bike,
the same chord rides it; while carrying, it checks a clear set-down and mounts. A distant
parked bike stays where it was left: walk beside it before using this shortcut. Far recall
is not implemented. Release both buttons before another switch. Holding Y first and adding
LB does not turn an existing tap/carry into a switch. Menus, focus loss, device/owner changes
and invalid clock observations discard the shortcut until a new release and press.

This shortcut is experimental. Current built/installed runtime
`54FDA85C089AB2BBAEFF4846317F24CD6E02FB4253D0E0A66FC66FB790F0E803` passes 65 Release
tests in 5.02 seconds (attempt 25). Revised rider articulation passes native ramp 240 and its
captured grip/pedal readback checks; broader visual acceptance remains pending.
The input bridge is restricted to an explicitly opted-in private NativeOnly
experiment. Trial 243 completes one guarded press/release sequence and reaches
a normal skateboard stance. Its full process-bound export and independent audit
preserve the single input edge pair and native 504→103→100 sequence. Native
consumption remains explicitly unproved; this clean on-foot comparison has not
verified returning from BMX. Earlier trial 242 mounted visibly but timed out.
It does not fix ordinary switching yet. Left-stick down requests a bounded ground pump; the
automatic landing window and direct controller force path are measured in runs
235 and 236. Pumping requires measured wheel support and releases during hop,
manual and invalid input conditions. Right-stick and Skate D-pad mappings remain.
The rider still floats above the skateboard after the equipment shortcut.
Private normal-response ramp 245 reduces penetration and uncommanded landing
response, but increases peak rotation. It remains disabled for ordinary rides.
The additional private compliant-contact run 248 passes its route criteria but
worsens penetration, peak angular speed and settling; it is rejected as a
handling improvement and remains default off. Landing-only trial 252 reduces
peak rotation about 10%, but increases penetration from 6.471 to 13.873 mm;
that mixed result also remains private. Its CPU request HUD has no qualified
camera data, so it cannot yet resolve the hand-registration defect.
The separately guarded post-BMX original-input trial 251 visibly returns to a
normal skateboard stance in inspected samples. Its official verification remains
failed because three state flags are absent from compact exports and the log
capture uses the wrong path. Attempt 24 fixes those exports, confirmed in trial
253, but that trial fails the strict exact-client-row join: the terminal tick
falls between sampled client ticks. Attempt 25 retains the exact completion tick
and passes fresh trial 254's unchanged strict native and provenance checks.
Ordinary switching and physical LB+Y acceptance remain unresolved; a separate
explicitly selected human-input prototype is being developed. Fresh ramp 255
passes the same route gates with unchanged physics. All 21 camera diagnostic
receipts report rejection by `valid_camera`; no camera markers are qualified.
The rejected matrix/FOV subcheck and visible hand/bar gap remain unresolved.
Independent visual comparison also finds floating hands during descent in both
244 and 245; the earlier pose probe did not cover that moment. Extended probe
247 covers descent with all 128 solves/writes/readbacks valid and CPU palm/grip
error below 0.087 mm. Actual render timing remains unresolved; no offset fix is claimed.
Fresh private NativeOnly tests bypass BMX lifecycle/input
handling, correcting a prior isolation gap. That correction changes diagnostic
sessions, not ordinary controller mappings. Run 230 passes the native trick
rehearsal and supplies a real tailwhip screenshot; quality remains unaccepted.
Run 228 on the preceding attempt 9 passes four synthetic switching routes. Its 104
stored bike-part matrices match their requests, without establishing renderer
consumption or rider contact. Preceding private measurements show that the software board
request reaches board physics without a surviving measured animation mount
output. The user reports that ordinary Y gives a normal stance, but that manual
session's trace expired before the action. The newer virtual-controller comparison
233 captures the normal Y route and a brief native mount publication, with BMX
activity zero and normal skateboard stance visible. Its state sequence differs
from the software switch helper; a corrective implementation remains unverified.
No animation, physics-feel or visible quality fix is claimed.

The preceding built/installed runtime
`73EFF12E81A4BAB24020080AB39EE18F572976550805E9761262532E3921F773` passes 59/59 Release
tests in 4.21 seconds. Retained nearby search and strict three-stream board dwell are
applied; animation experiment 6 was rolled back after native board 219's strict CPU
pass still showed feet/deck separation (1/10). Run 223 passes all four short native
functional routes with four mounts and one allocation; source/runtime identity and
two teardown absence observations verify. Its 32 sampled frames are not yet judged,
and it does not provide extended board-pose evidence. Retained-search branch coverage,
physical first-packet capture, controller feel and 9.5 visible quality remain unaccepted.

Extended run 224 also completes all four routes on the unchanged runtime and retains
strict 2.25-second board100 coverage in all three CPU observation streams. Independent
pixel review still scores bike/rider contact 4/10 and feet/deck contact 1/10. The fall
in run 222 is intermittent and unresolved; neither a native pass nor CPU coverage
accepts the visible stance or controller feel.

Preserved failures: run 220 stops at native state 103 before retained remount; run 222
falls before board100 and visibly shows Heavy Slam; run 221 fails during GPU startup.
Earlier runs 212, 213 and 214 pass all four game-side routes after the step-off
orientation correction. Run 211's mounting fall and run 215's persistent rider/deck
gap with later nearby-bike remount failure remain preserved.

The bike must be slow and upright on clear ground before dismounting. F6/`bmx toggle`
requests BMX/native skateboard switching; its source guards
are tested and Run 95 verified the scripted BMX/skateboard/park/carry/set-down/remount cycle. Physical controller
acceptance and broader lifecycle coverage remain separate. RT/LT no longer provide propulsion/
braking. Keyboard fallback retains E/Q. The [native guide](native-prototype.md) lists the complete
layout and transition requirements.

The isolated physical chassis (`Start-BmxGame.ps1 -NativeChassis -Ride`) adds measured
full-body rotation and force-driven wheel response. Right-stick partial down/up balances
a grounded manual/nose manual. Following the October 18 request for Streets controls,
left-stick Y now requests airborne pitch/flip, matching its authored `Flip` binding.
Center the left stick after takeoff before applying down/up. Right-stick hop/trick
motion cannot become pitch torque. Left-stick diagonals combine yaw and pitch after rearming.
Airborne LB/L1 plus right-stick down requests a left barspin, and RB/R1 plus down requests
a right barspin. Both triggers plus right-stick left/right request the corresponding tailwhip.
Center the right stick to rearm while optionally holding the modifier; return it to neutral for the timed
catch. These named combinations match the recovered BMX authored trick sets. Timing, catch,
deadzone and physical response are ReSkate's adaptation, not a direct
port of the Streets binary. Run 95 verified native switching, the parked/carry lifecycle and
same-body pause/resume. Run 88 passed a caught and landed barspin/tailwhip combo and the
next-step physical left-stick pitch response with the current mapping. Its exercised
right-stick-only phases produced no air-pitch command. `-Ride` selects this physical backend
automatically; the explicit `-NativeChassis` form remains valid.

## What was recovered

Read-only extraction from the installed BMX Streets `global-metadata.dat` found its generated
InputSystem asset `MashBox_Freerun_InputActions`, literal 47090. Its BMX map binds LS X steering,
RS Y preload/flick-hop/manual intent, South acceleration, East braking, R3 quick hop and North
dismount. The same multi-vehicle map has separate throttle/car-brake actions; their presence
was not treated as proof that BMX pedals on RT. The earlier left-stick barspin/right-stick
tailwhip split has been replaced by the recovered named modifier-plus-right-stick requests.
The October 18 request preserves Skate's native D-pad behavior; Streets' separate
drone/emote/marker/respawn D-pad bindings are not adopted. Native consumer analysis now
confirms LS Y reaches the flip system and modifier-plus-right-stick selection uses named
trick sets. See [the native reference](streets-rider-landing-reference.md). Trigger lean,
shoulder slider still require implementation and validation. The expanded catalog now implements
all 58 nonempty authored BMX modifier/direction selections. Its [binding table and source
contract](../../build/bmx-streets-controls/trick-reference/catalog-implementation-contract.md)
distinguish recovered bindings from adapted motion. Six empty selections remain empty.
Named rider motions use bounded Skate-rig poses, limb release/catch, and articulated bike
targets. Actual chassis quaternion movement earns flip/roll credit separately from these
visual targets. This does not establish complete Streets physics or animation parity.

The checkpoint build passes all 35 BMX CTests, including the expanded catalog, rider contacts,
impact policy, body-guard queries and controller/D-pad filtering cases. Runs 95/96 add the
native observations below; expanded collision-envelope/bail acceptance remains pending. Carrying follows the
walking rider with a right-arm pose and bounded obstacle rays; it is not a separately simulated
carried rigid body. A short, expiring interaction capture removes only Y/Triangle while nearby
interaction is owned; ordinary keyboard/mouse walking and other pad controls remain native.
Full mounted capture and menus retain priority. Native walking legs and Skate's D-pad mappings remain untouched.
The native fall-request adapter still rejects an animated rig until its activation contract is
established; a queued native request alone does not count as a working fall animation.
Run 88 uses synthetic game-side input; physical controller transport and the actual native
D-pad actions still require a hands-on acceptance pass.

## Expanded selections and native checkpoint

These are the implemented airborne modifier/right-stick selections. Center the right stick
before a fresh selection; holding a modifier is allowed after that neutral rearm. Release the
right stick for recovery/catch. Overlapping chords use the table's top-to-bottom priority.
The six empty authored selections stay empty.

| Modifier | Up | Up-right | Right | Down-right | Down | Down-left | Left | Up-left |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| LB + RT | Grand Slam Left | Twix Right | Helicopter Right | Bri Flip Left | Willy Flip | Mc Griddle Left | Kick Whip Left | Superman |
| RB + LT | Grand Slam Right | Christ Air | Kick Whip Right | Mc Griddle Right | Bar Hug | Bri Flip Right | Helicopter Left | Nothing |
| LB + RB | Tuck No | Table | Invert | Turndown | Suicide | Euro | Lookback | Cannon Ball |
| LB | — | Tire Grab Left | Seat Grab Left | Bus Driver Left | Barspin Left | Toboggan Left | One Hander Left | Xup Left |
| RB | — | Xup Right | One Hander Right | Toboggan Right | Barspin Right | Bus Driver Right | Seat Grab Right | Tire Grab Right |
| LT + RT | Tuck Up | No Foot Can Right | Whip Right | Whopper Right | Crankflip | Whopper Left | Whip Left | No Foot Can Left |
| LT | — | Can Left | Moto Left | Nac Left | — | Griz Air Left | One Footer Left | Candy Bar Left |
| RT | — | Candy Bar Right | One Footer Right | Griz Air Right | — | Nac Right | Moto Right | Can Right |

All 58 selections have source-tested request, pose/cycle, release/recovery and landed-credit
paths. The linked rider/bike references guide bounded Skate-rig adaptations; they are not
direct Unity humanoid replay. The four Grand Slam/Bri Flip bindings that reference Scooter-named
clips are retained, with explicitly adapted compound bike movement. Native acceptance does
not yet cover every slot.

Run 95 passed all 11 sampled lifecycle phases: mounted bike → native skateboard → bike →
parked bike → carried bike → verified ground set-down → bike. Six native parts stay visible
while parked/carried, and chassis force/follow counters remain unchanged off the bike.
Three mounts reuse one allocation/world while fresh mount-lease tokens advance 1 → 2 → 3;
the parked/carry/set-down phases retain token 2. The carried interval recorded 244 successful
pose writes and 122 render writes over 2.032 seconds, with a maximum sampled hand-target
error of 0.000031 m. These are successful typed writes and target checks, not pixel or physical
walking-input acceptance. See the [switch/lifecycle review](../../build/bmx/chassis-ride-run95/independent-switch-lifecycle-review.json)
and [carry review](../../build/bmx/chassis-ride-run95/carry-pose-review.json).

Run 96 exercised Superman (slot 7) with LB + RT + right-stick up-left after the ordinary hop.
The gesture ran at 6.517–6.750 native seconds; neutral recovery completed an airborne catch
at 6.900 seconds. Actual wheel support began at 7.283 seconds, and clean credit followed at
7.350 seconds after 0.066667 seconds of support: one Superman, one combo, zero failures.
The runtime measured eight distinct successful owner-matched native render observations at
presentation phase ≥ 0.9, and 541 successful render writes over the complete rehearsal.
See the [independent temporal review](../../build/bmx/chassis-ride-run96/independent-catalog-review.json)
and [native report](../../build/bmx/chassis-ride-run96/bmx-selftest.json). This verifies one generic
catalog path beyond the earlier barspin/whip test; the other selections, actual controller
gestures and visible animation quality still need acceptance.

Both runs used runtime `B0936B896B11C926764FBF3D2F83D76BE4967299C2F2039C0B9B761085434DC4`,
unchanged archived sources and verified process teardown. The newly converted replacement
model has passed conversion checks but is not deployed in these runs. Tire appearance remains
postponed. Inverted-bike clipping and a complete native BMX fall/recovery sequence are still
unresolved; the private impact diagnostic releases only its saved manual NoBail toggle using
`nobail 0`, waits one second, then starts the test. Ordinary Ride/other profiles are unchanged,
and the runtime continues to respect independent manual/flight protection.

Run 97 subsequently passed the existing barspin/whip combo and next-step physical pitch
rehearsal on runtime `F3971A887608330394D20080385F2897C88232788739F1DB1013279610A25100`.
The [same-build inverted/side diagnostics](../../build/bmx/native-impact-runs98-99-independent-review.json)
(Runs 98/99) detected fresh head/frame guard contacts at 4.57189/5.22502 m/s closing speed
and released chassis control. Both native-fall tests failed: the rider stayed animated,
with zero native calls, no consumed request and no observed ragdoll. Contact detection and
released bike control are not a complete clipping/fall/recovery fix. The archived interim
handoff wording is misleading; a later text-only build says “bike control released.”
Physical controller actions, visible falls and broad inverted/side collision behavior remain
unverified. The new model conversion remains undeployed.

Local raw evidence and reproducible extraction:

- [InputSystem asset](../../build/bmx-streets-controls/input-actions-literal-47090.json) and
  [metadata inventory](../../build/bmx-streets-controls/metadata-input-inventory.json).
- [Movement source/output hashes](../../build/bmx-streets-controls/target-motion-manifest.json),
  [clip summary](../../build/bmx-streets-controls/target-motion-summary.json) and
  [decoder](../../build/bmx-streets-controls/decode_target_motion.py).
- [Human avatar paths](../../build/bmx-streets-controls/avatar-3526.json),
  [bike joint paths](../../build/bmx-streets-controls/avatar-3527.json) and
  [controller parameters](../../build/bmx-streets-controls/bicycle-human-controller-reference.json).

These ignored files are available in this workspace; a clean clone does not contain them.
The extraction reads installed files without executing or modifying BMX Streets.

## Movement cues and limits

| Paired rider/bike clip | Path IDs | Authored timing |
| --- | --- | --- |
| Pedaling | 2931 / 2941 | 0.833333 s, 48 Hz, loop |
| Preload_Jhop | 2910 / 2962 | 2 s, 24 Hz, loop |
| Jhop_01 | 3089 / 3047 | 0.333333 s, 60 Hz, no loop |

Each `motion-<id>.json` contains binding/channel offsets, named Transform paths and representative
samples; `clip-<id>.json` retains the raw decoded clip. Streamed coefficients were checked against
adjacent stored keys, and binding widths match the complete streamed/dense/constant channel totals.
Quaternion samples must be normalized before composing a pose. Nonfinite metadata sentinels are
explicit strings in the targeted JSON exports.

The authored controller scales its Pedaling state with `EngineRPS` and exposes Preload, Grounded,
YVel, Hop, Manny/Nosey and angular parameters. This supports synchronizing the ReSkate rider to
crank phase and actual input/airborne state. Current posing adds bounded crouch, weight shift and
air extension while maintaining grip/pedal contacts; it does not replay these Unity clips directly.

The preload clip's two-second loop is not a measured input-charge requirement. Local animation
translations are not world motion or hop height. Raw humanoid scalar attributes are not Skate
joint quaternions. The October 17 physics investigation reconstructed the stripped custom type
trees with a workspace-local optional decoder and recovered named bike settings, wheel friction
curves, suspension, balance and jump configuration. See
[the physics backend investigation](physics-backend-investigation.md), and the local
[typed configuration report](../../build/bmx-streets-controls/physics-typed-configs.json).
These are serialized defaults; active settings selection, native consumers and runtime overrides
still require verification. No full animation retarget, native code or complete physics behavior
has been ported.
