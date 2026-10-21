# BMX Movement

A standalone C++20 library for bicycle physics, coupled rider suspension, rider posture and tricks. It has no Unity, ReSkate, Frostbite, Steam or game-install dependency. Link `bmx::movement`; the closest measured profile is exposed through `<bmx/suspension.hpp>`.

This is an independent, configurable adaptation informed by local BMX Streets analysis. It is **not a complete extraction of Streets' movement implementation or a verified handling match**. The source, runnable Windows demo, tests and reference records are included.

The distribution includes a Windows demo, test executables, source, and actual original-game/module comparison recordings. The included MinGW static archive is compiler-specific; build the source with your game's compiler when integrating it. Session02 contains the expanded 64-slot investigation; earlier profile-v1 recordings remain in the workspace's `recordings/comparison-session01` and the preserved 0.2.0 archive.

## Coupled observed profile

```cpp
#include <bmx/suspension.hpp>
#include <bmx/pose_track.hpp>
#include <bmx/vehicle_pose.hpp>
#include <bmx/ik_track.hpp>

auto profile = bmx::streets_observed_v2();
bmx::CoupledMovement movement(profile);

// Once per profile.movement.simulation.seconds, normally 0.01 s:
// chassis and rider are fresh measured Body states; contacts are front/rear.
auto result = movement.step(input, chassis, rider, contacts, body_generation);
if (result.valid && result.converged) {
    // Update host rider inertia from result.rider.inertia_body and
    // result.rider.inertia_orientation before applying this tick's wrench.
    // Apply result.bike.force / torque to the chassis at its COM.
    // Apply result.rider_force / rider_torque to the rider at its COM.
    // Apply bike.hop_impulse and rider_hop_impulse once, without dt scaling.
    // Apply rider_angular_impulse as a world angular impulse once.
    // The host applies gravity separately to both bodies.
}
```

`bmx::CoupledWorld world(profile)` supplies a runnable flat-plane host. `bmx_demo --streets-observed-v2 --camera-behind` runs this profile with live XInput. D-pad Down resets the reference host between recorded attempts. The older `Movement`/`FlatWorld` APIs and default tuning remain available.

In the Windows ZIP, double-click `bin/run_observed_demo.cmd` to launch the observed v2 profile. In a source checkout, build first and run `demo/run_observed_demo.cmd`.

The v2 reduction keeps the measured 80 kg Spring role independent from a 30 kg aggregate of seven selected bike bodies. It solves internal joint reactions between measured bodies, including anchor leverage and principal inertia orientation. This remains a reduced rig, not a recreation of every native collider or joint. Your engine supplies world collisions and fresh wheel contacts.

Configure the engine bodies' angular-speed limits from `CoupledSettings` (15 rad/s in v2), and their masses, inertia tensors and principal-axis rotations from the profile. Rider stabilization and manual actuation are external actuators already included in the returned wrench; do not add their diagnostic torques a second time. The native Chassis PID's general animation-weighted target is not reconstructed. The recovered manual branch supplies its own pitch target or direct body torque, enabling the Chassis pitch controller only for that branch.

Check both `valid` and `converged` before accepting a coupled result. `coupling_residual` reports the remaining force/torque change between solver iterations. A host must handle a rejected or unconverged result through its own physics-step policy; accepting `valid` alone does not establish convergence. If the host declines a valid but unconverged result, call `movement.invalidate_sample()` before continuing, or restore the saved controller and body states together; skipping the wrench alone leaves controller history ahead of the host.

The v2 model applies angular damping in its returned torque: rate 1 on supported bodies and the observed rate 2.75 while unsupported. Disable duplicate engine angular damping when using this model's damping. Both rates and the optional airborne switch are configurable.

`profile.coupled.manual_pitch` exposes the source angle/direct-torque modes. Live Nosey Manual mode was observed. The v2 Manny Manual choice is an explicit configurable inference; its moving runtime mode was not exported. The authored defaults are Auto for both. See [manual policy provenance](reference/manual-physical-policy-v2.json), including the mode comparison, input timer, per-body torques and remaining wheel-contact differences.

The v2 input profile also enables the recovered RS flick gestures. Ollie arms below processed Y −0.6 and fires above +0.45; Nollie mirrors those signs. The crossing must finish within 0.2 s spent outside the preload region. There is no minimum charge duration. A previously triggered manual/nose hold lowers its corresponding arm threshold to zero. Each gesture has its own 0.2 s cooldown; firing one requires the opposite detector to pass neutral before another reversal. These thresholds apply after stick processing, not directly to raw HID values.

Normal flicks select the native nominal 4.5 m/s velocity command; R3 selects the separately observed 6.4 m/s quick-hop command. The v2 two-wheel/no-peg support reduction rejects a Nollie request on rear-only support, uses 4.275 m/s on both-wheel support, and retains 4.5 m/s on front-only support. These commands are distributed as four recipient-weighted impulse parts; they are not a promised whole-rig launch speed or height. Full-negative preload, Ollie transition and neutral recovery use recorded joint commands. Nollie and hold-assisted flicks use the generic quick-hop actuator fallback when no matching full-preload track is active; their physical animation was not separately measured. See [flick policy provenance](reference/flick-hop-policy-v2.json).

`Output::hop_request_accepted` identifies a newly reserved physical hop. `actions.hop_pressed` can still describe a gesture denied while another hop is pending; use the accepted flag to start host effects. Simultaneous R3 takes priority over a flick. Ownership, focus, menu, invalid-sample and ground/air cancellation are explicit host policies. The configurable input-to-impulse delay is a measured timing fit; the gesture's crossing timeout is not an extra launch delay.

The current native catalog has **58 available tricks and six empty input slots**. All 64 were exercised using the installed virtual controller; every available trick has a confirmed native fire and an observed animation ending. The library includes 58 fitted neutral-relative posture tracks, with 52 observed humanoid bone channels and three explicitly unavailable channels, plus independently exposed bike-part and IK curves. Actual body states remain force-integrated; joint command tracks supply actuation, not replacement body trajectories.

Use `PosturePlayer` or `sample_posture` for a timeline, and `sample_posture_phase` for observed enter/hold/recovery boundaries. `PoseSample` provides named holder-local positions and quaternions; `posture_to_world` transforms them into your own presentation holder. Retarget onto your own skeleton. `vehicle_pose.hpp` exposes measured part origins relative to Chassis COM; `ik_track.hpp` preserves literal IK weights and discrete target selectors. An IK weight alone is not proof of grip/pedal contact. Missing channels remain unavailable. Short observed holds do not establish an entire repeating loop.

The demo draws an independent capsule rider from 15 landmarks. Its recorder exposes the 52 available posture channels for integration and analysis; it does not draw a separate mesh bone for every exposed channel.

Trick scoring/catch policy remains independent: an accepted request, a completed animation, and a clean landed trick are separate events. Native landed scoring was not exposed by these observations. See the session02 coverage, provenance and calibration records for measured errors and remaining differences.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Windows demo is `build/bmx_demo.exe` with Ninja/MinGW, or `build/Release/bmx_demo.exe` with Visual Studio. The library and tests also build without Windows; the demo uses Windows OpenGL and XInput.

Use `-DBMX_BUILD_DEMO=OFF` for an engine/server build. No third-party package download is required.

## What is implemented

- A combined-body controller and an opt-in coupled bike/rider solver, two wheel rotors, normal contact impulses, lateral/longitudinal tire response, coupled braking, speed-dependent steering, balance, manual/nose-manual pitch, pumping, preload and one-shot hops.
- Air yaw/pitch control, quaternion orientation tracking, inertia-aware integration in the reference host, and independent landing/catch validation.
- All 58 named trick selections in the recovered catalog: barspins, tailwhips, crankflips, grabs, tables, inverts and compound selections. Body motion, contact release and bike articulation are exposed for presentation. Trick duration, pose, catch and scoring rules are adaptation tuning.
- World-space procedural rider joint targets, crank/pedal motion and handlebar/tail articulation. Retarget these to your own model.
- A separate observed-contact peg/rail sliding solver in `<bmx/grind.hpp>`. Your engine identifies the real rail contact; this solver does not search for rails or pull the bike toward them.
- A small flat-plane reference host and a 3D demo for testing the module without another game.

The reference hosts have no obstacle collision, full character locomotion, ragdoll, rail detection or engine animation retargeter. `CoupledWorld` includes the two-body suspension joint; `FlatWorld` keeps the earlier combined-body approximation. Your engine supplies its collision world and avatar. Advanced grind classifications and exact original-game physics/animation parity remain unverified.

When using the v2 coupled profile, pass its gravity and fixed step explicitly to `solve_grind(body, contact, profile.movement.tuning.gravity, profile.movement.simulation.seconds)` too; the rail helper's defaults remain the legacy gravity/60 Hz values. For the older v1 `ObservedProfile`, use `profile.tuning.gravity` and `profile.simulation.seconds`.

## Plug it into an engine

```cmake
add_subdirectory(path/to/bmx-movement)
target_link_libraries(your_game PRIVATE bmx::movement)
```

Run the controller once per `movement.fixed_seconds()`, with fresh measured body state and fresh queries for both wheels. The default remains 1/60 s; configurable fixed steps from 1/240 to 1/30 s are supported:

```cpp
#include <bmx/movement.hpp>

bmx::Movement movement;

// Inside your fixed physics tick:
// bmx::Body body = ... measured COM, normalized orientation, velocities,
//                         mass and principal body-space inertia;
// std::array<bmx::ContactSample, 2> contacts = ... front, then rear;
// bmx::RawInput input = ... your input system;
const auto frame = movement.step(input, body, contacts);
if (frame.valid) {
    // host.addForce(frame.force);       // WORLD space, applied at COM
    // host.addTorque(frame.torque);     // WORLD space, applied for this tick
    // host.addImpulse(frame.hop_impulse); // this frame's part, not multiplied by dt
    // animator.retarget(frame.rider);
    // trickUI.consume(frame.events);
}
```

`Movement` does not integrate the host body. Its `frame.body` is the supplied observation. **The host applies gravity exactly once**: returned forces exclude gravity. Do not also integrate `FlatWorld`, add `force*dt` and the same impulse twice, or feed render-frame timing into the fixed-step controller.

Use SI units: meters, seconds, kilograms, Newtons and Newton-meters. Coordinates follow the bundled bike geometry: **+X rider-left, +Y up, +Z forward**. Quaternions are Hamilton XYZW, rotating body vectors into world space. Input stick +X means rider-right and +Y means stick-up; convert your engine axes at the boundary. Rider arrays use `[left, right]`.

For each wheel query, supply `hit`, `clear`, or `unavailable`. A hit needs a world contact point, unit outward normal, **unsteered** bike-forward direction, surface velocity and signed wheel/surface separation. Positive separation means a gap. Unavailable queries reject the step rather than pretending there is a floor. The reference host is an infinite static plane; it does not establish general collision-engine correctness.

`SimulationSettings::swept_plane_normals` defaults to false and is enabled by `streets_observed_v2()`. It can anticipate a closing normal impulse for a current `clear` query tagged `ContactSample::verified_plane`, with zero surface velocity, using this tick's predicted body motion. Supply a current world point, unit normal and signed separation for that clear query. Set the tag only for an exact static plane; a triangle normal or previous hit alone is insufficient. For a bounded planar face, the host must verify the predicted contact stays within that same feature. Otherwise leave the tag false or disable the option.

This independent numerical aid applies only when the gap exceeds `max(0.03, contact_gap)` and is at most 0.25 m, and predicted normal motion crosses it within the fixed step. It supplies no friction or support status and leaves the strict support test unchanged. `Output::swept_plane_normal_mask` reports admitted rows; `swept_plane_normal_impulse` reports the two actual applied impulses. Those impulses are already included in returned force/torque. Curved surfaces, moving surfaces and general collision detection require your engine's own handling.

Reset `Movement` on respawn or body replacement, or supply a changed nonzero fourth `body_identity` argument to `step`. Scheduled hop parts are canceled on invalid observations and input/identity interruptions. Available input must be neutral to arm; focus loss, open menus, unavailable input or a different `device` identifier require neutral rearming. Call the physics controller while input is disabled so passive motion can continue. Gate a paused simulation by omitting physics ticks.

`frame.actions` describes the current input request. The four basic barspin/whip selections use `tricks.bar_*` and `tricks.tail_*`; the other catalog selections use `tricks.movement_*`. The presentation helpers combine those channels. A request is not a landed trick: consume `frame.events.combo_landed` and its counts only after confirmed landing/catching.

For a no-engine smoke test:

```cpp
bmx::FlatWorld world;
bmx::RawInput input;
input.available = input.focused = true;
input.device = 1;
world.step(input); // neutral arms the controller
input.pedal = 1;
for (int i = 0; i < 180; ++i) world.step(input);
const auto& frame = world.output();
```

The measured profile is opt-in:

```cpp
auto profile = bmx::streets_observed_v1();
bmx::FlatWorld world(profile.tuning, profile.simulation, profile.rider);
// Or use the same three arguments with bmx::Movement in your physics engine.
```

This profile uses the observed 100 Hz clock, gravity −12.5 m/s², 0.26 m wheel radii, 18-degree steering limit and measured rider dimensions/neutral template. Its delayed four-step hop is a reduced-body adaptation of the observed staging. Effective launch magnitude and speed-dependent brake scaling are empirically fitted; they are not recovered game equations. Its 9 cm contact-query horizon and 85,000 N response budget let this reference host anticipate a fast landing within a fixed step. They do not change the strict 5 mm support test and are not Unity's native contact offset or force limit. Your engine still supplies collision queries and continuous collision handling. Combined mass/inertia, suspension, tire equations, detailed rider/trick trajectories, catches and scoring belong to the independent model. See [COMPARISON.md](COMPARISON.md) and `reference/current-hop-observations.md` for measured results and remaining differences.

Run `bmx_demo --streets-observed-v1 --camera-behind` for this profile. Put the profile switch before numeric overrides such as `--hop-speed`, `--gravity` or `--pedal-torque`. `--step-seconds` configures the fixed clock; `--mass` configures the reference host. The behind camera helps visual comparison but is not a recovered Streets camera.

## Demo controls

| Action | Controller | Keyboard |
| --- | --- | --- |
| Pedal / brake | A / B | E / Q |
| Steer / air pitch | Left stick | Arrow keys |
| Preload, flick-hop, manual / nose-manual | Right stick Y | K / I |
| Quick hop | R3 | Space |
| Trick direction | Right stick | I J K L |
| Shoulder modifiers | LB / RB | Z / X |
| Trigger modifiers | LT / RT | C / V |
| Reset demo | D-pad Down | R |

For example, while airborne and after centering RS: LB + RS down selects Barspin Left; RB + RS down selects Barspin Right; LT+RT + RS right/left selects Tailwhip Right/Left. Return the stick to neutral to catch/rearm. All eight modifier sets and 58 named directions are in `include/bmx/detail/source/bmx_trick_catalog.h`.

`bmx_demo --scripted --seconds 12 --csv telemetry.csv` runs an explicitly labeled scripted input replay. A normal demo session reads XInput and falls back to keyboard. The separate `tools/drive_demo.py` can use this workspace's already-installed virtual controller for a bounded rehearsal; it does not install a driver.

## Recording and provenance

For a capture of only the demo's own rendering, run `bmx_demo --seconds 14 --frames new-frame-directory --csv telemetry.csv`. The frame directory must be new and its parent must exist. The demo exports its actual OpenGL framebuffer every 100 ms, a timing CSV and completion summary; it captures no other application. `tools/encode_framebuffer.py new-frame-directory --gif` creates an AVI, optional GIF preview and validation audit. This is live demo rendering, with measured frame times and repeated frames on the encoding grid; it is not a native 60-fps capture.

An optional second path, `tools/record_demo.mjs`, captures the exact demo window through Computer Use when app approval is available. `tools/encode_recording.py` encodes those sampled window frames. The earlier workspace `controller-run01` video uses **demo-owned framebuffer export** and remains in the 0.2.0 archive. Session02 uses game/demo-owned framebuffer export; recordings and controller receipts are kept together.

The comparison uses identical requested timelines from `tools/drive_target.py`, actual XInput readback and verified neutral/disconnect cleanup. `tools/comparison.py` aligns telemetry and composes actual framebuffer frames on a shared input timeline. The reference game can advance simulation time more slowly than wall time during recording, so both clocks are retained. Source frame gaps are held in the video; no intermediate rider motion is generated. Session02 expands native coverage to all 64 input slots and fresh suspension/balance/air probes. The earlier session01 braking report excludes a terrain drop and preserves its separate level-ground reference.

See [REFERENCE.md](REFERENCE.md), [COMPARISON.md](COMPARISON.md) and the reports under `recordings/comparison-session02` for current binary identity, measurements and remaining gaps. Raw game binaries and incomplete analysis carriers are excluded from the distributable package. The temporary observer was removed and the user's CatMod restored with its original SHA-256 after capture; each session's `deployment.json` records its restoration.

This package reuses independently authored ReSkate BMX source; see [LICENSE](LICENSE) and [source-manifest.json](source-manifest.json). The included license is GPL-3.0; review its terms when integrating or redistributing this code.
