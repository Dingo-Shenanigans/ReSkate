# Current serialized physics reference

The current local BMX Streets build was inspected without executing game code for this extraction. Source images are pinned in `calibration-current.json`; accepted custom objects were decoded from current IL2CPP-generated type trees, checked against the common Unity header, read completely and serialized back to the exact original bytes.

The report accepts **82 typed objects**. Six SmartData definitions failed strict layout verification and remain unavailable. These are authored settings; active prefab selection, per-player settings, runtime tuning and CatMod overrides require live observation.

| Source | Authored values |
| --- | --- |
| TimeManager | Fixed timestep `0.009999999776482582` s, approximately 100 Hz; maximum allowed timestep `0.03333330154418945` s |
| PhysicsManager | Gravity `(0,-12.5,0)`; solver and velocity iterations 20; contact offset 0.0025 m |
| MotorVehicleSettings 47332 | Chassis mass 10 kg, driver mass 80 kg, wheel settings mass 8 kg; top speed 10 |
| EngineSettings | `_forceFactor=0.07`, `_maxSpeed=7.5`, `_maxForce=10`, wheel propulsion enabled, direct propulsion disabled, front/rear gear 25/9, RPM animation boost 8 |
| WheelSettings | Radius 0.26 m, max steer 18 degrees, friction constant 2, ordinary friction branch, ratio multiplier 2.6, disconnected multiplier 0.7 |
| AirControlSettings | Spin/flip scalars 0.28, angular-speed maximum 12, angular drag 2.75, chassis-body blend 0 |
| JumpSettings | `Vel=5.2`, selected jump velocities `[3.2,3.2,4.5]`, relative/driver shares 0.5 each, force wait 0.06 s |
| HandlingSettings | Max lean 16 degrees, extra lean 14 degrees, ground-steer torque scalar 45, manual-steer scalar 40, fakie-manual scalar 250 |
| StabilizationSettings | Z/upright scalar 1200, X scalar 800; driver spring linear limit 0.3 grounded / 2.5 air; linear damping 140, spring drive 1100 |
| Player SmartData defaults | Quick-hop multiplier 1, jump multiplier 1, flip multiplier 1, spin multiplier 1; these decoded definitions do not prove saved/current values |

Units named in Unity built-in schemas are retained. A custom field called torque or force does not establish an SI quantity until its native consumer, ForceMode and time scaling are resolved. In particular, `SpinTorque=0.28` must not simply replace the independent controller's 200 N m torque.

Four serialized Proto_BMX roots were found: 8827, 9099, 19406 (Menu) and 19875. VehicleController objects 54074 (root 19875) and 60034 (root 9099) both point directly to settings object 47332. The active live instance remains unknown in this static report.

Every root contains eight Rigidbody objects with authored masses: spring/driver 80, chassis 10, wheels 10 each, headset 1, bottom-bracket 1 and pedals 1 each. Do not sum these and declare an effective rider/bike mass: settings explicitly replace wheel mass with 8, and runtime connections, kinematic state and selected rigid-body roles matter. Driver inertia is authored `(6,8,6)` grounded but `(150,150,150)` in air; chassis inertia is `(1,1,1)`. These are separate bodies, not the tensor of the module's combined rigid body.

Component defaults also differ from settings: WheelPhysics carries radius approximately 0.26308, inertia 1.1, friction constant 1 and disconnected multiplier 0.2 before settings application. The report preserves both sets and their exact object identities.

The standalone `SimulationSettings` permits a 100 Hz comparison profile while keeping existing 60 Hz defaults. It exposes clock, reference mass/inertia, wheel radius/inertia and hop scheduling. The default hop remains instantaneous; the observed profile emits four explicit impulse parts after a short delay. Each returned part is applied once, separately from sustained force. Its wider query horizon and larger response budget address fast impacts in the reduced reference host. Those numerical policies are independent of the native contact offset and coupled-rig force distribution. They do not establish full numerical game parity.
