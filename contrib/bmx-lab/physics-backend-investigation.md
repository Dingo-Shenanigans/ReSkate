# BMX physics backend investigation

Investigation and implementation record, 2026-10-17. Native calibration, independent
chassis riding, switching, pause/resume and the caught/landed air-trick rehearsal now pass.
Run 62 is the current physical build; the isolated -Ride launcher selects it automatically.
Controller feel, broader terrain/collision coverage and visual acceptance remain open.

The user's current priority is **physics first**. Tire work is postponed. The portable
trick release/catch pose checkpoint is frozen; no trick-only deployment should stand in
for fixing the physics foundation. The separate BMX Streets investigation supplies
reference component/parameter evidence, not a callable Frostbite physics implementation.

## Legacy Run 54 motion and its limits

| Evidence | Actual behavior / limit |
| --- | --- |
| `Extension/BMX/bmx_native_motion.h:21` | Submission accepts linear velocity, target position and **yaw**, with no chassis orientation or torque input. |
| `Extension/BMX/bmx_native_motion.cpp:75`, `:102`, `:185` | Validates the current offboard rider, applies bounded positional velocity correction, and publishes an expiring 200 ms lease. This is a rider-following servo. |
| `Extension/BMX/bmx_native_motion.cpp:205` | Writes linear velocity and pending flag `8` to the existing offboard skeleton's bodies; synchronizes native offboard-flight velocity. It allocates no bike body. |
| `Extension/BMX/bmx_native_motion.cpp:233` | The motion target is rebuilt at line 249 as a yaw-only basis with world-up Y. Native pitch/roll are not transferred from a bike chassis. |
| `Extension/BMX/bmx_runtime.cpp:193` | Two downward wheel rays provide support heights. Their difference produces visual pitch, rather than rotational contact dynamics. |
| `Extension/BMX/bmx_runtime.cpp:220` | Six obstacle rays form a small ray fan. The source explicitly says this is not a convex sweep or dynamic bike collision. |
| `Extension/BMX/bmx_runtime.cpp:242` | Visual position follows the physical rider; pitch comes from wheel heights and roll is a speed/steering heuristic. Neither is measured chassis orientation. |

Native movement, completed pose writes and six model placements demonstrate those paths
executed. They cannot establish tire forces, chassis balance, angular momentum, contact
response, rider/bike physical coupling, or accepted appearance.

## Reusable native contracts

`Engine/Game/Build/20260929/remote_collision.h:5` and
`Extension/Multiplayer/Remote/remote_collision.cpp:25` define 16-byte body/shape handles
`{world, uint32 index, uint32 generation}`. The resolver at `remote_collision.cpp:56`
checks 32-byte supported-build fingerprints before using these APIs:

| Operation | Native RVA | Source contract |
| --- | --- | --- |
| Resolve client physics world | `0x3d2e920` | `remote_collision.h:11` |
| Allocate independent body | `0x266a6c0` | `remote_collision.h:17` |
| Set motion type: fixed 1 / keyframed 2 / dynamic 3 | `0x25daa90` | `remote_collision.h:21` |
| Create capsule in body-local coordinates | `0x266ace0` | `remote_collision.h:33` |
| Shape category / collides-with / owner | `0x25d9ea0` / `0x25d9410` / `0x25db410` | `remote_collision.h:38` |
| Local center of mass | `0x25d9180` | `remote_collision.h:50` |
| Set quaternion XYZW + position XYZW | `0x25dc000` | `remote_collision.h:54` |
| Linear / angular velocity; wake | `0x25da600` / `0x25d8530` / `0x25d3360` | `remote_collision.h:58` |
| Body / shape handle validation | `0x25d7b10` / `0x25d7e00` | `remote_collision.h:69` |

The existing creation path (`remote_collision.cpp:138`) uses **type 2**, a capsule,
zero local COM, no shape owner and initially disabled collision masks. Placement is at
`:163`; ownership/generation checks at `:122`; world replacement handling at `:238`.
This is a useful independent-body precedent, not a validated dynamic-body initializer.
The `+0x980 = .01` setter is an inverse-mass scalar: the native linear impulse consumer
multiplies its input by it. `+0xaa0` bit clearing retains only its observed low-level
meaning (`remote_collision.h:25`); neither field is established as friction or damping.

Terrain queries can be reused now. `Extension/Objects/ParkEditor/park_editor_surface.h:17`
defines the typed native ray/result ABI; `:49` checks its four fingerprints; `:110`–`:152`
checks result ownership, bounded hit count and finite fractions, returning point/entity/body.
`Extension/Profile/local_profile_objects.cpp:132` resolves the fresh **client TLS context**
and world, and excludes the current local rider through verified body/entity lookup.
`Extension/Profile/local_profile_runtime.h:92` exposes hit/clear/unavailable separately.
It currently exposes no hit normal, material or contact impulse. A clear ray does not prove
regional collision streaming. The sphere query `0x26c0fd0` shares a core implementation
with the ray (`park_editor.h:58`), but has no verified typed wrapper here yet.

The six bike entities are client static-model props, not bike collision bodies
(`Engine/Game/Build/20260929/world_props.h:4`; `native_prop.h:55`). The park editor can
move an existing object's physics transform (`park_editor_runtime.cpp:229`), but that
does not prove force integration or dynamic vehicle initialization.

## Original audit requirements before the dynamic chassis experiment

- Prove dynamic mass, inertia tensor, gravity/damping/material initialization and valid
  capsule/body defaults from a real dynamic creator. Do not merely change type 2 to 3.
- Prove force/torque or impulse units, persistence and call timing. Existing wrappers do
  not establish these. Two dynamic-only native functions, `0x25da0b0` / `0x25d7fe0`, were
  observed to store float4 values in indexed state arrays `+0x970` / `+0x978` and dirty/wake
  a body; their downstream semantics remain unresolved. They are not proven impulses.
- Prove coherent body pose, linear/angular velocity and sleep/contact readback, with
  handle-generation checks before and after the snapshot.
- Prove the correct terrain collision category pair, self/rider exclusions and contact
  normals. The remote proxy masks target characters (`remote_collision.h:77`), not a bike.
- Prove the thread/phase for dynamic writes. The additive skater listener supplies native
  simulated time after that skater's body contacts and before the original skeleton response
  (`no_bail.h:56`, `no_bail.cpp:366`). That alone does not establish a safe independent-world
  pre-step force boundary or all-world post-step snapshot boundary.
- Prove shape/body removal order, native destruction and partial-creation failure cleanup.
  Current remote proxies only disable masks and retain allocations until world teardown;
  `clear_remote_collision()` is not a destroy API (`remote_collision.cpp:183`, `:282`).
- Wheel shapes, friction/contact response and steering/axle constraints are subsequent
  contracts. Existing source does not provide a native wheel/joint backend.

Useful bounded static starting points are the existing retail proxy creator `0x1043390`,
skater physics creation `0xf8d730`, and body synchronization `0xfc06b0`
(`remote_collision.h:11`, `:25`; `skater_state.h:31`). These are investigation targets,
not additional callable contracts established by this report.

## Original orientation-transfer audit

The visual interface has position plus Euler yaw/pitch/roll, **no chassis quaternion**
(`Extension/BMX/bmx_visual.h:6`). The six-part generator rotates local articulation first,
then roll Z, pitch X and yaw Y, and produces native basis/translation matrices
(`bmx_world_transform.h:31`, `:49`). Native prop placement already accepts full 4x4 matrices
(`native_prop.h:46`; `bmx_world.cpp:52`).

The rider uses the matching quaternion composition `qY(yaw) * qX(pitch) * qZ(roll)` for
world-space hips, palms and ankles (`bmx_rider_pose.h:77`, `:256`). It converts targets back
through the live parent hierarchy, then writes only intended pelvis XYZ/quaternion lanes
after fresh owner/state/layout/200 ms checks (`bmx_rider.cpp:41`). Thus one consistent
position/Euler snapshot already rotates **visible** parts and rider targets coherently.
Existing fixtures exercise combined yaw/pitch/roll (`Test/bmx_world_transform_tests.cpp:60`,
`Test/bmx_rider_pose_tests.cpp:291`), not an independently simulated chassis.

Missing transfer is a canonical measured chassis quaternion and COM-to-bike-root transform.
An adapter could decompose a quaternion into the existing Euler order, but arbitrary flips
need singularity/continuity handling; a shared normalized quaternion or orthonormal basis
consumed by both paths avoids that conversion. The visual root is ground/contact height,
not a defined body COM. Both paths must use the same rotated COM-to-root offset and native
step snapshot. The current publisher instead anchors to the offboard rider and heuristics.
Visible IK does not constrain the upright native rider's physical bodies to the chassis;
that coupling and camera behavior require separate verified contracts. No transform API
change or dynamic coupling is implemented by this audit.

## Next isolated chassis drop test

After chassis initialization, actuation, readback, timing, terrain filtering and lifetime
are established, test one owned dynamic capsule/chassis in the isolated development world,
without rider attachment, pedals, tricks or six-part bike rendering. Wheel/joint discovery
is not a prerequisite for that first chassis test. Seed pose once; do not continuously servo
its position or orientation.

Required archived evidence is: exact game/DLL fingerprints and API guards; world/context
and handle generation; shape/category/mass/inertia/COM configuration; native step serial/dt;
coherent position/quaternion/linear/angular velocity over the drop; queried ground evidence;
and explicit contact/sleep/cleanup outcomes. Measure descent and gravity response, finite
unit orientation, terrain penetration/settling, angular response from an off-center contact
or a separately proven torque/impulse, and response scaling under a second known mass.
Declare tolerances before the test. Update counters alone cannot pass it.

Stop actuation on expiry, world/owner/generation change, invalid readback or bounded-motion
violation. Disable owned collision/velocities only through verified APIs on the owning
thread, then destroy shapes/body through the proven native lifetime path. Demonstrate
successful cleanup and stale-handle rejection, including cancellation and partial failure;
never touch handles after world replacement. Per-body destruction remains unproven and
blocks a reusable riding backend. The first diagnostic is deliberately narrower: one
allocation attempt in a dedicated development process, followed by termination of that
exact isolated process after the report or timeout. Process teardown releases its resources;
this does not validate per-attempt body destruction or reusable mounting/unmounting.

After this passes, measured chassis pose can feed the common visual/rider transform path;
wheel/contact forces, balance and rider coupling remain separate acceptance gates.

## Native body and accumulated-force evidence, October 17

Two bounded analysis carriers preserve selected original code and referenced data at their
original addresses. Their modified headers, synthetic entry exports, source ranges and
omissions are recorded in `build/bmx/fbphysics-native-slice-manifest.json` and
`build/bmx/fbphysics-native-slice-v2-manifest.json`. They are static analysis artifacts and
must never be executed. The original supported `Skate.exe` SHA-256 is
`fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9`.
REA dossiers are retained in `build/bmx/rea-fbphysics-native-evidence.json` and
`build/bmx/rea-fbphysics-native-v2-evidence.json`.

- Body reset `0x2641070` initializes inverse mass at `+980` to 1, inertia-to-mass ratios
  at `+988` to `(1,1,1,0)`, zero local COM and identity local rotation. These stores are
  established in the decoded prefix; a truncated tail prevents claiming full initializer coverage.
- Linear producer `0x25d14b0` adds `inverseMass * input` at `+970`.
  Angular producer `0x25d19a0` adds `worldInverseInertia * input` at `+978`.
  Off-center producer `0x25d1550` also adds the angular response to `(point-COM) cross input`.
  Matrix helper `0x25d72c0` rotates the diagonal `+980 * +988` into world coordinates.
  Thus the principal inertia is `1 / (inverseMass * ratio)` for positive finite values.
- `0x25da0b0` and `0x25d7fe0` overwrite the pending vectors; they are different operations
  from additive producers. Pending vectors are not immediate observed velocities.
  **Run 56 corrects the initial impulse interpretation:** at the tested 60 Hz, consumption
  multiplies the pending vectors by the native step duration. These inputs are force/torque
  like for one step, rather than instantaneous impulses. Variable-step behavior is untested.
- Pose and velocity arrays use a 32-bit element count and a 16-bit current ring-plane
  index. Each plane stride is the native padded count expression, not the plane index.
  Direct COM pose interpretation is restricted to the fresh body with independently
  checked zero COM, identity local frame, current generation and allocation bit.
  The `+f98` association seen in shape transforms is not established as a body-indexed
  parent array and must not be read using a body index.
- The retail synchronizer `0xfc06b0` calls the three additive producers. Its callers,
  world mutation phase and integration timing are not yet established. The first drop test
  therefore withholds impulses and records that limitation explicitly. A skater's contact
  callback alone does not prove a safe mutation boundary for an independent world body.

The bounded release-candidate scan at RVA `0x2669000..0x2676000` found no verified
body/shape destroy function. This is a negative result for that range only. The dedicated
`-ChassisProbe -ProfileName bmx-user-data-chassis-probe` launch allocates at most once,
seeds once, observes native gravity/collision, and never mounts or moves the rider.

First-run evaluation is declared before launch: verify the supported image and defaults,
then the seeded COM within .01 m of the requested position. A useful drop observation must
contain finite unit quaternions (squared norm within .01 of 1), descending position and
negative vertical velocity before first contact. On a locally flat patch, the upright
capsule's resting COM should be about .35 m above the queried floor: allow .15 m for the
first diagnostic and require final speed below .3 m/s over the last .5 native seconds.
At least 100 double-read-validated samples over five native seconds are required to assess
settling. These are diagnostic thresholds, not bike handling acceptance. No surface normal,
sleep state or contact manifold is available, so slope, roll or rejected samples must be
reported rather than forced into a pass. Stop on movement beyond 10 m from the seed or
4 m below the queried floor. The native impulse and mass-scaling tests remain pending.

## Run 55: first native drop

The isolated test completed 6.016666 native seconds with 364 archived stage samples,
362 seed/observed samples and zero rejected snapshots. The exact diagnostic DLL SHA-256
was `ad8ffd15a63e6229d3963fdb83d28fc8a9d000d2750747f96e5aaf7ab7384927`.
The report, log, runtime and process record are in `build/bmx/chassis-drop-run55/`.
The body was seeded once at floor + 1.5 m, inverse mass .02 and inertia ratio (1,1,1).
No subsequent movement lease, pose drive or impulse was applied during observation.

- Early vertical velocity fits `vy = -9.8000016 * nativeSeconds` through .4 seconds.
  This is observed game gravity in its native length/time units, independent of Streets defaults.
- The first contact-compatible change occurs between .466667 and .483333 seconds,
  at COM about floor + .350067 m. Vertical velocity changes from -4.57333 to -2.68466;
  lateral and angular motion begin. The manifold and surface normal are not observed.
- Maximum quaternion squared-norm error is 2.55e-7. The body tips and drifts .879486 m
  horizontally. Its final speed is .08159 m/s and angular speed .23448 rad/s: it is still
  moving, so this is not a rest/sleep proof. The original floor ray cannot establish
  penetration after that drift. Interpret position and velocity XYZ only; native W lanes
  do not retain the usual position/vector sentinel values during simulation.
- The owned capsule was disabled and keyframed under fresh guards, then the exact isolated
  process PID 13952 was terminated. A delayed process check verified teardown. This releases
  process resources and does not establish a per-body destructor.

The independent drop establishes useful native simulation and readback. It does not establish
wheel grip, steering, balance, rider coupling or improved riding. The installed test copy was
restored to the backed-up Run 54 runtime after archiving the diagnostic DLL.

## Run 56: native producer pulse and unit correction

One exact local wrapper matched the supported retail synchronizer hook. The owned body,
allocation/dynamic bits, generations, client TLS and 72 immutable owner-chain watches passed;
the once-only call occurred at .166667 native seconds under a 100 ms lease. The diagnostic
supplied linear `(5,0,0)` and angular `(0,5,0)` inputs at mass/inertia 50. Both pending vectors
changed from zero to .099999994 in their corresponding axes. Subsequent observed linear X
velocity and angular Y velocity changed to **.00166666647**, not .1, and persisted until contact.
The effective momentum change is `5 * (1/60)`, consistent with a one-step force/torque input.
The archived old JSON field names containing `impulse` are historical labels, not corrected
unit claims. Current source uses the corrected interpretation.

This is why the native consumption test was required: producer arithmetic alone did not
establish integration units. The exact wrapper observation supports this producer call site;
it does not establish a global physics-worker barrier. The complete six-second report, DLL,
exact source snapshots and process record are in `build/bmx/chassis-drop-run56/` (DLL SHA-256
`2fb75b2e47329d83b3f25b3ec68c11a84c046c3f454587b280216d877672217f`).
The exact owned process PID 9044 was terminated and teardown verified, then Run 54 restored.

## Recovered Streets reference, October 17

The local [reference report](../../build/bmx-streets-controls/physics-reference.json)
retains input hashes, full curves, configured flags, object IDs and coverage boundaries.
It accepts 28 selected bike components, one linked motor/handling/jump settings asset and
four linked SmartData records. Every accepted object has an independently checked common
header and reproduces its original serialized bytes. Six other SmartData records fail full
byte coverage and remain unknown. These are authoring values, not observed runtime behavior.

- Project physics: 100 Hz fixed timestep, gravity Y = -12.5, 20 position and 20 velocity
  solver iterations, contact offset .0025.
- `MotorVehicleSettings_ProtoBMX` object 46345: wheel mass 8, radius .26, maximum steer
  angle 18, friction constant 2, maximum slip angle .2 radians, maximum lateral force 3000.
- The same asset names jump velocity 5.2, jump velocity vector [3.2, 3.2, 4.5], lean
  limits 16 plus extra lean 14, and air spin/flip torque .28 with maximum angular 12.
  Units, mixing, settings overrides and actual consumers still need verification.
- Raw prefab body defaults differ from the linked settings. Do not blindly substitute
  prefab wheel mass/radius or assign these fields to prototype constants by name alone.

The verified portable Cpp2IL tool is local to `build/bmx-streets-controls/Cpp2IL.exe`, SHA-256
`663fb432433b4371fd1ee0ebc321a8fff2a9aac5ac4230c843f9e03ddee4e04c`.
Its metadata analysis identifies Unity 2022.3.62f2 / IL2CPP metadata 31.1, code registration
VA `0x184b76e70` and metadata registration VA `0x1854538b0` in the exact image hash recorded
by the report. Generated stubs and the local
[method index](../../build/bmx-streets-controls/physics-method-index.json) are derived metadata
coordinates, not recovered C# method bodies. REA inspection of a generated DLL authenticates
that derived artifact only; it does not establish original CIL or native method behavior.

The independent `bmx_tire_dynamics.h` work is a portable force-arithmetic checkpoint.
At that checkpoint the deployed DLL still used the old adapter; Run 62 below supersedes it.
Passing standalone fixtures alone does not establish
native collision, complete bike simulation, or improved riding feel.

## Focused native consumer evidence

The whole-image Ghidra query timed out after 300 seconds. A separate **non-runnable,
analyst-derived analysis carrier** retained exact selected code pages at original preferred
addresses plus source data sections. Its modified headers and omitted code are recorded in
[the slice manifest](../../build/bmx-streets-controls/physics-native-slice-manifest.json).
This is never a game replacement. The original image was not modified or executed.

Three bounded functions were inspected through REA/Ghidra 12.1.4. Complete local evidence is
[retained here](../../build/bmx-streets-controls/rea-physics-native-evidence.json).

| Original mapped method | REA record | Established behavior and limits |
| --- | --- | --- |
| `MGSteerableWheel.GetMaxSteerAngle`, RVA `0x8328a0` | `ev_a46d6231d13ffd9f3119823620fa080bc5c35993e9d8232095d6b6c73283e494` | Clamps its speed argument to at least .5, uses a speed-dependent expression blended over `(speed-5)/20`, and clamps the result to 8..70. A referenced math helper is omitted, so the complete curve formula is not yet verified. This is distinct from the authored 18-degree setting. |
| `MGSteerableWheel.ApplyVelocityAlignedSteeringLimit`, RVA `0x8326f0` | `ev_546271f84480a99de6305ae506d4668575d25584f2d315f643fe244a36c80b6c` | An enabled settings branch samples a curve and blends a wanted angle toward a window around velocity-aligned steering, with a separate overall angle bound and assist strength. Effective settings and caller argument mapping remain to be checked. |
| `WheelPhysics.ApplyRealisticSideForce`, RVA `0x2972480` | `ev_592ab8c67dc8662c03ff2cd3b98bbfcdb6db99842343d4c049eace154a6e1d01` | Samples the named sideways-friction curve, combines low-speed assistance and friction multipliers, rotates a lateral direction by wheel-body rotation, then calls `Rigidbody.AddForceAtPosition` on the connected body. The call's numeric mode is 2, mapped by the same build's enum to `VelocityChange`. This proves that branch's actuation form; it does not prove the branch is selected for the active bicycle or that fields named force use Newtons. |

Method identity comes from the matched IL2CPP metadata/registration, and call names/field
offsets from the same build's generated metadata. Pseudocode is decompiler inference over
the retained original bytes. Missing callees and runtime active-branch selection remain
unknown. A separate `UpdateWheelLoad` query had no analyzed procedure at its address;
its behavior was not inferred from nearby code or the method name.

The four recovered wheel components author `_useRealisticFriction = 0`. Consequently the
realistic-side-force function above is **not established as the active BMX grip path**.
Its existence must not be mistaken for selection; the ordinary friction consumer and any
runtime setter still need tracing before adapting the actual BMX grip rule.

The second, separately manifested carrier now retains `WheelPhysics.FixedUpdate`, ordinary
friction, braking and wheel-load consumers. See the exact source ranges and 11 metadata-derived
entry aliases in `build/bmx-streets-controls/wheel-ordinary-native-slice-manifest.json`, the
address/name map in `wheel-ordinary-call-rva-map.json`, and four REA dossiers in
`rea-wheel-ordinary-native-evidence.json` under the same directory.

- `FixedUpdate` RVA `0x2973760` selects the ordinary call `ApplyFrictionForce` when
  `_motorVehicleSettings.WheelSettings.UseRealisticModel` is false; the true branch calls
  `ApplyRealisticSideForce`. This is the actual observed selector, distinct from the wheel
  component's `_useRealisticFriction` field. The recovered settings asset also authors
  `UseRealisticModel = 0`; runtime replacement/overrides remain unobserved.
- `ApplyFrictionForce` RVA `0x2971680` clamps the wheel-load factor to 0..1.25 and blends it
  toward 1 at low speed. It combines several grip/pressure/state multipliers, projects its
  result onto the contact tangent plane, and invokes `Rigidbody.AddForceAtPosition` with
  numeric mode 2 (`VelocityChange` in the matched build). A separate resistance call uses
  mode 5 (`Acceleration`). These are velocity/acceleration adjustments, not direct evidence
  that the named force fields are Newtons or portable unchanged to another engine.
- `UpdateWheelLoad` RVA `0x2977030` uses positive contact-normal dot collision impulse,
  divided by fixed delta time, and increments a sample count. `FixedUpdate` averages the
  accumulated samples and divides by the authored load scale (1300 in the checked wheel
  components), then clears the accumulator. No live load or effective pressure was captured.
- `AddBrakeForce` RVA `0x2970a90` requires its grounded/contact state and calls
  `Rigidbody.AddForce` with mode 5 in opposition to the wheel velocity direction.
  Two referenced scalar/vector helpers (`0x66e280`, `0x66e370`) lack an exact recovered
  metadata identity; the complete magnitude law is not asserted from their names.

This improves the behavioral reference for an adaptation. It is not a direct Unity-to-
Frostbite code port, and does not substitute for the independent native chassis tests.

## Run 57 calibration and independent chassis implementation

Run 57 repeated the six-second native capsule experiment at mass 25 kg, principal
inertia {12.5, 8.3333, 6.25}, and the authored OnlyTerrainCollision masks In=0,
With=0x101. A 5-unit X force and 5-unit Y torque produced next-step velocity changes
0.003333333 m/s and 0.009999999 rad/s at the observed 60 Hz native step. This matches
F*h/m and torque*h/I, independently checking the mass/inertia setters and the corrected
force units. There were 363 samples and no rejected snapshots. Terrain contact still
occurred with the narrower mask. Rider exclusion was not proven because the test body
was placed three metres away. Exact source, DLL, report and verified process teardown
are retained in build/bmx/chassis-drop-run57/. Run 54 was restored after the experiment.

The new source path consists of bmx_native_chassis, bmx_chassis_ride and the portable
bmx_chassis_control controller. It is explicitly selected by Start-BmxGame.ps1
-NativeChassis together with an isolated ride/rehearsal mode. The legacy adapter remains
the default. The new path creates one 106 kg body with two near-spherical capsule tires
and a frame capsule. Proposed principal moments are {26,32,10}. It reads native COM,
full quaternion and velocities; terrain rays supply local contact planes. Forces include
rear-wheel drive, braking, tire grip, balance/manual torques and a separately identified
one-shot hop. Native gravity is not added a second time. The rider and six model parts
follow measured chassis orientation; the bike receives no continuous pose/velocity target.

Allocation is bounded to one pool attempt per world/state, with a bounded history of 16
world identities. Unmount disables collisions, clears pending forces, zeros velocities
and parks the body as type 2. The same pool is reused on later mounts. This is a deliberate
bounded development alternative to a verified destructor, which is still unavailable.
A changed/uncertain world is forgotten without touching stale handles or allocating
again for that same world. Test processes are terminated and verified after collection.

Current model limits remain material: one rigid chassis, approximate capsule wheels,
zero-camber contact arithmetic, balance assistance, static queried-surface velocity, and
only the calibrated 60 Hz force path. Native wheel joints, material friction tuning,
moving-platform response and complete gyroscopic evolution are unvalidated. Portable
and mocked bridge tests establish arithmetic/guard behavior only. The 26 BMX CTest
suites pass; actual physical riding measurements are the next acceptance boundary.

## Run 58: first independent physical riding pass

The isolated native rehearsal completed all nine simulation seconds and passed the
physical chassis and rider/render gates. Exact inputs, source snapshots, executable
hashes and the report are retained in build/bmx/chassis-ride-run58/.

- Actual chassis COM horizontal displacement: 8.9942 m from the runtime's initial read.
- Actual chassis horizontal speed before/after braking: 2.7121 / .4073 m/s; final .0404 m/s.
- Correct signed right turn: -.1095 radians in the native rider and chassis traces.
- One 477 Ns hop command; next-step measured vertical velocity change, after accounting
  for gravity, is 4.5000006 m/s, consistent with the configured 106 kg body.
- COM hop rise: .9964 m; measured takeoff vertical speed: 4.3375 m/s.
- 541 force applications / 541 samples, exactly one per successive native serial; zero
  rejected producer samples. Maximum rider-target distance: .0723 m.
- During clear flight, native horizontal velocity stayed constant and vertical velocity
  followed -9.8 m/s^2. Full quaternion rotation, six native parts and rider pose writes
  continued throughout. This is physical/runtime evidence, not visual user acceptance.

The original supervisor recorded a post-termination identity-check error. It is retained
unchanged in run.json. A separate teardown-followup.json records two successful empty
PID queries 300 ms apart, verifying that owned PID19808 exited. The supervisor now tolerates
transient incomplete process metadata after issuing a verified owned-process stop, while
still requiring two successful absence observations and treating query errors as unknown.
Native equipment switching and actual parked-body pause/resume are the next regression.

## Runs 60–62: airborne tricks and bounded landing response

Run 60 demonstrated both full trick rotations and airborne catches, plus the correct
next-step rear-stick pitch response, but stopped at the first slightly nose-down single-
wheel landing because the contact solver requested more than the existing 4000 Nm cap.
The failed native report and partial analysis remain intact. The fix retains that cap:
a single common fraction scales contact normal/tangent impulses, body response and rotor
response. Motor/brake/balance/hop contributions are kept separate. This preserves friction
cones and momentum accounting while native capsules absorb the remaining impact. Strict
GCC/MSVC fixtures and an independent 20,000-case contact/momentum/energy oracle pass.

Run 61 is inconclusive: its original 100-second supervisor window expired after startup
and before a terminal native report. It is not counted as a native pass. The supervisor
now permits 140 seconds of observation plus 10 seconds for verified teardown, while the
runtime synthetic-input lease remains 60 wall seconds. No test time or native step was
fabricated. Background validation jobs were stopped for the next native run; the earlier
slowdown's exact cause was not established.

Run 62 completed all nine native seconds in 9.781 wall seconds and passed the physical,
render and trick gates. DLL SHA256 is
8bb970540f2b5fc492fac81f3274debda276289689896f36ac9859cf43c3b451.
Exact source/DLL/log/report/verified two-observation teardown are in
build/bmx/chassis-ride-run62/. Independent replay passed 22 physical and 25 trick checks.

- One native hop; COM rise .9964 m. Actual COM travel 8.6457 m.
- Correct steering sign; braking 2.7121 to .4073 m/s; final .0968 m/s.
- Barspin request serial392, full 2pi rotation and airborne catch420; tailwhip request392,
  full 2pi rotation and airborne catch426. Actual queried support at437 commits exactly
  one barspin and one tailwhip as one landed combo, with no early or duplicate credit.
- Rear-stick pitch serial401 produces +.0351567 rad/s on serial402 about the latched
  measured body-X axis, with +55.1049 Nm requested torque and one native application.
- Contact fraction reaches .589081 at the one-wheel impact, then the ride continues.
- 541 native force applications, .0723 m maximum rider tracking error. Average controller/
  native-query bridge time is 1.436 ms, peak 2.988 ms; average six-part placement is .036 ms.

Pedal torque now remains unchanged below 6 m/s and tapers to zero at 8 m/s based on the
larger of relative forward speed and rear rotor speed times radius. It does not clamp
body velocity or alter coasting, gravity or braking. This is a proposed governor, not
recovered Streets drivetrain behavior. Low-speed, cutoff, platform invariance and a
36,000-step free-wheel cadence fixture pass under GCC and MSVC. High-speed terrain feel
is not established by the native nine-second low-speed rehearsal.

The user-facing isolated -Ride launch now chooses the physical backend automatically.
A physical controller session and visual/ramp acceptance remain separate from synthetic
input, successful native bone writes and the measured flat-ground ride. The standalone
BMX trick tally does not modify Skate's score.

## Ramp transitions and landing quality (2026-10-18)

The contact bridge now searches coherent small patches beneath and ahead/behind
each wheel, then verifies the inferred disk contact with a fresh ray along the
fitted normal. It selects the nearest actual verified surface, including a ramp
face reached by the tire before the hub crosses the toe. Missing or ambiguous
geometry leaves the wheel unsupported; unavailable native queries still stop the
backend. Verification prevents extrapolating contact beyond a real lip. Search is
bounded at 18 native rays per wheel (three in clear air). No bike pose, height or
velocity is snapped to a ramp.

The 30 mm speculative contact skin is now separate from actual support within
5 mm. A positive gap permits its measured closing speed rather than holding a
stationary bike above the surface. Input, balance and trick judgment use the same
actual-support predicate. Balance damping follows bounded measured normal
rotation on continuous curved support instead of resisting all ramp pitch rate.
Any support-mask, direction, ownership or pause transition clears that history;
airborne motion receives no automatic upright correction.

Native trick judgment freezes rotations and catches on first actual contact,
requires at least 60 ms of subsequent suitable support, and resets that timer
across brief bounces. A gap over 40 ms cancels the attempt. Body/support alignment
and closing point speed reject inverted, ceiling, unstable and excessively hard
scoring outcomes. These are proposed BMX scoring values, not recovered Streets
effective settings; they do not change collision response or native Skate score.

Run 63 passed the native physical and barspin/tailwhip rehearsal with DLL SHA256
1565db96746b66ed39cd1f324f03be52cbb0c29fcb356fdc18d5b5d1afe0273c.
First landing support was serial 437, with no credit; serials 438–440 accumulated
support and serial 441 committed one combo after .066666663 native seconds. The
independent review recomputed support and quality from the recorded points,
normals, gaps and measured velocities. Both catches occurred in the air at 420
and 426. The maximum requested torque stayed within 4000 Nm, with one contact
fraction of .736948. This was nine native seconds in 59.718 wall seconds, close
to the existing 60-second lease; a successful controller-feel check is separate.

Run 64 created one actual native BuildKit kicker in the separate ramp profile.
All 169 pre/post grid samples and a native object pick establish its collision
and ownership. Its measured slope is 0.4 (21.8 degrees), rising toward negative
fixture-local Z, with approximately one metre of height. This proves the queried
ramp geometry, not the capsule collision mask or a completed ramp traversal.

Run 65 correctly stopped during fresh route preparation before teleporting or
driving: the approach corridor contained elevated obstacle bodies at local Z +7
and +8.5. Its failed result is retained. The dedicated ramp driver uses measured
position/support to sequence pedaling, natural flight, settling and braking;
it sends no hop, air torque, steering or pose trajectory. A later traversal must
independently establish ascent, lip departure, sustained flight and a stable stop.
Portable tests and the independent 20-case ramp oracle pass; the oracle includes
60 sloped impact cases and remains distinct from native terrain evidence.

Runs 66–68 preserved separate setup failures: unused far-approach terrain exceeded
the initial flatness threshold, ordinary teleport recreated the canonical skater,
and its initial dismount request preceded grounded readiness. The final route
checks only the used corridor, accepts a fresh canonical owner solely during the
bounded teleport arrival, and refreshes the existing guarded dismount lease until
actual on-foot arrival. Fixture/world/editor identity, stable arrival and time
limits remain required; no forced native state is written.

Run 69 completed the first real traversal attempt. At the toe the old center-only
estimate read about four degrees while the actual lower ramp face measured 21.8
degrees. Its actual contact projection was .12662 m ahead of the hub, on the ramp,
while the downward hub ray still hit the floor. The native collision impulse then
bounced the bike before ascent. Run 70's adjacent-face search fixes that contact
and verifies ascent, but still stalls: COM rise .38586 m. Both failed results are
retained and are not native ramp passes.

Run 71 changes only the rigid native wheel capsules from radius .335 to .305 m,
retaining their .01 m vertical axis. Their outer extent is now .31 m, about 30 mm
behind the logical .34 m tire; they remain collision backups within the existing
100 mm logical penetration abort. These shapes share the chassis body and do not
have independent axle spin. Their native material/friction coefficient is still
unknown. Same-route comparison measures flat-ground external resistance dropping
from about 61 N to effectively zero, toe speed rising from 3.035 to 4.007 m/s, and
maximum COM rise increasing from .38586 to .80377 m. The bike still stalls before
rear-wheel lip departure. This supports contact interference, not a claim about a
native friction coefficient or general curb/side-impact robustness.

Run 72 used 55 Nm but expired at the previous 60-wall-second lease after only
5.616 native seconds. Its partial rise of .85062 m is not a completed traversal
result. The diagnostic lease is now 120 wall seconds, while the required riding
duration remains nine actual native seconds. The ordinary controller has no
diagnostic shutdown timer. The current proposed pedal torque is 60 Nm; gravity,
the 6–8 m/s cadence governor, brakes and force/torque caps are unchanged.

Run 73 did not reach the riding phase: teleport arrival exceeded its separate
12-second preparation bound. The supervisor then closed the owned game process;
this archive does not establish a native game crash. F6 was used before the
scheduled preparation, creating a chassis pool for a rider subsequently replaced
by native teleport. Later mount attempts correctly refused that stale pool.
The private ramp process now reserves mounting for its verified route handoff;
ordinary controller riding keeps F6. Arrival reports additionally retain observed
position, target errors, stable age/reset reasons and canonical candidate changes
to distinguish future setup failures without weakening readiness checks.

A fresh ordinary Ride session on 2026-10-18 used Run 73's DLL
2e7d6dda949eb4c47945048c88c122fc726deedf8bf057db4325f1b8a6d2fc54.
Its startup status confirmed six native bike parts, an active independently
simulated chassis, 198 force applications and rider pose updates. This is startup
evidence for a controller trial, not acceptance of ramp feel or a landing test.

Run 74, DLL 90cac7fdafee2b1f110aa52f6e13f7b9e8a7d85bf9820ede071d13ae870bcac9,
completed 9.016666 native seconds in 57.671 wall seconds. Preparation succeeded
with the new mount guard and arrival telemetry. It climbed the ramp, reached
.93155 m COM rise and recorded first landing support at serial 1120. The runtime
reported .5 s of custom-support absence; independent momentum checks establish
only .08333 s and .06667 s uninterrupted gravity-only stretches. Unexplained
native impulses occur between them. The overall result remains **failed**:
`lost_support_during_stop`.

The fine grid brackets the lip at uphill progress 1.20–1.25 m. Full-rotation
capsule geometry puts the rear backup at approximately zero gap to the far end
of this bracket during the unexplained impulses; the front and frame remain
well clear. The logical .34 m tire would still contact this finite edge after
the infinite-plane projection leaves the ramp. This supports a missing edge
feature as the next hypothesis; it does not identify a native manifold directly.

Run 74's first landing is front-wheel-only, with body/support dot .6275, falling
to about .549 before braking. Prior-observation point closing speed was 3.15589
m/s, its labeled gravity estimate 3.31922, and post-contact speed 1.64515. The
new telemetry exactly matches the adjacent archived native observation. The
driver starts braking after .06667 s of front support. Its first support loss
at serial 1130 still has a fresh hit and .852 mm gap: point opening speed
.05691 m/s crosses the .05 m/s eligibility threshold. Normal load remains
344.6 N because the next gravity step closes the constraint. The balance mode
then chatters with the support classification. Both wheels eventually settle
and the bike stops, but that later recovery does not erase the failed result.

The current search's steep-toe coverage and landing evidence have passed 30
portable/MSVC suites and focused strict compiler checks. Steep tests use actual
analytic planes through 80 degrees relative to body-up, including yaw/fakie,
fresh witnesses, lip clearance, depth and invalid-query cases. Prior landing
evidence is a conservative scoring estimate; native collision forces and
orientation between observations remain unobserved.

The finite-lip query now refines an observed face-to-clear boundary to at most
2 mm and requires a fresh radial witness at that edge. It retains plane/depth
guards and uses at most 28 rays per wheel. This is a sampled longitudinal edge
feature, not a recovered native manifold or a full swept-wheel query. Balance
continuity can retain assistance for one freshly loaded opening contact that
gravity closes on the next verified step; strict grounding, hopping and scoring
still use the original actual-support criteria. The ramp driver now requires
at least .12 s of continuous measured gravity-only flight, followed by .06 s of
both-wheel support before braking. Separate short clear intervals cannot add up
to a pass, and a front-wheel-only touchdown cannot trigger braking.

Run 75 (DLL 0fcd1fcea78d58cbb523b8b82e23d29946b5d2f70ef06178e8e1544910cbbd27)
failed before pedaling: the ordinary teleport/dismount left the first bike root
.33305 m behind the measured start, whereas the route permits only .04 m of
rollback. The former .35 m arrival tolerance incorrectly admitted that setup.
No ascent or edge contacts occurred; this run does not test the new lip behavior.
The corrected preparation requests the already measured row at local Z +7.0,
retains +7.5 as the fixed route boundary, and accepts only an actual entry inside
the first metre of that boundary and within .09 m laterally. It probes the floor
at the actual arrival, retains the stable on-foot requirement, and rechecks
owner, position, floor, world and editor immediately before body creation.
The existing driving bounds remain unchanged. The corridor is sampled terrain
evidence; it does not prove clearance of the full collider swept volume.

Run 76 (DLL 99c6177452a87beeea8d0411389efc1eb919ef1176ac4c136ab7470f3bd06bdb)
confirms the setup correction: zero horizontal arrival error and first chassis
root .496 m inside the unchanged approach boundary. It completed 9.016666 native
seconds with no rejected submissions or contact-force attenuation. It climbed
the ramp but never qualified sustained flight or a landing; the maximum clean
ballistic interval was .083333 s. At 5.833333 s the inverted bike crossed the
.4 m lateral root limit. The full result remains failed.

The edge fallback ran for 20 wheel observations, including eight at the rear.
Rear contact nevertheless disappeared intermittently near the lip. Offline
replay of recorded full poses against the measured finite plane predicts valid
edge contact during the 4.600–4.633 s misses. That analytical replay does not
reconstruct the missing native ray packets. Later, the rear wheel settles about
30 mm into the logical tire radius while unobserved native impulses occur,
consistent with the smaller backup shape taking over. The approach also loses
speed from 4.16 m/s at the toe to .42 m/s uphill at first rear-edge support.
Independent energy, edge and driver analyses are retained beside the frozen
run. No gain or torque change is attributed to those diagnostic observations.

Run 77 recorded seven genuine native-clear radial queries after the finite-edge
geometry gates had passed. A fresh 0.5–1 mm interior down-hit followed by another
actual radial hit now retries that specific clear case, retaining the total
2 mm boundary uncertainty and all invalid/unavailable/occlusion guards. Maximum
query count is 30 per wheel in this rare case; ordinary planes remain at 18.
Run 78 recovered all five such misses encountered in its diverging trajectory.
It still failed: a later boundary bisection became ambiguous before a radial
query, and maximum continuous ballistic motion was .116667 s, below .12 s.
No pedal torque, balance gain or pass threshold was changed.

Runs 78–79 separately measured the reported jumpiness. In Run 79 the pre-mount
client was 60 updates/s (6.55 ms client frame); mounted client timing fell to
10 updates/s (119.22 ms), while rendering remained approximately 59 FPS.
The mounted 1,250-sample client profile attributes 78.4% of samples to
`NtQueryVirtualMemory`, predominantly below the BMX rider's `WriteProcessMemory`
lane writes. Explicit write-zone timing averaged about 47.5 ms per pose callback,
whereas pose solving averaged .39 ms. Nested zone totals overlap and must not be
summed. This identifies the guarded pose-write path as a concrete performance
bottleneck, without implicating intentional time scaling. Exact source, DLL,
matching PDB, logs and before/after stacks are archived in
`build/bmx/native-ramp-run79`. The controlled next test changes only those local
pose writes and retains the same physical tuning.

Run 80 replaced only the rider's changed quaternion/hip lanes with bounded,
SEH-guarded local copies. The helper preserves ownership/layout/TTL checks,
stages finite source values, rejects cross-page lanes and does not alter page
protection. Optimized Windows fixtures cover writable, read-only, no-access,
guard and invalid bounds/source cases. All 31 BMX tests passed. The native
rehearsal retained identical physical outcomes and all 1,082 rider pose writes
and 541 render publications. Nine simulation seconds took approximately 16 wall
seconds instead of 32; this is an improvement, not real-time acceptance.
Mounted rider callbacks now average .45-.48 ms, including about .41 ms solving.
The remaining 53.6% `NtQueryVirtualMemory` profile is predominantly the separate
board-hide scale writes in `on_render`; those deliberately remained unchanged.
Both native runs closed their exact owned process with two absence observations
and retained unchanged source manifests. The ramp still fails its flight and
landing gates, independently of the rendering performance correction.

Run 81 changed finite-edge midpoint queries to the fitted face normal. All 56
rear edge candidates verified, with no ambiguous refinement rejections; the
previous 27 failures disappeared. A separate initial-face gap remains, so the
ramp still does not qualify sustained flight and settled landing. Run 82 then
changed only the board-hide scale stores to the same bounded local-copy policy,
preserving the two root bones and skipping already-zero descendant XYZ lanes.
Its mounted status measured 60 client updates/s and 60 rendered frames/s;
9.016666 native seconds took 9.109 wall seconds. The render callback averaged
.1964 ms, compared with 1.5226 ms in Run 81. Earlier WPM latency varied widely,
so these measurements do not establish a fixed speedup on other machines.

Run 83 independently passed flat riding with the current Streets controls:
one caught barspin, one caught tailwhip, one landed combo and the next-step
left-stick pitch response (.03532 rad/s). Its 9.016666 native seconds took
9.125 wall seconds, with 541 force applications, 1,082 rider pose updates and
541 fresh owned render publications. The hop rose .99829 m and final COM speed
was .32796 m/s. This uses synthetic game-side input; controller feel and native
D-pad usability require physical acceptance. Exact source/DLL/logs are archived
under `build/bmx/chassis-ride-run83`.

Run 84 added a bounded fresh nearby-patch query only after the original surface
fit failed at a partly clear boundary. All six previously missing rear contacts
were recovered from new native hits, without a cached plane or a relaxed support
gate. The rare fallback uses at most 42 rays per wheel; ordinary flat, clear-air
and rough fully hit paths retain their previous budgets. This fixed the initial
face gap, but the 60 Nm pedal setting still rolled slowly over the lip rather
than producing the required flight.

Run 85 changed pedal torque to 75 Nm. It recorded .483333 s of continuous measured
gravity-only flight, followed by both-wheel support sustained beyond .06 s.
The maximum COM rise was 1.037628 m. It nevertheless failed the unchanged 5 m
route boundary during braking: actual root travel reached approximately 5.607 m.
Run 86 raised front brake torque from 25 to 60 Nm, retaining the 45 Nm rear brake.
Flight and landing remained valid, and stopping improved to approximately
5.158 m, still a failure. Both runs took approximately 9.11 wall seconds for
9.016666 simulated seconds. The ramp driver's progress tally stops updating after
failure; these stopping distances use the continuing actual bike-root samples.

The Run 86 trace identifies a low-speed brake/contact coupling defect. The
controller brakes each free rotor before solving road contact; contact then
spins the rotor back up, without another brake solve. Below approximately
1.5 m/s, the impulse needed to stop the free rotor is smaller than the available
brake budget. The resulting braking force falls roughly in proportion to speed
and produces a long creeping stop. Increasing maximum torque cannot remove that
tail. The next change couples bounded accumulated brake impulses with the tire
contact iterations, preserving the per-step torque budget and equal/opposite
rotor/chassis reaction. It must pass conservation, passivity, traction and output
cap checks before another native run. The 75/60/45 Nm settings and all native
flight, landing and route limits remain fixed for that comparison.

The successful flight does not establish complete landing fidelity: Run 85 also
recorded a native backup-collider impulse during touchdown, after a torque-capped
wheel response and approximately 30 mm rear-wheel penetration. That occurred
after the qualified flight and before the settled landing. Broader terrain,
impact handling and physical-controller feel remain separate acceptance work.

Run 87 passed the same native ramp route with the coupled brake correction
(DLL `B9328A21E53C857BE4ED9E2E8596D1CCA6770C3B9F7E29E962A84D80CF8B9E5D`).
The brake-active solver replaces accumulated normal and joint brake/friction rows
separately, so each wheel's own brake reaction participates in its support load.
The joint block minimizes kinetic work within the friction ellipse and the
per-step brake torque interval. Motor torque is applied once. Brake/contact
responses share one output fraction across chassis, rotors and reported impulses.
It uses 96 bounded brake-active sweeps for static convergence; the original
12-sweep path and rotor arithmetic remain unchanged when no brake torque applies.
Two thousand old/new cases verified that inactive-brake behavior bit for bit.

Both strict GCC/MSVC fixtures and all 32 BMX CTests passed. Independent checks
cover momentum, passivity, torque budgets, static hold, backward motion, moving
ground, zero/limited friction, airborne braking and coherent output attenuation.
No speed/pose clamp or physical gain change is part of the correction. Offline
stress timing was approximately .17-.53 ms per solve (worst batch mean .60 ms);
native Run 87 measured 59 client updates/s and 60 rendered frames/s, with
9.016666 native seconds taking 9.093 wall seconds.

Run 87's body states through the first brake sample match Run 86 exactly, as do
all pre-brake force/torque/contact commands. Independent motion analysis measured
.483333 s of continuous post-lip gravity-only flight and .066667 s of settled
two-wheel support before braking. Actual braking distance improved from
1.331261 to 1.044715 m; maximum root progress was 4.871706 m inside the unchanged
5 m bound. Both wheels remained supported throughout braking, final sample speed
was .001615 m/s and there were no rejected native submissions. Braking's maximum
unmodeled native impulse was approximately 1.48e-5 N*s. The known touchdown
backup-collider impulse remains unchanged. Exact source, DLL, matching symbols,
profiles and independent comparisons are in `build/bmx/native-ramp-run87`.

Run 88 repeated the current Streets controls with this same DLL and passed:
one caught barspin, one caught tailwhip, one settled landed combo and no failures.
Left-stick Y produced the next-step pitch response; exercised right-stick-only
phases produced no pitch command. Independent event/identity analysis passed
21 checks. Its 9.016666 native seconds took 9.313 wall seconds. This proves the
scripted combinations on the physical backend; it does not establish physical
controller/D-pad acceptance, complete Streets physics or the full trick catalog.

Run 89 passed the physical equipment lifecycle with the same DLL. Its quarter-
second pause preserved the exact body token and pose with one allocation. Six
ordered status samples verified BMX, native skateboard, a quiet skateboard
interval, returned BMX, walking and final BMX remount. The board interval kept
BMX force/placement counters unchanged with input capture released and the body
parked. All three mounts reused the same allocation. The supervisor verified
the exact process, unchanged source and two post-stop absence observations.
See `build/bmx/chassis-ride-run89/switch-evidence.json` and
`bmx-chassis-lifecycle.json`. These are native state/activity checks; visible
board recovery and physical button operation still require user acceptance.

## Expanded collision envelope and current native checks (October 18)

The physical pool now includes eight conservative capsules covering both wheels,
frame, bars, saddle and the seated rider's hips, torso and head. Fresh native
surface witnesses also check the current and previous body-envelope positions.
They distinguish unavailable queries from clear space, bound the historical path
to 1.5 m, and require contact evidence before a severe inversion can request a
bail. An airborne inverted orientation by itself is not a crash. Impact decisions
precede new wheel/control forces and trick landing credit. These are conservative
envelopes, not animated limb collision or native convex continuous collision.

Run 90 passed the existing ramp with this expanded envelope (DLL
`5E4541B938B98DDF6796BABB074B02814352DF50E68B6D3110BD34C5762C7CA4`).
All 541 measured physical samples matched Run 87 exactly: body position,
orientation, velocity, forces, torques and rotor state. Flight remained .483333 s,
settled support .066667 s, braking distance 1.044715 m and final speed .001615 m/s.
There were no false bails or rejected submissions on that route. This establishes
ordinary-ramp regression coverage; it does not establish inverted or side-impact
recovery. See `build/bmx/native-ramp-run90/independent-baseline87-comparison.json`.

Run 93 captured 361 coherent native samples after the existing board wipeout
request with the private diagnostic profile's NoBail setting disabled. The game
moved from board state100 to animated offboard504, never cleared animated bit13
and never entered native ragdoll. A completed observation file is therefore not a
working fall. The BMX adapter still fails closed until the authored activation
contract is established; simply queuing the pending ragdoll state is insufficient.

Runs 95 and 96 used DLL
`B0936B896B11C926764FBF3D2F83D76BE4967299C2F2039C0B9B761085434DC4`.
Run 95 passed bike/board switching, pause/resume, dismount, carry, verified set-down
and remount with one native allocation. During the quiet carry interval, successful
render-pose writes improved from Run 94's 7.32/s to 60.04/s and maximum sampled hand
error fell from 49.915 mm to .031 mm after raising the bike .12 m and moving it .06 m
inward. Native force/follow counters stayed unchanged while walking/carrying.
Run 96 selected Superman through the recovered modifier/right-stick combination,
observed eight distinct completed render writes at at least .9 movement phase,
caught and landed one movement with no failed combos, and passed the unchanged
physical riding checks. Its 9.016666 native seconds took 9.219 wall seconds.
Both supervisors preserved source and binary manifests and confirmed process
absence twice after closing. Physical controller feel, walking pixels, the other
expanded trick poses and impact/ragdoll recovery still require separate acceptance.

The subsequent guard review found a missed crossing when a sphere already partly
overlapped the surface, then its center passed through. The old condition required
the previous entire sphere to be clear. It now requires the previous center to be
on the positive side and the current center behind the face, while retaining a
fresh fitted plane, exact historical segment hit, consecutive owner history and
the 1.5 m path/depth bounds. Strict GCC/MSVC tests cover a -.18 m sphere gap after
shallow overlap, policy-history acceptance, missing-history rejection, and finite
face crossings both inside and beyond the real edge. All 35 CTests pass.

Run 97 passed the prior barspin/tailwhip combo with that fix (DLL
`F3971A887608330394D20080385F2897C88232788739F1DB1013279610A25100`),
including the same .0353217 rad/s next-step left-stick pitch response and no failed
combos. Its 9.016666 native seconds took 9.109 wall seconds.

Controlled impact Runs 98/99 used the same binary and remain **failed complete-fall
tests**. The upside-down seed detected a head impact at serial30, alignment
-.999941 and incoming closing speed 4.571890 m/s. The side seed detected severe
frame-contact tilt at serial34, alignment .019162 and closing speed 5.225017 m/s.
Both stopped the chassis and released BMX control before attempting native fall;
both reported zero native activation calls and no observed ragdoll because the
rig remained animated. Manual NoBail was explicitly disabled only in this private
diagnostic profile. Archived reports and two confirmed process absences are in
`build/bmx/native-impact-run98` and `native-impact-run99`. This proves these sampled
contact-trigger/release cases, not general map-clipping recovery or working crashes.

After those runs, the interim status string was corrected from implying native
fall handling to "BMX impact: bike control released." The existing final failure
message already reported that the native fall request was unavailable. Original
run reports/logs are preserved; this text-only change does not alter their physics.
