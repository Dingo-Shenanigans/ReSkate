# Analysis and implementation boundary

Local static and runtime investigation, October 21, 2026. The current BMX Streets build and the standalone demo were driven sequentially with the installed virtual XInput controller and recorded from their actual framebuffers. A temporary observation-only MelonLoader mod captured native physics and rider bones; CatMod was temporarily disabled to avoid its handling overrides, then restored with its original hash and the observer removed. The installed game binaries were not patched.

## Pinned source pair

The installed source changed during discovery. A coherent pair was copied into the ignored `build/bmx-movement-current/source` analysis directory and compared against the installed files after copying:

| Artifact | SHA-256 |
| --- | --- |
| GameAssembly.dll, 112,962,048 bytes | `22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8` |
| global-metadata.dat, 33,108,228 bytes | `7e1af5100b90a78c907999239bc6952dfdce2461fc4321626658f24f2815d7ea` |

Cpp2IL regenerated the current method metadata and mapped 200,703 definitions. Its generated declarations establish names, signatures, fields and native coordinates; their empty bodies are not recovered source. `reference/evidence.json` contains the bounded movement inventory and provenance checks. The old `3d3042...` image and `3fb61e...` metadata do not identify this pair, and old RVAs cannot be reused directly.

## New native observations

An incomplete, never-executable analysis carrier retained exact selected current code pages and original data. Its SHA-256 is `4281c0ede8b175656cc5286e1ef99f640661ef899407b8587799a789bb5a6772`. The local manifest pins each retained method's original offset and digest. REA/Ghidra inspected five functions; the 5-record, 5,282,315-byte evidence bundle is saved under `build/bmx-movement-current/rea-movement-native-evidence.json` in the workspace. It is not required to build the module.

| Function / current RVA | Evidence ID | Established static behavior |
| --- | --- | --- |
| SpinSystem.RunRotationPhysics / `0x8d1760` | `ev_01e6023f14fe05135a2edd0b784605a42a57eb0f4713a1ccf6d959253763f96a` | Separate spin/flip inputs multiplied by settings and per-player multipliers; blended angular-velocity gates; torque scaling includes fixed delta time, `_torqueMult`, and 100. Quick-spin state has additional conditions. |
| SpinSystem.Tick / `0x8d2130` | `ev_39e50ed9d2d5d0af8fad0ffce8bd973181ba0425cc367c09e69b5e2c83cbb88b` | Returns when `BMXCollisionHandler.AnyColliding`; otherwise runs rotation physics and limits rigid-body angular-velocity magnitude to the configured maximum. |
| IKTargetManager.UpdateTarget / `0x8b0000` | `ev_e6efab86c8b5a950c618c70017bca3c8c0531772d7aa4b513b4d1a80f72da625` | Position comes from `anchor.TransformPoint(offset)`; rotation multiplies anchor rotation by the quaternion constructed from degree offsets converted to radians. This sets targets; it does not solve the rider skeleton. |
| LandActivity.Land / `0x8ce050` | `ev_9cfa72f86b6ef600faa0f3213b1adb236991c3c627f0eb4e730b6d07828cc65e` | Activity/trick detection, report construction, callback and collection clearing. This method does not by itself recover contact physics or landing animation. |
| NoseyActivity.Track / `0x8ce910` | `ev_332339cb93bd6be4d9e951e7b9bc015a63450872542ab04d0bc0394985115c7e` | Contact/eligibility checks, timer threshold, one-shot enter event, then duration accumulation. This is tracking logic, not a recovered physical nose-manual controller. |

Named callees were matched against freshly generated current metadata: `Transform.TransformPoint`, `set_position`, `get_rotation`, `set_rotation`, `TransformVector`, `Quaternion.Internal_FromEulerRad`, `Rigidbody.get_angularVelocity`, `set_angularVelocity`, `Time.get_fixedDeltaTime`, `PlayerSmartFloat.get_Value`, `AnyColliding`, `AddBlendedRotationTorque` and `GetBlendedLimiterVelocity`.

Ghidra pseudocode is a representation of machine code. Some computed calls, initialized state and helper implementations remain unresolved; terminal-call flow assumptions are retained in the bundle. Static analysis does not observe effective live tuning, player settings, input bindings, mod overrides, collisions or animation.

## What the C++ module represents

The force/contact solver, fixed-step host and scoring are independent C++ code. Version 0.3.0 adds measured spring commands and posture, bike-root and IK-control tracks for all 58 available selections from the current active vehicle provider. The provider has eight sets of eight slots, including six empty slots. Reset-per-selection runtime captures observed all 58 fires and completed exits to Idle; all six empty slots remained empty. Current catalog and timing provenance supersede the historical catalog used by v1. Native landed scoring is not exposed by the observer.

`CoupledMovement` reduces the physical rig to a 30 kg aggregate bike and an 80 kg rider joined by an implicit six-axis acceleration-mode spring, retaining measured principal inertia orientation and radial limits. Native target commands and four-pulse hop behavior drive that model; sampled posture is retargeted through an independent rider-driven presentation holder. The observed native skeleton holder is AnimatedSkeletonRigidbodyCOM, not a Spring ancestor. Thirteen vehicle/control roots and four IK limb-control channels per available trick preserve availability flags and literal weights. See [the v2 profile](reference/observed-profile-v2.json), [rig reduction](reference/selected-rig-reduction-v2.json) and [comparison report](COMPARISON.md).

Coverage remains partial. The reduced contact and joint model, all input branches, full runtime IK solve, ragdoll/bail/recovery, walking, rail acquisition and advanced grind classification have not been fully reconstructed or validated. No original game DLL, animation asset or incomplete analysis carrier is included in the standalone distribution. Numerical fit and observed selection coverage do not establish full game parity.

## Flick input and physical policy

The pinned current `FlickIt` consumer and exact serialized roundtrips establish processed-stick thresholds of 0.6 to arm, 0.45 to cross, and a 0.2 s crossing timeout. Ollie uses negative-to-positive Y; Nollie mirrors it. The crossing timer advances only after leaving the preload side and pauses on returning there. Each instance has a 0.2 s repeat interval. Opposite-event callbacks set only `requireZeroOut`, preserving its pending latch/timer; passing Y −0.1 for Ollie or +0.1 for Nollie clears that gate. The source ticks Nollie before Ollie, using the preceding step's manual/nose hold callback. Serialized `minFlickTime=0.1` is not read by this recovered path and does not establish a charge requirement.

Original `InitializeInputs` delegate usage words resolve through the pinned metadata to `SetOllie` / `SetNollie`, which write staged jump sizes +3 / −3. `GetVel(abs(size))` selects the current array's third element, 4.5 m/s; the native full-RS flick trace independently confirms that command. R3 remains the distinct 6.4 m/s quick-hop path. Source Nollie eligibility rejects a negative jump from Manny contact, suppresses its Nollie flag from Nosey contact or peg contact, and scales the retained Nollie vector by 0.95. The C++ option explicitly reduces those native front/back-end contact predicates to its two eligible wheel supports. It does not reconstruct the native peg/end collider graph. See [flick policy provenance](reference/flick-hop-policy-v2.json) for hashes, current object bindings and evidence IDs.

Actuation ID83 adds separately recorded full-negative preload, Ollie transition and neutral recovery commands. Recovery comes from a different moving control attempt with no hop, rather than from the launch trajectory. Its neutral-relative boundary differs by about 56 mm; runtime joins the preceding applied target with an independent offset that fades by the measured phase endpoint. Holding an enter endpoint for an arbitrary duration and lifecycle cancellation are also host policies. No separately observed Nollie actuator track is bundled; its generic quick-hop fallback and the unmeasured hold-assisted cases remain physical-animation limits. Native coroutine/render scheduling is distinct from the configurable input-to-pulse timing fit.

## Earlier v1 runtime evidence and hop analysis

The actual game UI identifies build **2.0.0.3063.0 (2026-10-20)**. Runtime observation confirms a 0.01 s physics step and gravity −12.5 m/s². `reference/calibration-current.json` contains 82 strictly decoded, round-trip-verified current serialized settings; runtime inventories retain effective values separately, including the player's quick-hop multiplier 2 versus the asset default 1. Six SmartData layout failures remain explicitly unavailable.

`streets-clear02` contains 300 actual 1280×720 game frames, 2,873 fixed samples, 55 skeleton slots with 52 mapped bones, and zero recorder errors. Its full controller timeline has 1,434 matching XInput readbacks and verified cleanup. The pedal phase stays approximately level; the bike crosses a drop during the brake phase. `streets-flat03` uses a shorter timeline and remains level throughout pedal/coast/brake: 131 frames, 1,272 fixed samples, 577 matching readbacks and zero recorder errors. Recorded world/simulation clocks, body identity and source frame hashes are retained. These observation counts are not claims of native 100 fps video.

Additional REA/Ghidra analysis covers current FlickIt dispatch, quick-hop multiplier/selection, Jump, wheel steering, blended rotation torque, trick selection and animation-input preparation. The 12-record evidence bundle is `build/bmx-movement-current/rea-physics-match-evidence.json`, 11,382,734 bytes; carrier SHA-256 `acb599af2df13db6435c11cc77fb7d5049dc65eab95fdf446bc1ff557066213e`. Native trick selection corroborates the 0.7 radius and angle sectors; runtime confirms **(set 3, slot 4) Barspin Left** and **(set 5, slot 2) Whip Right**. This confirms two selections, not every historical catalog entry or all authored motion.

The `EnumJump` coroutine and consumers establish four quarter-vector velocity changes separated by fixed-update yields, a separate settings wait of 0.06 s, and conditional quick-hop/nollie/manual multipliers. `GetVel` selects `JumpVelocity[size-1]`; the unrelated `Vel=5.2` field is not read by that consumer. The earlier v1 pass did not export the runtime array. V3 subsequently measured `[3.2,3.2,4.5]` and `lastJumpForce=(0,6.4,0)`, and established the first Chassis recipient is applied twice by the native iterator. The v2 reduction retains that multiplicity explicitly rather than treating 6.4 as one whole-rig impulse. See [current-hop-observations.md](reference/current-hop-observations.md) for evidence IDs, source bytes, control flow and limitations.

The independent observed profile fits clock/gravity/radius, skeleton dimensions/neutral placement and staged hop behavior. Mass and inertia remain independent: the eight selected vehicle bodies sum to 110 kg, while a broader joint graph also contains active rider/hair bodies. Neither naive sum establishes the combined controller's effective mass. Original contact, multi-body coupling, speed-dependent braking, detailed pedal/IK motion, all trick trajectories and ragdoll remain fidelity gaps.

Use `tools/snapshot_reference.py` and `tools/verify_reference.py` to refresh/check local reference identity when available. They are development analysis tools, not runtime dependencies.
