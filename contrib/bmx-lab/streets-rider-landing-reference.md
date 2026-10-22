# Streets rider, controls and landing reference

Read-only investigation of the supplied `D:\SteamLibrary\steamapps\common\BMX Streets`
install, October 18, 2026. This records reference behavior for an independent adaptation;
it does not establish equivalent handling in ReSkate. Neither game was executed for this
investigation, and installed files were not changed.

## Evidence and identity

The original `GameAssembly.dll` SHA-256 is
`3d304228003aeeb7e96ee7588ad92526d67eadd3c5db530f7bc0d661399d5ef1`.
The existing matched Cpp2IL metadata identifies Unity 2022.3.62f2 / metadata 31.1.
Its generated C# files supply method identities, signatures and field offsets, **not
original C# bodies**. RVA below means original image RVA; preferred VA is
`0x180000000 + RVA`.

To avoid repeating the previously timed-out whole-image analysis, the local
[generator](../../build/bmx-streets-controls/make_rider_input_slice.py) retained 64 KiB
of exact selected original code pages, original data and 20 metadata-derived entry aliases.
The [manifest](../../build/bmx-streets-controls/rider-input-native-slice-manifest.json)
pins each method's original file offset, size and hash. Carrier SHA-256:
`11ac66f8b7511e6266f225a73954f917768673895a9804bca04653449588ecb9`.
This incomplete, analyst-derived carrier is **never to be executed, loaded or installed**.
Its modified headers, omitted code and synthetic aliases are documented in the manifest.

REA/Ghidra 12.1.4 supplied native pseudocode and assembly over those exact retained bytes.
The complete [15-record evidence bundle](../../build/bmx-streets-controls/rea-rider-input-native-evidence.json)
is saved locally; the REA session was closed. Ignored evidence files are available in this
workspace, not a clean checkout. Decompiler types and unresolved indirect calls remain
limitations, and static evidence does not observe the active runtime configuration.

## Authored controls versus current adaptation

The extracted InputSystem asset, metadata literal 47090,
[MashBox_Freerun_InputActions](../../build/bmx-streets-controls/input-actions-literal-47090.json),
authors these BMX bindings:

| Control | Streets action | ReSkate baseline before this control revision |
| --- | --- | --- |
| LS X | Steer | Steer; separate airborne barspin request |
| LS Y | Pump and Flip | Front-weight diagnostic; airborne pitch was on RS Y |
| RS Y | Preload, Flick Hop, Manny/Nosey, Manny Angle Control | Preload/flick-hop/manual; adapted airborne pitch |
| RS vector / X | Trick Stick, Grind Pose / Extra Lean Stick | Rear trick gestures / tailwhip request |
| A/Cross | Accelerate; Revert Activator | Pedal |
| B/Circle | Brake | Brake |
| R3 | Quick Hop | Quick hop |
| Y/Triangle | Dismount | Dismount to walking |
| RT minus LT | Extra Lean; Lateral Hop Direction | No pedal/brake binding |
| RB minus LB | Slider | No established corresponding feature |

The user explicitly requested keeping **Skate's D-pad mappings**. Streets' separate
General-map D-pad bindings (left drone, right emote, up marker, down respawn) are reference
evidence only and should not replace Skate's bindings. The multi-vehicle asset also contains
RT Throttle / LT Car Brake; their existence does not make RT the bicycle pedal control.

The asset has no explicit action/binding processors or interactions for these BMX entries.
That does not prove zero effective deadzone: InputSystem device defaults, rebinding and
consumers can still transform input. ReSkate's `.12` radial deadzone, `.18` neutral zone,
`.78` flick threshold, gesture timers and catch rules are independent tuning, not recovered
Streets settings. Keyboard E/Q is also a ReSkate fallback, not the authored Streets W binding.

## Native control consumers

**LS Y reaches the flip consumer.** `PlayerInputBehaviour.StageInputData`, RVA `0x881c10`,
reads `_steerInputAction` at object `+0x5c8` and `_flipInputAction` at `+0x5d0` into the
X/Y movement pair. An optional `_flumpInputAction` can override that pair. The stage packs
signed inputs to -127..127; `PerformInputs`, RVA `0x87b5a0`, unpacks with `1/127` and dispatches
the input interface. `VehicleController.Steer`, RVA `0x86bfb0`, stores the direction vector
and its Y component as `_flipInputRaw` at `+0x51c`. `HandleInputs`, RVA `0x864670`, evaluates
the configured response curve and smooths that value into `_flipInput` at `+0x38c`.
`HandleSpinSystem`, RVA `0x8654e0`, passes `_spinInput` / `_flipInput` to the spin subsystem.
This supports the LS Y Flip binding; it does not recover an interchangeable torque value,
Unity-to-Frostbite sign convention or proof that every vehicle selects the same override.

Relevant records:

- Stage: `ev_3581cc7ff89e6b09dd52adfb1ad3ca08c4b5ed407d019bd3c459b3c6f135cae3`.
- Dispatch: `ev_187db7c13af7f191f230b6aa53dc2a0cb7aaaf0db8ae43658d7722bf7b0bf966`.
- Steer: `ev_70f339e4be9734bfbe3ff63f5d9ad749eea25f792fdf686d8ebcf2c3ad35d74a`.
- Input shaping: `ev_56da1b96646ba8a9834be912ffa13b60a719c55250dd9af18a9be1cb80ee683a`.
- Spin handoff: `ev_af58fb08d0b25f87c12ff006fa326f72b7de94e13acf90aa38ffbbcce8f48119`.

**RS selects a direction inside a trick set.** `TrickSystemBrain.UpdateTrickInput`, RVA
`0x8b47f0`, and `GetStickDirection`, RVA `0x8b35d0`, use squared stick magnitude `.49`
(radius `.7`), a normalized angle and directional slots. Forward/back sectors are widened
by the explicit 30/150/210/330-degree branches; other sectors use a 45-degree quantization.
The selected identifier is `setIndex * 10 + directionSlot`. The targeted authored-list
extraction below resolves the supported barspin/whip identifiers. This is materially
different from labeling every fast LS lateral flick a barspin and every RS lateral flick
a tailwhip.

`ReadTrickMask`, RVA `0x87fa70`, first checks whether all eight configured TrickSet input
actions exist. If so, the first nonzero action in order 0..7 wins. Otherwise it uses the
raw Gamepad fallback at RVA `0x87f850`. Same-build `Gamepad` metadata confirms the shoulder
and trigger fields; this fallback uses values **greater than .2**, in this priority order:

| Combination | Set index |
| --- | --- |
| LB + RT | 0 |
| RB + LT | 1 |
| LB + RB | 2 |
| LB | 3 |
| RB | 4 |
| LT + RT | 5 |
| LT | 6 |
| RT | 7 |
| None | -1 |

Configured actions/rebinding can supersede that fallback, so it is not a universal live
mapping claim. `SetHopNullOut`, RVA `0x8b43e0`, can clear selected sets and latch a null-out
flag. `GetStickDirection` / `UpdateTrickInput` clear that flag when the stick is below `.7`;
`ZeroStickInput`, RVA `0x8b4cc0`, uses the same radius. This supports neutral rearming,
without establishing our timed flick/catch implementation as Streets behavior.

The same literal asset's separate **Tricking** map supplies all eight short-named authored
actions: `LS RS` (RB modifier + LB), `RS` (RB), `LS` (LB), `LT RT` (RT modifier + LT),
`RT`, `LT`, `LS RT` (RT modifier + LB), and `RS LT` (LT modifier + RB). Here the names
`LS`/`RS` refer to the shoulder bindings, not the stick axes. The matched
`TrickDataSets.TrickSetEnum` names the same priority/index combinations as the table above.
A search for literal names `TrickSet1..8` would miss these short-named actions.

## Four supported named selections

The bounded [typed extraction](../../build/bmx-streets-controls/trick-reference/typed-trick-reference.json)
accepts 136 selected components, set lists, referenced animation-data records and an input
asset, with zero errors. Every accepted object reproduces its original serialized bytes.
The [verification record](../../build/bmx-streets-controls/trick-reference/extraction-verification.json)
confirms all four installed inputs have unchanged hashes after extraction. No code ran in
either game. The [complete BMX slot map](../../build/bmx-streets-controls/trick-reference/bmx-named-trick-map.json)
retains object IDs/names, authored list order and null entries.

Both known `Proto_BMX` roots 8863/19476 have VehicleController objects 52961/58826 linked
to `MotorVehicleSettings_ProtoBMX` 46345. Its **TrickDataSets** reference is object 46147,
`BMX_TrickDataSets`. Its `_trickSets` order directly supplies these indices:

| Modifier and RS direction | Named entry | Set / slot / trick ID | Set / entry object |
| --- | --- | --- | --- |
| LB + RS down | Barspin Left | 3 / 4 / 34 | 46151 / 45974 |
| RB + RS down | Barspin Right | 4 / 4 / 44 | 46154 / 45977 |
| LT + RT + RS right | Whip Right | 5 / 2 / 52 | 46149 / 46115 |
| LT + RT + RS left | Whip Left | 5 / 6 / 56 | 46149 / 46112 |

Native cardinal argument/constant branches at RVA `0x6185c0..0x61880d`, called by the
retained direction selector, establish up/right/down/left angles 0/90/180/270 degrees and
slots 0/2/4/6. The verification record retains the caller instructions, exact helper bytes,
original file offsets and +/-pi, +/-pi/2 constants. The CPU-dispatch alternative math
kernel and the entire transcendental implementation were not reconstructed.

The `.7` radius applies to the vector presented to the selector after InputSystem reading
and signed-127 packing/unpacking. It is not proof of a `.7` hardware-stick threshold.
Applying it after ReSkate's existing radial deadzone is explicitly an adaptation.
ReSkate requires all controls, including modifiers, released after equip/reset/focus/source
loss. Once active, centering RS rearms another selection while a modifier remains held;
changing modifiers with RS still deflected cannot create a new request. A fresh airborne
RS neutral is required after takeoff. The authored
records above establish supported named requests, not identical animation duration,
catch windows, live override selection or complete behavior of `TrickSystemBrainV2`.

Records: selector `ev_b231108837b6473753dadcb3f780e4d66853eb91bd4ec7288a7422598f718666`;
direction `ev_6caef61c9a4ddfc06bf0defa826b1de7cb91af3350a5762a8152abfd8437688f`;
configured mask `ev_5e2a22310e05d05f42a068ff53534895ed9ac3faabe6634f2032d313659da003`;
fallback `ev_ccaf0f110afe6580739d97e864d6514deb339fad52680ede5fa18d99dd5ba8bd`;
null-out `ev_2d877cf835c1bdf2b04dd22f562e5fd7f916a082714ee6b33f2941ed6ce9908b`;
neutral `ev_17a1582f02826b5115e1299138595a780ebb06a219ebcef91307d6527c769710`.

## Rider contacts and landing recovery: established boundary

Existing [paired clip evidence](controls-and-reference.md#movement-cues-and-limits) provides
pedal/preload/hop motion, avatar paths and authored animator parameters. Those humanoid
tracks are not Skate bone quaternions or physical root trajectories.

Native `FullBodyMotionBehaviour.FixedUpdate`, RVA `0x8efd50`, smooths named elbow, knee,
spine and hip modifiers, writes them to `FullBodyMotionComponent`, and resets wanted
contributions after publishing. Several scalar smoothing factors are `clamp(dt*30,0,1)`;
elbow limits include +/-90 degrees. It is a modifier pipeline, not evidence of a complete
grip/pedal IK solution. Record:
`ev_c5dd04ad450ba9b08f0efbf27afa27b025cef1d8953c3fb857a06c36516999e2`.

`IKTargetManager` metadata separately declares four named bike anchors, four limb targets,
and per-limb position/rotation offsets. `UpdateTarget` is mapped to RVA `0x8301b0`, length
`0x272`; it was **not** included in this code slice, so local/world offset composition,
contact authority and late-animation ordering remain unresolved. This is the next narrow
contact-target consumer to inspect, alongside the actual selected prefab anchor references.

`VehicleController.OnLanded`, RVA `0x868c00`, only establishes a narrow automatic-pump rule
in this body: if `_timeInAir > .15` and both manual/nose inputs are <=.05, it sets
`_autoPumpTimeRemaining` to `.3`. This is not proof of stable landing, catch scoring,
impact acceptance or ragdoll recovery. Record:
`ev_1483c7891653ab5776a9fd3cb506a428bd948bb056f33acccf4e4492467f6a83`.
The actual `LandActivity.Land` entry is metadata-mapped to RVA `0x84d9d0`, length `0x60f`,
and remains unexamined. Its declared collision/trick dependencies identify a useful next
target, not a recovered recovery algorithm.

ReSkate should therefore keep measured contact/impact quality, 60 ms stable landing,
frozen first-touch scoring and native ownership guards independently validated. No
Streets recovery timing, hand/foot offsets or physics gains were copied from names alone.
