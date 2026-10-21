# Current rider, suspension and trick playback evidence

This pass uses the installed current build, not the older source labels or default serialized values. Pinned GameAssembly SHA256 is `22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8`; metadata SHA256 is `7e1af5100b90a78c907999239bc6952dfdce2461fc4321626658f24f2815d7ea`. Both snapshot hashes were verified before analysis.

Static analysis used exact selected original method pages and original data in never-execute carriers. No original game bytes were modified or executed for these analyses. The observation-only V3 recorder was separately loaded by the normal authorized game launch and did not set forces, poses, inputs, animation state or preferences.

## Runtime pilot

`recordings/comparison-session02/streets-suspension01` completed at 2026-10-21T15:42:15Z with 176 game framebuffer images, 363 bone samples, 1,601 chassis telemetry rows and **1,601 body/joint/animation rows with consecutive fixed indices 0–1600**. Observer errors: zero. Simulation time advanced 16.00 seconds over 20.0556 wall seconds; this pacing difference must be retained when aligning externally timed controller gestures. Pose samples are limited by actual rendered callback frequency even when the requested rate is 30 Hz.

The compact, hash-linked `pilot-observations.json` in that capture folder preserves authoritative configuration and parameter ranges. Raw body observations remain in `body-physics.jsonl` and raw playback observations in `trick-states.jsonl`.

All eight selected bodies are present, dynamic, and have useGravity=true: Spring80 kg, Chassis10 kg, two wheels8 kg each, Headset/BB/two pedals1 kg each. These total110 kg. The other associated articulated character bodies are not thereby proved to contribute an equivalent rigid mass.

The active IManagedRigidbody object's concrete ManagedRigidbodySystem list was successfully observed as:

1. Chassis Body
2. Spring Body
3. Headset
4. Back Wheel Back
5. Front Wheel Front
6. BB Body
7. Right Pedal Body
8. Left Pedal Body

Previous native evidence for ManagedRigidbodySystem.AddForce shows an explicit list[0].AddForce, then a GetEnumerator/MoveNext loop applying the same vector/mode to the list. Metadata maps `0x1b0fe20` to List.get_Item and `0x1b04330` to List.GetEnumerator. The first chassis therefore appears to receive an extra application; exact generic enumerator behavior and applied per-body force counts were not instrumented. Treat the duplicate as a static derivation, not an observed force count. Four-quarter hop scheduling remains established separately in `current-hop-observations.md`.

The runtime inventory now exports the actual numeric JumpVelocity array [3.2,3.2,4.5]. During this pilot `_lastJumpForce` changed from zero to **(0,6.400000095,0)**, directly corroborating GetVel(1)=3.2 times the live quick-hop multiplier2. This is the native stored vector; the portable single-body launch fit is a different parameter.

## Spring joint: effective configuration

The selected Spring Body joint connects directly to Chassis Body. Both local anchors are (0,0.35,0). The joint uses local coordinates, axis(1,0,0), secondaryAxis(0,1,0), swapBodies=false, autoConfigureConnectedAnchor=false, massScale=connectedMassScale=1, enableCollision=false and enablePreprocessing=false. Linear X/Y/Z motions are Limited; angular X/Y/Z motions are Free. RotationDriveMode is Slerp, projectionMode None.

**All Spring drives use acceleration mode**, including Y. Initial measured drives:

| Drive | Position spring | Position damper | Maximum force | useAcceleration |
|---|---:|---:|---:|---|
| X | 100000 | 10000 | float max | true |
| Y | 1100 | approximately140 | float max | true |
| Z | 10000 | 1500 | float max | true |
| Angular X/YZ | 6000 | 600 | float max | true |
| Slerp | 6000 | approximately600 | float max | true |

A force implementation must account for acceleration-drive effective mass/inertia; these numerical spring values are not established as raw N/m gains. Unity reported currentForce/currentTorque as zero in the initial sample; that does not establish zero actual physical reactions.

The linear limit changed from approximately0.3 to2.5; contactDistance=1 and bounciness=0. Its limit spring/damper are zero. During the pilot, Y spring stayed1100, Y damper ranged approximately140–264.93234, and mass scales stayed1. Spring inertia changed continuously from grounded(6,8,6) to air(150,150,150), with identity inertia rotation. Spring local COM is (0,0.8,0). Chassis inertia was(1,1,1).

Front/rear wheel Y drives use force mode (useAcceleration=false), respectively spring18000/11000 and damper500. Their native joint motion/projection configurations are recorded separately; do not substitute the Spring acceleration-drive policy for these wheel drives.

## Native spring and mass-scale consumer

SpringDynamicsBehaviour.RunVehicleTick (RVA0x8d3cf0) reads live MotorVehicleSettings.StabilizationSettings each tick. It selects the grounded or airborne linear limit and grounded/air Y spring. A body-up projected velocity exceeding the configured threshold selects the up-damper value. Grounded desired Y/slerp dampers decrease by dt*100, clamped to their configured minima. During falling air motion (world velocity.y<0), desired Y damper rises by dt*200 up to the configured falling cap. While airborne, X, Y and Z joint.targetPosition components are checked in that order. Each magnitude above the normal air limit replaces both current and desired limit with abs(component)+0.5; the last qualifying component wins, rather than a vector norm or maximum. The native getter RVA0x4d697a0 maps to ConfigurableJoint.get_targetPosition in the matched metadata; it is separate from Joint.currentForce.

Current limit and Y/slerp dampers interpolate toward wanted values with alpha=clamp01(dt*DriverSpringLerpDampSpeed), live speed30. The method writes linearLimit, yDrive and slerpDrive. Only the cached slerp drive explicitly receives set_useAcceleration(true) in this tick; Y's acceleration flag is inherited from the authored joint and was verified live as true. Start copies the existing yDrive, linearLimit and slerpDrive; ResetSystem restores the grounded limit/down-damper working state.

VehicleController.FixedUpdate separately interpolates the joint's body/connected mass scales toward DriverConnection grounded/air settings with alpha=clamp01(dt*6). It also adds world-down acceleration using **legacy `_userGravitySmartData.Value` at offset0x540** to both Chassis and DriverSeat bodies. This is distinct from Physics.gravity, each body's useGravity, and `_userGravitySmartDataNew` at offset0x548. Both user values were zero in the earlier/current inventory, so this additional path was zero in those observations.

Static evidence IDs:

- Spring tick `ev_e8a25c69d0bf571f4b460027af64b4a0bf70431ed7e1b2b4e9dc5c785255896b`
- Spring initialization `ev_12d32e32557bf9387392fe0d1503c0d4fc536140403076b781364dd1e8bdccb0`
- Spring reset `ev_906fd4e4689683f3dafb5e9938c14150599a6b960117c556d59ac154a9a3291b`
- Vehicle fixed update `ev_afac84e6b4be4c76854a1bf0668df092eb55719d2ebcb3ae374eaa419578985f`
- Runtime inertia initialization `ev_497c656ffd6f5e26349223cc20717c76355b6c1bec8b9201edbb2f9a19b39090`

## Skeleton binding and IK coverage

The 55-slot humanoid observation has52 mapped transforms and3 null slots. Its nearest Rigidbody ancestor is **Animated Skeleton**, not the separate Spring Body. V3 reports that body's COM/orientation as pose_reference with explicit ancestor binding, plus all ancestor candidates and a separately marked unbound Spring candidate. In the pilot their transform origins were close while their COMs differed by about0.818 m, largely because Spring's local COM is0.8 m above its transform origin.

Use the emitted skeleton reference for pose-local trajectories, then subtract the measured neutral pose and reflect X once for the portable module's left-positive basis. Do not silently use chassis COM or the unbound Spring COM as the skeleton frame.

IKTargetManager and FullBodyMotion helper-weight/target-ID transform discovery remained unavailable in this pilot. Those fields are null, not inferred zero release weights. The emitted bone positions/rotations remain valid. Additional helper binding requires a verified live component relationship before a subsequent observer revision.

## Actual catalog and playback

V3 exported64 explicit positions: **58 available definitions and6 empty slots**, in all three source-role tables. The bound vehicle provider resolves slots through effective MotorVehicleSettings.TrickDataSets; native VehicleController.ResolveVehicleTrick directly validates set/slot bounds and indexes this table. This establishes canonical vehicle-provider availability for the observed local vehicle. Keep separate set and slot IDs.

The export includes actual SyncTrickAnimationData object identities, TrickName/Points, phase and mirrored player/vehicle clip identities/durations/frame rates, speed multipliers, air-only and hold-through-landing flags, controller fade/loop/hold policies, and animator settings. Some clip names contain Scooter although referenced by the active BMX table; names are preserved without assigning another runtime vehicle role.

TrickAnimator.ApplySpeed sets both player and vehicle states to `MasterSpeed * Sync.OverallSpeedMult * phaseMultiplier`. MasterSpeed is Character.SlotTrickSpeedMultiplier when PlayerProgressionSettings.IsEnabled, otherwise Character.MasterSpeedMult; it is not their product. Bound per-slot multiplier is clamped0.5–2 by the brain. At observed MasterSpeed1, Grand Slam Left's2.5-second enter clip with overall4.8 and enter1 has nominal enter duration0.5208 simulation seconds, before interruption/blending.

TrickControllerV2.Fire only accepts while phase Idle, with a valid data/enter clip and a changed data identity; it captures fired set/slot, resets flags, sets loopCount1, raises the layer and starts Enter/Launch. Hold caps relevant normalized times to0.9995. Optional auto/requested Tweak begins near TweakBeginBlendNormalizedTime-0.03. GoToExit guards against repeated exit, selects held-pose or loop-exit fade/start overrides and changes phase to Exit. These animation phases are not scoring success by themselves.

The suspension pilot intentionally fired no trick: controller stayed Idle, so populated live playback state during a fired trick still needs matrix/pilot validation. No animation completion is inferred from catalog presence alone.

Evidence: brain resolver `ev_9fcdc8c986e04cb3246e5fdb9e6578a63b237c6a1077efd84c9e82ba5de4a941`; vehicle resolver `ev_28460d612d6713cef22ccb661bc6759a9110e7435942ef44d9fc64a2d72bbbbc`; slot-speed clamp `ev_37e4477905b5a7fc04b24d521e1d19f05eb456bc2fa136082edb82e8cf39d10b`; ApplySpeed `ev_df6e11a41007feae27355a392714293cf9a8c84852f32c4fb7aac2ff6aaf6231`; MasterSpeed `ev_7b6d3b05e53e813bd50d3a5aa5a40e621f62d848e7d0d35f0fd396e6f22b963c`; Fire `ev_3b046cca4a0bc198a6d006f38c4c097519f67f25094be697faedebb7d39f2314`; Hold/Update `ev_89003eda6f52553531e5fe3d59fbd0a37c895b3ab9edd860925613345bd18956`; Exit `ev_4d61718f432202da43a605846abc46df7471c6470bde1ec005477f8e726af3d9`.

Full retained bundles: `build/bmx-movement-current/rea-rider-suspension-evidence.json` (13,871,285 bytes,11 records) and `rea-rider-animation-evidence.json` (5,086,857 bytes,8 records). Pseudocode and exact-byte carrier manifests are adjacent. Both native sessions were closed after export.


## Active playback validation and bail coverage

Session02 `streets-matrix01` recorded six actual set0 fires before a bail: Grand Slam Left, Twix Right, Helicopter Right, Bri Flip Left, Willy Flip and Mc Griddle Left. The V3 Dual animation export contains concrete player/vehicle Animancer native identities, clip IDs, time, normalized time, live effective speed and effective weight. For Grand Slam Left the authored Enter clip is2.5s and observed playback speed4.8; Launch began atsimulation384.58 and Exit at385.11 with player normalized time1.0176. This supports an effective Enter duration near.521simulationseconds, not a2.5second required airborne hold. A controller/phase reset is not evidence of a successful scored landing. `pilot-trick-transitions.json` records concise observed transitions.

The V3 pilot ended with571frames and1307pose samples but only1842fixed observations through20.4324wallseconds;3831errors subsequently accumulated because the old recorder counted missing local vehicle as errors and returned without rows. This was a recorder coverage failure during a bail, not evidence of engine simulation stopping. V3.1 preserves explicit invalid fixed rows, separates source unavailability from exceptions, guards independent channels and reacquires scoped references after respawn. Its source/build SHA256 is4209bd3fa89dab5837bd7b47c3b9d703c0a2f55c2c42a6ffbca58e9d33f7ad3c; deployment and new runtime validation are owned by the parent capture task.


## Confirmed input gate, jump recipients and runtime inertia consumer

The current native PlayerInputBehaviour.StageInputData (evidence ev_911f5f2a7b0624457134228f0b8ccb97985d55ee1a34dc95d365472d7c0431e1) suppresses right-stick preload/manual/nosey/trick inputs while its stationary camera orbit gate is active. It requires the same orbit vehicle, a nonnegative orbit start at least3seconds earlier, or the sticky _stationaryOrbitStickNeedsRelease flag. The sticky flag clears when trick-stick magnitude squared is below.01. get_CanStartStationaryVehicleOrbit (evidence ev_8ee8916ac69da88471d32e55235f2a916dfebec41a9df9a7a13c4c00a84b2523) checks local player/replay mode, a live character/vehicle, vehicle _stationary, no remote-control action, gameplay/session/UI gates. The getter does not expose a numerical speed threshold. Ground RS while idling is therefore not a valid unrestricted preload experiment; moving or immediate post-respawn inputs must be measured separately.

ManagedRigidbodySystem.AddForce (evidence ev_3a4753c2c1250dd508e058e54f09b060e16573755580cec8a7113db1ef94e167) first calls list index0 and applies the supplied force/mode, then constructs a fresh list enumerator and applies the same force/mode to each yielded body. The exact constructor (evidence ev_7683bc1a2525440aeaa9f752341e4a7b8e7a9b151866dab7f32e06b5409350ab) sets index0. The exact shared MoveNext callee (evidence ev_4899d54a88136fd8ac9ca4d7cc126f1f0f9bb8b885c48ab28728b480364711b7) fetches array[index] then increments. Combined with the live eight-body list beginning Chassis, this confirms Chassis receives each managed hop velocity change twice and each other listed body once. A reduced30kg base containing10kg chassis consequently has momentum-equivalent scale4/3, while80kg Spring has scale1. This does not model all individual constraints.

VehicleController.HandleStabilization (evidence ev_78f40062a3384ce243b1ef63817adae0397fca670a9cbdbf251706a2afb60a70) selects airborne treatment when _overrideNotGrounded, _timeInAir>0, or Time.time minus _timeAtLastJump is below.35. The Spring manual/nosey/ground inertia is assigned immediately in ground treatment (nosey magnitude>.5 has precedence); in airborne treatment the desired inertia is settings.DriverBodyInetiaTensor_Air. Its current inertia vector then moves toward desired by at most500*Time.fixedDeltaTime in vector norm. This is Vector3.MoveTowards behavior, not exponential interpolation or500per-axis. From(6,8,6) to(150,150,150), native100Hz predicts X/Z increments2.900115 and Y2.859836, matching recorded first steps. Auto tensor/kinematic branches and callback order apply. The full HandleStabilization decompilation reports a bad-instruction branch elsewhere; the inertia branch and its getter/setter mappings above are recovered, not a claim of full method closure.

Native bundles rea-rider-input-force-evidence.json and rea-rider-stabilization-evidence.json preserve complete records and exact-source carrier manifests. The carriers are never executed or packaged into the standalone module.

Reset matrix01b validates V3.1 across bails/respawns:9569fixed rows,9133valid,436explicit unavailable, no index gaps and zero exceptions; all16set0/1 slots fired at observed current local bindings. Captured rider pose references remain the actual skeleton-bound holder, separate from the Spring COM. Existing captures contain all8physical bike part transforms but do not explicitly contain the animated vehicle root or a renderer-to-body binding; that visual-articulation distinction remains open.


## Animated bike-root consumer and V3.2 channels

TuckUpTest.RunVehicleTick (evidence ev_d2fde784b7ae94b8e9ff4faad4c796738ed5623f2db609bcc85526b8cba8fe90) consumes actual root animation as spring actuation. With translation animation enabled, currentRootPosition is rootBikeJoint.localPosition plus BikeGrindPoser.RootPosition, additive position, and shralp position. Additive position is then cleared. Joint.anchor is ultimately assigned Vector3.up*.35; targetPosition is currentRootPosition minus anchor minus Vector3.up*YPos. YPos moves toward the anti-case desired offset at abs(Physics.gravity.y)*dt*.15. Normal desired anti-case offset iszero; activation depends on airborne/contact target state and weights. This consumer contains no autonomous direct left-stick/preload scalar curve: those inputs feed animation and the animation-root pose becomes actuation. Dead-state paths reset target and original root position.

SetJointTargetRotations (evidence ev_3270cf628036bb13e082f58f48388040ffc745ce3861314bbe73cc5f551c62bd) initially assigns animatedVehicleRootJoint.localRotation as the Spring target rotation, then combines current grind, shralp, additive and normal-correction rotations. The later drivetrain target paths and mirrored pose branches are retained in the bundle and are not asserted fully reconstructed here. Start (evidence ev_92856dccad9b6dd5d0e8c4969354cef27622af3175c82203dc989b3e02aad5c3) caches the exact joint and initial root local pose. The complete four-method bundle is rea-rider-joint-target-evidence.json.

V3.2 adds both actual root/target roles, renderer and skin-bone binding inventory, four actual UnityIKLimb channels, IKTarget reference weights and current/next target identities. FullBodyMotion discovery includes global inactive objects and requires exact Animator/Animancer.Animator/Hips binding; no nearby-object assumption is used. FullBodyMotion candidate counts and enabled/active status remain explicit even if the mechanism is absent. The original V3.1 source remained untouched while the game ran; V3.2 was built in an isolated stage then promoted after the parent verified the game exited. Final223744byte DLL SHA256996100d894be754a2be9986201d5c301b888759edbb3a52a872270a244eb0792. Runtime validation is pending source playback in streets-visual-matrix01.

WheelPhysics.get_NominalContactPoint (evidence ev_40f8310c3016695511be0da0f49f911229099f47ec8729667104d6a455f93102) uses assigned wheelBaseTrans.position directly; otherwise it uses wheel-collider transform position minus radius*transform.up, with component-transform fallback. NominalContactDown (evidence ev_1d3f05557e642719fdebed946c94da501e48c489dc13cf11167fb481873adcc4) is negative collider/component transform.up. RuntimeWheelCasterRay.Tick (evidence ev_3b31b52b96c421d671b56d1c5757fdd810990aea9429222bc58751ca04505a88) builds ray origin from Rigidbody.position plus body.rotation*localPosition. These consumers use transform/body-position coordinates, not a universal wheel COM origin. Native contact geometry and base/collider offsets must be observed; matching wheel COM height to radius alone does not establish ray origin. V3.2 records actual bounds and base/contact references for this distinction. Native wheel evidence bundle is rea-wheel-origin-evidence.json.


The Spring SLERP damper policy does not increase its wanted value while falling. Grounding decreases wanted SLERP by100*dt with a minimum equal to the configured600. Falling only caps it at600*10. With pilot initialization600 this remains600. Wanted Y damper is reset to the configured up value when body-up projected velocity is strictly above1.5; the falling world-Y branch then increases it by200*dt up to600. Current drive values lerp with clamp01(dt*30). These are separate stateful operations; pilot up/down values both140 make the directional reset numerically equal.

## V3.2 visual matrix validation and startup limitation

`streets-visual-matrix01` completed 290.0438 wall seconds with 2536 actual Unity framebuffer images and 5057 rider pose samples. Its startup marker was opened while the external writer still held it; V3.2 set the started state before the failed read and never opened the body-physics or fixed trick-state writers. Consequently this run is valid only for its recorded video, rider/IK/animation/control-root pose streams. The summary reports physics_samples=0 and channel write errors; it is not fixed-physics coverage. The detailed receipt is `capture-channel-limitation.json` in that run. Existing V3.1 reset matrices supply the separate fixed-physics evidence.

The visual run verifies an active FullBodyMotion component by exact humanoid Animator/Hips binding (one match among three global candidates), all four UnityIKLimbs by exact Animator binding, and actual per-limb weights and target identities. Both Tuck `_animatedVehicleRootJoint` and `_rootBikeJoint` refer to the same Joints transform in this vehicle. Their Chassis-relative poses and the BB/left/right-pedal animation targets are valid after the current body binding becomes available. These are control-animation targets, not proof that visible bike renderers descend from them.

The initial actual renderer inventory contains 81 entries, including 54 with no selected physics-body ancestor. None descends from the Tuck animated Joints transform. Direct ancestry alone therefore cannot reconstruct all visible frame, bars, rear-wheel and drivetrain transforms. Current metadata exposes EquipSlotVehicle's ParentConstraint configuration methods; V3.3 records actual renderer ancestor ParentConstraints and their source weights/offsets to establish the missing bridge, plus actual renderer/ancestor poses for verification.

V3.3 startup reads and parses the entire marker before changing the started state, retries transient IO/partial-JSON reads for at most two seconds, and opens all six essential channel writers atomically. A failed transaction disposes its writers and removes only files it created. Successful capture-start records all_channels_opened; rejection emits capture-start-failure. This is paired with the controller tool's atomic marker publication. Staged and promoted DLL SHA256 is `221f513aed4945df26b21937acf9f3653005f4f4f1d828f5cd7ffdc5bf65bd06` (243200 bytes). Promotion occurred only after the parent verified game exit; new short runtime validation is still required.

Static EquipSlotVehicle.ConfigureSingleSourceParentConstraint (evidence `ev_ad0e2c78de609a483d4467240f589f73bb3831f5e72d835b7b144a6501f9b97f`) disables active/locked, removes sources after index0, creates or replaces source0 with the supplied transform and source weight1, optionally sets rotation offset, then sets constraint weight1, locks and activates. A missing source removes source0. The helper does not establish that all offsets are zero. ConfigureBackWheelVisualsConstraint (`ev_19a06c66fdacd59a156b32b7408265fea8dea88ab5ef6fd4e5405ebaa038336a`) likewise supplies one actual source with weight1 and calls a further rear offset consumer; its complete offset semantics remain runtime-observed.

ConfigureForkParentConstraint (`ev_ca895a41f6962c68711e3680518117be8617464eb0cf22fb5db16bddb33690e1`) supplies `_headSetConstraintTrans` as its sole weight1 source, uses rotation-axis mask5 (X|Z) and translation-axis mask2 (Y), then computes a Y-only translation offset and zero rotation offset before activation. This establishes a constrained visual bridge separate from direct Rigidbody ancestry. Exact actual source identities, hierarchy, per-source offsets and resulting visible poses must still be read from V3.3. The four-method bundle is `rea-visual-constraint-evidence.json`; exact-code carrier SHA256 `68f321e8df2424094bf388104e4ff09549c67f26209709ed45446fcfd7f42b38` retains only selected original code pages and original data. It was never executed, and the native session was closed after export.

## V3.3 complete controls capture and visual bridge

`streets-controls-v33` validates transactional startup with all six channels opened, one complete marker read, 2532 valid fixed rows with no index gaps, 579 pose samples, 295 framebuffer images and zero errors. The detailed derived receipt is `build/bmx-movement-current/observer-v33-validation-controls.json`.

Actual ParentConstraints bind the seven visible bike roots to Chassis, Headset, front/rear wheels, BB and left/right pedal physics transforms. All observed sources and constraint weights are1 and offsets zero. Six roots match their source transforms within4.3 micrometers and0.00011 degrees throughout all579 sampled poses. Rear-wheel constraint activation changes in the capture; its overall maximum discrepancy is0.013m/0.191degrees, so an always-active rear-wheel assumption is broader than the recording. All seven renderer entries without direct-body or constraint binding are avatar/marker renderers, not unbound visible bike parts. This closes the principal physical-part-to-visible-bike mapping using recorded source transforms; Tuck animation control targets retain their separate role.

Live wheel radius is approximately0.2686582m and width0.0789453m for both wheels, distinct from the earlier0.26m authored setting. Each physical wheel uses a convex MeshCollider with contactOffset0.001m. Front collider center is approximately(0,0,0.0377248) in its Rigidbody transform frame; rear center is(0,0,0). COM includes an additional approximately0.0085m Y offset and is not the geometric center. The front/rear WheelBase positions in Rigidbody coordinates are approximately(0,-0.268641,0.037725) and(0,-0.268658,0); native NominalContactPoint equals WheelBase.position exactly in the checked row. Captured coordinates and actual geometry supply the bridge; no mesh vertices are exported.

## External upright torque: separate balance PID

The joint alone is not the complete rider orientation model. Native BMXBalanaceBehaviour.Tick directly calls VehicleBalancePID.RunPID in its recovered ordinary air, wheel-contact and peg-contact branches; it does not call the older TickBalancePID method. Caller evidence `ev_f32cc7f447092294957f6ce77fcb94796d5fd0a278a71a0b6d083c974092e849` is retained in `rea-balance-caller-evidence.json` and its complete pseudocode file. Callback ordering and live private PID multipliers were not instrumented.

RunPID (`ev_9f3d8f2af98da62f7973e24dbb70003bdee22309d82a469c3d9a8e0950d477e4`) updates separate human/vehicle PIDBody objects. Authored exact-roundtrip objects in both BMX roots bind human PIDBody to Spring and vehicle PIDBody to Chassis. It calls vehicle Tick(dt,vehicleForceMult,max(xPIDMult,xPIDMultRevert),mult), followed by human Tick(dt,1,max(previousPitchMult,xPIDMultHuman),mult). The hard-coded human force multiplier is1. The original constructor bytes initialize vehicleForceMult to1; later runtime overrides of that private value were not exported.

In the ordinary no-wheel/no-peg airborne branch the caller sets overall mult=max(0.02,clamp01(1-abs(flipInput)+0.25)). It overrides xPIDMult=(1-abs(flipInput))*0.2*flipStabilizationMult*userXMult. Authored userXMult=1 and a fresh ordinary hop with neutral flip/undisturbed stabilization state therefore gives overall1 and pitch0.2, before RunPID's maxima with human/revert multipliers. This overrides the earlier HandleStabilization air assignment of1. The peg-contact branch's pitch-disable conditions are not general airborne behavior. Flip-entry recovery and landing-normal evolution remain stateful branches in the complete caller record.

PIDBody.Tick (`ev_9d83b419c2730dd1407dfbc564d12e9df66ce978ccd4bb5f36f79caffa96b6eb`) calculates signed pitch/roll projections against its interpolated normal. The angle-axis call at RVA0x4cdf7f0 uses zero angle and body-right axis; it is not Quaternion.Inverse. It calls scalar PID with massFactor=1 grounded or10 airborne, and pMassFactorMult=false grounded/true airborne. Roll PID setpoint0 is negated into local Z torque. Pitch PID setpoint is mannyAngle+animatedXPIDAngle; its result is doubled and multiplied by pitch control, with an80-degree eligibility condition. Final local torque multiplies forceMult, forceMultExtra, overall mult, and an additional factor2 airborne. It applies Rigidbody.AddRelativeTorque(Vector3), the default force-mode overload, to that body's Rigidbody. This is an external stabilizing torque, not an equal-and-opposite internal Spring joint reaction.

PIDController.Update (`ev_823f848ec7a3a62a1d314625fcbe781aa1e9ef528e63c437397b26c55228cad0`) computes error=setpoint-actual, integral+=error*dt, derivative=(error-lastError)/dt. The raw sum is P*error*(pMassFactorMult?massFactor:1)+I*integral+D*derivative*massFactor; it clamps the raw sum to ±maxOutput, then multiplies outputMult. Authored nested PID settings are P0.1/I0/D0.01/maxOutput8. RunPID replaces outputMult with effective StabilizationSettings getters: grounded Z720/X480.00003 and airborne Z360/X360 in suspension01. Authored raw getter backing fields are larger and must not be substituted. Other live settings include normal lerps6, human-up blend0.2, vehicle-up blend0.1 and SystemMaxAngular15. The eight-method complete bundle is `rea-upright-pid-evidence.json`; both native sessions were exported and closed, and no target code or state was modified by this analysis.
