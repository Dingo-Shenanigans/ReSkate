# Temporary BMX Streets observer

This development tool is a read-only MelonLoader observer for the installed IL2CPP BMX Streets build. It supplies evidence for calibrating the independent C++ module. It is not a runtime dependency of that module.

`build.ps1` compiles against the already-installed game wrappers and .NET 6 runtime using the Roslyn compiler bundled with PowerShell. It does not install an SDK, download packages, deploy itself or execute the game.

The parent workflow temporarily copies `bin/BmxMovementObserver.dll` into the game's `Mods` directory and preserves/restores user mods separately. Any physics-changing mod must be recorded in the baseline or disabled reversibly before claiming an unmodified game reference.

Environment variables passed only to the explicitly launched game process:

- `BMX_OBSERVER_OUTPUT`: fresh directory below this workspace's `standalone/bmx-movement/recordings`.
- `BMX_OBSERVER_START_GATE`: fresh marker JSON within that output directory; defaults to `start-gate.json`.
- `BMX_OBSERVER_DURATION`: 2–300 seconds after the marker is observed, default 14.
- `BMX_OBSERVER_FPS`: 1–10 actual framebuffer samples/second, default 10.

The observer remains inactive when no output variable is supplied. It refuses an existing output directory. `observer-initialized.json` proves initialization; `observer-ready.json` appears only after a local driver vehicle and its chassis body are found. A diagnostic framebuffer JPEG/JSON is refreshed once per second before the capture gate and after capture completion. It captures no desktop or other application.

The controller workflow writes the start marker only after its neutral bootstrap. The observer then records:

- `telemetry.csv`: measured chassis COM, velocity/angular velocity/orientation, current effective gravity/fixed timestep/time scale, grounded state, local input fields, wheel loads and classic/v2 trick state where available. `vehicle_com_x/y/z` separately records the vehicle's combined COM observation. Chassis mass is not combined rider/bike mass.
- `rider.jsonl`: all 55 `HumanBodyBones` slots (null when unmapped), actual world/local transforms and parent IDs, selected limb aliases and IK anchors/targets per captured frame. Animator and chosen chassis/vehicle IDs identify the observed relationship.
- `runtime-inventory.json`: active associated rigid-body masses, principal inertias, COMs, joint/body connections, gravity/timing, available vehicle/user/spin values and authored `AnimationCurve` keys/wrapping. Associated-body totals can include kinematic avatars, ragdolls, cameras or helper bodies and are not a coupled physics mass.
- `frames.csv`, `frame-*.jpg`: actual Unity end-of-frame images from `ScreenCapture.CaptureScreenshotAsTexture`, encoded with the game's ImageConversion API.
- `capture-start.json`, `capture-summary.json`: process, marker and UTC timing, frame/sample counts and completion status.

The observer never sets a transform, input, force, velocity, physics parameter, animation state or game preference. Destroying a temporary screenshot texture only releases its own capture resource. Callback order relative to the game's physics scripts is not established; the CSV identifies the actual `MelonMod.OnFixedUpdate` sampling hook. Screenshot encoding can affect frame pacing, so measured timestamps are retained. Missing/unavailable values remain `NaN` or null and must not be treated as zero or inferred success.

Remove the deployed observer after the controlled run and restore preserved user mods. Keep all raw observations and driver reports before deriving calibration conclusions.

Version 0.2 can capture several controlled runs without restarting the game. After a capture completes, write `next-capture.json` into the **original output directory**:

```json
{"name":"streets-clear03","duration_seconds":32}
```

The name must be one fresh sibling-directory basename. The observer creates that sibling, writes its ready/initialization records and waits for the new folder's `start-gate.json`. It preserves previous telemetry, frames and summaries, writes a numbered request-hash receipt in the original control directory, and refuses an existing destination. Subsequent requests still go into the original output directory. The diagnostic JSON includes current chassis position, quaternion, velocities and combined vehicle COM for selecting a clear starting location through ordinary game controls.


Version 0.3 adds independently sampled body/joint and animation observations. Build.ps1 compiles both C# sources. New settings:

- BMX_OBSERVER_BONES_FPS: 1�30 pose samples per second, default10, independent of video. These use OnLateUpdate; video uses WaitForEndOfFrame.
- BMX_OBSERVER_PHYSICS_HZ: 1�100 samples per simulation second, default100. At the observed100Hz engine timestep every FixedUpdate callback is retained, including callbacks clustered in one rendered frame.
- The duration and repeated-capture duration limit is300seconds. Frame limits scale with the requested duration instead of stopping at600images.

New outputs:

- body-physics.jsonl: eight selected bike-body roles; fixed index, elapsed/UTC/fixed simulation time; transforms, local/world COM, velocities, mass/inertia, gravity/drag and joints. Each ConfigurableJoint drive independently reports spring, damper, maximum_force and use_acceleration, plus motions, limits, anchors, targets, mass scales, projection, collision and current reactions. Unity currentForce/currentTorque are reaction observations, not a force decomposition. Spring working values, input/grounded fields and managed-body list references are included.
- trick-states.jsonl: matching fixed indices, actual selected/fired tuples and guards, controller phase and loop count, definition identity, player/vehicle Animancer states for Enter/Tweak/Loop/Exit. State observations are not synthesized scoring-completion events.
- trick-catalog.json: all64 separate set/slot positions with canonical names/availability, plus raw vehicle-provider, vehicle-settings and brain-fallback role tables. Clip identities, duration and speed properties are read; no clips or animation curves are copied. A bound native vehicle provider resolves the actual slot and can override its raw table; fired definition/state records are authoritative.
- rider.jsonl retains all55 humanoid slots, sampled separately from images. pose_reference is the nearest actual Rigidbody ancestor of the selected humanoid Animator, if one exists, with COM position/body quaternion and explicit binding. All ancestor candidates and the separate Spring candidate are listed. An unrelated Spring body is not silently treated as the skeleton holder. FullBodyMotion IK helper weight/target-ID transforms are included when discoverable; missing values remain null.
- observation-schema.json records coordinate, timing, sample and phase-enum boundaries. fixed_index is the zero-based telemetry row index; elapsed uses the same capture Stopwatch. Pose and video retain their actual independent sample times.

The observer adds no animation/event handlers, input injection, physical writes, or calls to Fire/Resolve/Play. Runtime validation must establish field availability and useful coverage before using a capture to calibrate the module.


V3.1 preserves every native fixed callback in `telemetry.csv`, including periods where a bail removes `LocalDriverVehicle`. Invalid rows have `valid=0`, unavailable values as NaN, and explicit `fixed_index`, `binding_reason`, and `current_vehicle_id`. `body-physics.jsonl` emits matching `sample_valid=false` rows with all eight roles unavailable. Missing local ownership is expected source unavailability and does not increase the exception counter. Body, joint, animation and output channels report actual exceptions separately in `channel-errors.jsonl`; one channel failure does not suppress the others. `selection_current` on pose/trick observations identifies whether cached character data are still bound to a current local vehicle. Cached character playback can remain observable during a bail, but is not represented as current vehicle physics. Upon local vehicle reacquisition, scoped references are cleared and discovered again, including humanoid, IK, input, trick brain, joints and motion component. Capture summaries separate valid/invalid fixed ticks and per-channel exception counts.

Runtime video/pose clocks retain their original capture UTC origin and simulation time. Invalid fixed rows remain in the timeline and must not be interpolated into valid native physics or treated as successful trick landings. Lower requested physics rates intentionally sample fixed callbacks by simulation time, so `fixed_index` gaps at those rates represent sampling rather than inferred missing engine updates.

Version 0.3.3 makes capture startup transactional. It reads and parses the complete marker before setting the started state, retries transient shared-file or partial-JSON reads every 50 ms for at most 2 seconds/40 attempts, and opens all six essential channel writers together. A failure disposes writers and removes only the files created by that attempt. A rejected start emits capture-start-failure.json; a successful capture-start.json includes marker-read attempts and all_channels_opened. The controller publishes markers atomically as a separate safeguard.

Version 0.3.3 also records rider.visual_transforms: actual renderer and ancestor transforms, with world/local and Chassis-COM-relative poses. Renderer binding inventories and each pose include the relevant Unity ParentConstraint states, source transforms, source weights and offsets. Source ancestry identifies selected physical bodies where verified. These observations distinguish animation control roots, physical parts and actual visible mesh transforms; direct binding must not be assumed where absent.

The V3.3 build SHA256 is 221F513AED4945DF26B21937ACF9F3653005F4F4F1D828F5CD7FFDC5BF65BD06 (243200 bytes). Build source only; installation remains a separately coordinated temporary operation.