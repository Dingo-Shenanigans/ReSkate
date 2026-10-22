# Coupled virtual-controller trials

This is a host-only controller harness. It creates no engine marker and
publishes no recorder acknowledgment. The separate engine marker/trace producer
is implemented and pure-reviewed in build42. Neutral baseline276 established a
bounded real entry/active rider binding. The coordinated native277 attempt was
cancelled by the user before the recording ACK, so that attempt did not test
activation-to-GO, virtual input consumption, tricks or landing quality.

The harness itself passed 27 fake-only methods. Those tests used no game, driver,
controller or native API. A completed driver transport is not a full quality
claim. Without a fresh matching genuine engine activation marker it remains
neutral after its declared bootstrap and fails within the unchanged60-second
limit.

`Bmx-CoupledVirtualTest.py` imports the exact approved
`contrib/bmx-lab/Bmx-VirtualGamepad.py`, requiring SHA256
`2964E18026D92EBD2239E0B938D25FACE48A792F2F14C18C43EF094BE3A2AFFE`.
The original file and all its scenarios remain unchanged. Its native owner,
executable/runtime, foreground, Escape, dependency and report guards are reused,
as are bootstrap, ready/go, scheduler, finally-neutral and disconnect handling.
The original six reviewed package/client files and already installed ViGEm are
required for any later execution. There is no installation path here.

The checked-in harness is `contrib/bmx-lab/Bmx-CoupledVirtualTest.py`; its
workspace lookup uses that repository depth. Its source was packaged from the
frozen reviewed host candidate; only relocation, naming and documentation were
adapted. The engine producer remains a separate source component. These three
host files do not change `bmx_coupled_ride_private.inc` or control physics.

## Fixed requests

All four timelines begin neutral and end neutral at6000ms. The original
20ms schedule, maximum120ms lateness,6s maximum,1s neutral bootstrap and60s
global ready wait are unchanged. There is no D-pad, Y, Start or View input.

| Scenario | Requests relative to GO |
| --- | --- |
| `coupled-neutral` | Neutral throughout |
| `coupled-pedal-brake` | A240–1240ms; B2000–2500ms; neutral otherwise |
| `coupled-steer` | A240–1640ms; LS x+0.25 at840–1240ms, x−0.25 at1240–1640ms; B2400–2800ms |
| `coupled-hop-barspin-left` | A240–1240ms; R3 at1500–1620ms; LB+RS y−1 at1800–1980ms; neutral otherwise |

These use frozen cut32 mapping: A pedal, B brake, R3 quick hop, LS steering,
and airborne LB+RS back requesting catalog ID34 (`barspin_left`). The last
scenario is for a later test after neutral, pedal/brake and steering qualify.
Fixed timing cannot prove the bike was airborne, a trick was accepted or caught,
or a landing succeeded. Those require actual coupled observation and video.
The existing old one-body `native-y`, equipment and trick scenarios are neither
modified nor used by this front end.

## Marker protocol (engine producer implemented in build42)

The engine must write one immutable activation and one eventual immutable
terminal record into the exact existing isolated profile
`build/bmx-user-data-coupled-test`. No marker may come from the working user
profile. Publish complete UTF-8 JSON, without BOM, by exclusive temporary file
and no-overwrite atomic rename. Each record is at most65536bytes and contains
exactly the fields below. No duplicated fields, NaN, Infinity or partial files.

Use these names, where FILETIME is canonical unsigned decimal and nonce is the
exact lowercase32-hex recorder nonce:

```text
coupled-<PID>-<FILETIME>-<capture_nonce>-activation.json
coupled-<PID>-<FILETIME>-<capture_nonce>-terminal.json
```

`process_creation_filetime` comes from the actual Windows process creation
FILETIME, not a UTC reparse or PID alone. The host independently supplies and
checks the reviewed runtime SHA through its root-owned owner receipt and the
unchanged `WindowsGuard`. A marker makes no runtime-image hash claim.

Activation example (addresses and epochs below are synthetic documentation):

```json
{
  "schema": 1,
  "event": "coupled_trial_activated",
  "process_id": 1234,
  "process_creation_filetime": "134054832620000000",
  "capture_nonce": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "operator_generation": 100001,
  "activation_begun_tick_ms": 101000,
  "activation_succeeded_tick_ms": 101002,
  "conservative_expiry_tick_ms": 111000,
  "epoch": {
    "owner_world": [101, 102, 103, 104, 105, 106, 107, 108],
    "token": 7,
    "serial": 100,
    "time": 1.5,
    "h": 0.01666666567325592,
    "bodies": [
      {"world": 106, "index": 11, "generation": 0},
      {"world": 106, "index": 12, "generation": 2}
    ]
  },
  "entry_ready": true,
  "actual_spring_render_verified": true,
  "backend_active": true,
  "normal_force_adoption_enabled": false
}
```

The engine must capture `activation_begun_tick_ms` with real GetTickCount64
immediately before `activate_prepared`, then publish only after that exact call
succeeds, following real entry completion and completed actual Spring render
verification. `activation_succeeded_tick_ms` is the real after-call tick. Never
publish readiness based on elapsed entry time. Set conservative expiry to
**before-call tick+10000**, because the backend's existing10s lease starts inside
the call; the later operator finish tick can overstate safe remaining time.
`operator_generation` must be the actual current operator generation (currently
its created tick), never an arbitrary host counter.

The epoch must be the genuine jointly qualified current native pair for that
activation. `owner_world` order is client, entity, core, context, rig, world,
native state, world context, matching `native_coupled::policy::Epoch`. Body order
is bike then Spring rider, with actual world/index/generation. The host requires
all eight nonzero identities, distinct valid indices, a common world, token,
native serial, finite native time and actual native60Hz h. It does not invent
an owner/body identity before the genuine marker supplies the first one.
Endpoint schema checks are not proof that every intervening physics epoch ran.

Terminal example (synthetic):

```json
{
  "schema": 1,
  "event": "coupled_trial_terminal",
  "process_id": 1234,
  "process_creation_filetime": "134054832620000000",
  "capture_nonce": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "operator_generation": 100001,
  "terminal_tick_ms": 107500,
  "issue": "actual_terminal_reason",
  "epoch": null
}
```

Publish the terminal signal as soon as the operator terminates or activation
fails; do not wait for the existing delayed final report. Include a real epoch
when a qualified pair remains available, with the same owner/body/token as
activation. Otherwise export `null` honestly. A terminal received before
activation also aborts. Malformed, foreign, future or mutated marker data aborts
instead of being ignored. Terminal handling is cooperative polling in the
original driver guard; it cannot guarantee physical neutralization during a
host or OS stall. It does not relax the game-side10s lease or cleanup.

## Synchronization

1. Root creates a fresh owned process/profile trial, exact runtime owner receipt,
   recording-gate nonce and waiting status using the existing no-Ride coupled
   launch. That launch has empty startup commands and only the already reviewed
   coupled private flags; this host adds no mode flags.
2. Start this front end with an exclusive ledger and fresh start-gate directory
   while the recording gate is still waiting. It validates owner/status first,
   constructs the approved backend only on explicit `--execute`, performs the
   declared raw-one-LSB/neutral bootstrap and exact two-neutral readbacks, then
   emits ordinary `ready.json` and retains the neutral virtual pad.
3. Root observes that ready record, starts recording, and releases the existing
   recorder gate using its actual ACK workflow. This program never writes the
   recorder ready file or ACK. It rejects an initially released capture status.
4. Native entry then runs. The host accepts only a new matching activation
   marker, at most1000ms old, after the exact released recorder tick, with at
   least6620ms conservative time remaining. Only then does it exclusively
   publish the original nonce/PID/FILETIME-bound `go.json` and begin the fixed
   six-second scenario. External pre-published GO cannot bypass activation.
5. The original exact owner/runtime/foreground/Escape guard runs first at every
   check. The added guard detects terminal signals, activation deletion or
   mutation, GetTickCount64 rewind and conservative expiry. Any refusal reaches
   the original `run` finally path: attempt neutral, disconnect, retain errors.
   The ledgers state `input_consumed: null` and `native_quality: null`.

The driver can finish and disconnect at6s while the native10s trial continues.
That completed transport is not whole-trial acceptance. Root must continue
recording and collect the genuine final coupled report/terminal and teardown.

Archive owner, capture status/ACK, ready/go, both markers, controller ledger,
actual coupled report, runtime/source pins and video under the root's exact
process generation. The host records the activation SHA, wrapper SHA, approved
driver SHA and externally supplied runtime pin. The first marker establishes
pair identity; it is not independent native-observer acceptance by itself.
No marker alone is an immutable complete-run provenance receipt.

Execution shape for a separately owned, recorded trial. The live user session
must not be targeted, and the untested277 coordinated attempt is not acceptance.
The engine producer and host source/runtime pins must match the chosen build:

```powershell
python -B contrib/bmx-lab/Bmx-CoupledVirtualTest.py `
  --execute --scenario coupled-neutral `
  --owner build/bmx/<run>/owner.json `
  --ledger build/bmx/<run>/coupled-virtual.jsonl `
  --start-gate build/bmx/<run>/fresh-controller-gate --start-nonce <32hex> `
  --capture-status build/bmx/<run>/recording-gate.status.json --capture-nonce <32hex>
```

Fake-only verification:

```powershell
python -B contrib/bmx-lab/Test-BmxCoupledVirtualTest.py
```

Tests import the approved scheduler source but never construct WindowsGuard,
call GetTickCount64, import vgamepad, query a process or submit an OS report.
All profile/gate fixtures are temporary owned children of ignored
`build/bmx/coupled-virtual-test-fixtures`.
