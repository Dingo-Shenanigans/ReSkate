# Current BMX Streets hop observations

Static source: current installed GameAssembly.dll, SHA256 `22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8`. Exact original method bytes were retained in never-execute analysis carriers, with native RVA/file-offset/token verification. The original game binary was not modified or executed for this static investigation.

## Request and timing

`VehicleController.Jump(Vector2)` at RVA `0x8f3d20` rejects requests during these cooldowns: 0.15 seconds since the last completed jump, 0.25 seconds since a jump began, and 0.35 seconds since teleport. A negative-Y request is rejected during Manny. The coroutine receives `quickHop = Approximately(direction.y, 1)` and `nollie = direction.y < 0 && !Nosey`.

`<EnumJump>d__486.MoveNext` at RVA `0x910540`, file offset `0x90eb40`, length `0xc7c`, token `0x60018AA` performs a staged hop:

1. Wait up to 0.05 seconds for CanJump, advancing the request timer by the fixed timestep and yielding WaitFixed. Abort if eligibility never arrives.
2. Record absolute ExtraLeanValue, conditionally clear nollie when ungrounded or any peg collides, override not-grounded for 0.2 seconds, capture the upright normal, set both wheel-end zero-contact frame counts to 60, and fire jump events.
3. Yield WaitForSeconds(vehicle._jumpForceWaitTime), then a fresh WaitForFixedUpdate, before constructing the jump vector. This vehicle field was **0** at capture time.
4. Construct a direction using upright-normal/world-up blending and ExtraLeanValue, then scale by JumpSettings.GetVel(int(abs(direction.y))). Apply the live quick-hop height multiplier only for quick hops, 0.95 for nollie, the manual-hop multiplier when ShouldReduceHopHeight, and jumpForceMult.Value when bound.
5. Yield WaitForSeconds(JumpSettings.JumpForceWaitTime). This settings field was **0.06 seconds** at capture time; it is separate from the earlier vehicle wait.
6. Apply one quarter of the stored vector through IManagedRigidbody interface slot 18, AddForce, with ForceMode value 2 (VelocityChange). Repeat on three subsequent WaitForFixedUpdate resumptions: **four quarter-vector velocity changes total**. Store the full vector as _lastJumpForce after the fourth.
7. After another WaitForFixedUpdate resumption, compute a lateral hop contribution, apply it through the same interface/mode, and mark timeAtLastJump.

Evidence: EnumJump dossier `ev_0a4707eacb8212e89088a0cff80f8992b84440ee623bf2f4017addb2dec0ffe7`; Jump caller `ev_eba856668239ddc0b1cbdc7182b2cf43889d507958615d98c315a4eadcd1de35`. Interface method identity is derived from original metadata token order (`AddForce` token 0x6000172 minus first method token 0x6000160 = slot 18); ForceMode enum explicitly maps VelocityChange to 2.

## Multipliers and direction

The quick-hop helper returns max(playerSmartFloat.Value, 0), or 1 when unavailable. Live capture Value was **2**, while its definition default was 1. EnumJump consumes this result directly as a vector multiplier. Evidence `ev_0543a01f1bf904c8f9130184d709f711d74577026f0ac47eb842528fdb6aae6b`.

ShouldReduceHopHeight is `_timeSinceReleaseMannyInput < 0.15`, not a generic collision check. Its true branch scales by `_manualOrPegHopHeightMultiplier`, live **0.9**. Evidence `ev_d65fcb26769c7a460cb89002e998df14de9456296ad784aca0d9cdcb32de6c43`.

For upright normal N and world-up U, direction before GetVel scaling is inferred from native scalar arithmetic as:

```
s = 0.6 + 0.4 * clamp01(N.y)
D = lerp(U, N*s, clamp01(JumpSettings.PercentRelative))
if (N.y < -0.1): D = N*0.75
D += N * (0.1 * clamp01(abs(ExtraLeanValue)/20))
```

`D` is stored in _lastJumpDir. Surface-relative right is the normalized projection of driver-seat Transform.right onto the upright-normal plane. The later lateral hop uses abs(direction.x) * LateralHopAmount, a 0.9 factor while grinding, and the sign of ExtraLeanValue. If SwingStickLaterralStyle is enabled, direction.x is replaced by 1.2 * _extraLeanStickValue, with a 0.4 deadzone.

## Actual capture corroboration and limits

`recordings/comparison-session01/streets-clear02` completed with 300 real Unity framebuffer images, 2873 fixed samples, and zero observer errors. Physics.gravity=(0,-12.5,0), fixed timestep=0.01 seconds, timeScale=1. At simulation times 229.76,229.77,229.78,229.79 seconds, chassis vertical velocity rose through approximately 2.602,4.091,5.636,7.217 m/s, consistent with four staged upward changes. This does not isolate the other joint, collision, gravity, and animation forces.

The observer measured chassis COM/velocity, not a single rigid bike+rider equivalent. The connected dynamic body graph contains 40 bodies totaling 228.761 kg; its primary named bike-body subset totals 110 kg. Neither total alone establishes an effective physics mass, as active ragdoll drives, gravity enablement and other force paths were not isolated.

Barspin Left was observed at `(trick_v2_set, trick_v2_slot)=(3,4)` and Whip Right at `(5,2)`, corroborated by actual frame labels. These are two separate IDs, not integer IDs 34 and 52.

The native carrier omits unrelated code, imports and initialized runtime state. Indirect implementation binding and exact callback/coroutine scheduling remain bounded observations. Ghidra pseudocode is a derived representation, not original source.

## GetVel and yield consumer follow-up

`JumpSettings.GetVel(int size)` at RVA `0x2e91880` accepts size 1 through 3 and returns `JumpVelocity[size-1]`. Out-of-range size follows an exception path; it does not read the separate Vel field. Evidence `ev_7794ad7fd9e79e59a187c6afde27d97eb7511be23360c0abfa7314257497df63`.

Therefore quick-hop direction.y=1 selects JumpVelocity[0]. The current authored array is [3.2,3.2,4.5]; if that active array is unchanged, live multiplier 2 gives a nominal vector magnitude scale of 6.4 before the other multipliers and force paths. The generic runtime inventory exposed the array type, not its numeric elements, so 6.4 is a static/configuration inference; the runtime measured peak chassis velocity is separate evidence. **Vel=5.2 is not the selected jump strength in this path.**

The VehicleController static constructor raw instructions at RVA 0x900531 through 0x900558 allocate the exact same type-info global used for EnumJump's fresh yield objects and assign it to static offset 0x50. Original metadata identifies that static field as `WaitForFixedUpdate WaitFixed`. This binds those repeated yield objects to WaitForFixedUpdate. Instruction evidence `ev_8bfc39dbabb29e0495746fd9d61e02b56406a2a113ced1f9a71d48815669676a`. The constructor's decompiler pseudocode incorrectly truncates around call sites to an omitted write-barrier callee; complete decoded instructions were used for this claim. No bytes or analysis flow were patched.

`ManagedRigidbodySystem.AddForce` at RVA 0x2e919d0 forwards the supplied vector and ForceMode unchanged to its first list body, then enumerates its body list and forwards them to each body. This suggests a possible duplicate application to the first body. The caller's concrete interface implementation, actual list contents, iterator inclusion and contact/joint response were not measured here, so this cannot be treated as a confirmed effective vehicle impulse multiplier. Evidence `ev_3a4753c2c1250dd508e058e54f09b060e16573755580cec8a7113db1ef94e167`.

Full retained evidence: `build/bmx-movement-current/rea-enum-jump-evidence.json` (11,374,008 bytes, 5 records) and `rea-enum-jump-consumers-evidence.json` (735,391 bytes, 6 records). Exact pseudocode/instructions and carrier manifests are adjacent. Native sessions were closed after export.
