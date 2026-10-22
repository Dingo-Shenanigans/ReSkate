# Python virtual controller harness

`Bmx-VirtualGamepad.py` submits short, fixed Xbox controller report sequences to the isolated Skate process only when `--execute` is supplied. The default is a fast dry-run that does not import vgamepad, inspect a game, connect a device, change focus, inject keyboard input, or install anything. It writes a new JSONL plan/ledger under `build/bmx/virtual-gamepad`.

Run with Python3.11 or newer, from the repository root:

```powershell
python -B contrib/bmx-lab/Bmx-VirtualGamepad.py --scenario native-y
python -B contrib/bmx-lab/Test-BmxVirtualGamepad.py
```

The focused tests use fake backends, clocks and Win32 metadata adapters. They exercise the actual runtime guard check body, report sender, abort/release/disconnect paths, invalid timelines, inherited-free dry-run boundary and owner-file validation. They create no real controller and do not import vgamepad. They are not native gameplay evidence.

## Dependencies and host setup

Execution requires a separately approved and verified ViGEmBus driver installation and the exact private Python environment with the **six reviewed vgamepad0.1.0 files**. The user has approved driver setup in this development session; the setup/install verification is handled separately. This script neither installs dependencies nor launches an installer, and it does not prompt for another installation approval.

Use the separately verified official ViGEmBus1.22.0 setup, whose release removes the updater. The upstream driver project is archived. Preserve its Authenticode/hash/install verification receipt. Installing a kernel driver on another machine or changing the approved installation remains a separate setup action. See the [official release](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0).

The published PyPI0.1.0 artifact is a source archive whose setup script can launch its bundled older driver MSI and **does not honor** `VGAMEPAD_SKIP_VIGEMBUS_INSTALL`. Upstream main's0.1.3 setup has that flag; it must not be assumed to protect0.1.0. Do not pip-install the source archive, execute setup.py, or run the bundled MSI. The separately reviewed setup copies only five package Python files and the adjacent x64 ViGEmClient.dll from the hash-verified archive into the private environment, with no setup execution. The harness never calls pip. For future package versions, prefer a separately verified binary wheel and re-review the backend; do not fall back silently to source setup. See the [published package artifacts](https://pypi.org/project/vgamepad/0.1.0/#files) and [upstream setup source](https://github.com/yannbouteiller/vgamepad/blob/main/setup.py).

The private environment is `build/bmx-gamepad-runtime`, and its sole supported package root is `Lib/site-packages/vgamepad`. Execution reads `build/bmx/gamepad-setup/dependency-verification.json` (overridable only with an equally strict receipt via `--dependency-receipt`). It must bind schema1/event `bmx_virtual_gamepad_dependency`, version0.1.0, source SHA256 `57F6BD01AEC0C172947517FB782D150EF9B285F7F4D524C317374FA5C24A89DE`, the exact private interpreter/package paths, all six relative path/hash/size entries, and both setup/installer-executed fields false. The harness independently pins the six hashes, checks the actual files and package inventory, refuses reparse paths/extra files/bytecode caches, and checks the top-level import origin before importing anything from vgamepad. It requires a fresh Python process with no preimported vgamepad and disables package bytecode creation.

The six files are `__init__.py`, `win/__init__.py`, `win/vigem_client.py`, `win/vigem_commons.py`, `win/virtual_gamepad.py`, and `win/vigem/client/x64/ViGEmClient.dll`. No dist-info version lookup is needed for the manual copy. The first import occurs only after the owned-process/focus guard has passed. vgamepad opens its bus during import; its Xbox controller constructor submits an initial neutral report. That submission alone does not establish neutral XInput readback: the driver has a cached startup packet with nonzero axes. The reviewed0.1.0 backend removes/frees the device in its destructor. The wrapper makes that owner cleanup idempotent and calls it explicitly in `finally`, after sending neutral. The corresponding [upstream backend](https://github.com/yannbouteiller/vgamepad/blob/main/vgamepad/win/virtual_gamepad.py) explains the API; the exact published0.1.0 bytes are pinned locally. No optional driver/client API is loaded by dry-run or tests.

Before any scenario, the harness explicitly submits one raw left-stick X unit (`1` out of32767), with all buttons, triggers and other axes zero, followed by neutral. This bootstrap clears the driver's zero-report duplicate cache. The startup byte pattern and duplicate-cache behavior are documented in the [driver source](https://github.com/nefarius/ViGEmBus/blob/master/sys/XusbPdo.cpp). Every bootstrap attempt/result is timestamped separately in the ledger and guarded by owned-process, foreground and Escape checks. The public driver `vigem_target_x360_get_user_index` API supplies the actual XInput slot; the internal device index is not used as an XInput slot. Two exact all-zero XInput report observations on the same slot are required within1s before scenario inputs are allowed. A failed or late bootstrap aborts and still attempts neutral and removal. The initial device attachment and SDK constructor update precede this verification; the controller is globally visible during that short interval.

## Ownership and execution

An operator must first launch the exact isolated game, choose the intended test state and begin a separate genuine screen capture if needed. The harness never starts a game, arms a native observer, activates a window, changes the camera, captures an image, or posts to Reddit. A captured midtrick frame must come from the actual game recording and be tied to the ledger; a submitted trick input alone is not a trick screenshot.

Write a **new, explicit** owner receipt under `build/bmx`, using the supervisor's actual current PID, its exact Windows creation FILETIME and the installed runtime SHA256. Never copy an old PID/time. This example is a schema only; placeholder values are deliberately unusable:

```json
{
  "schema": 1,
  "event": "bmx_virtual_gamepad_owner",
  "process_id": 12345,
  "process_creation_filetime": "REPLACE_WITH_EXACT_CURRENT_FILETIME",
  "game_exe": "C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-game\\Skate.exe",
  "runtime_sha256": "REPLACE_WITH_VERIFIED_CURRENT_RUNTIME_SHA256"
}
```

Select the approved local Python environment, focus the game externally, and run one explicit scenario:

```powershell
build\bmx-gamepad-runtime\Scripts\python.exe -B contrib/bmx-lab/Bmx-VirtualGamepad.py --execute --owner build/bmx/current-virtual-pad-owner.json --scenario native-y
```

The execution guard binds the exact workspace, isolated executable path, PID, creation FILETIME, supported game hash and runtime disk hash. It holds a process metadata handle, checks process liveness/path/creation, foreground PID and Escape on each report and during waits. It checks file identity/size/timestamps each time, and re-enumerates the exact loaded ReSkate.dll path at least every200ms. Runtime hash is a disk identity; it is not an in-memory image hash or proof of loaded configuration. The receipt is an explicit operator assertion bound to actual process metadata, not a signed attestation or proof of gameplay state.

**Escape, focus loss, process exit, identity/file change, driver error, scheduler delay over120ms, keyboard interrupt or a ledger failure abort the scenario.** Cleanup still attempts a full neutral report and device removal even when focus is lost. Results record cleanup failures rather than claiming release. The guard polls every10ms between50Hz reports; this is not atomic OS routing or a real-time watchdog. A blocked Python/driver call can delay detection. Virtual controllers are visible to Windows globally, and game-side selection among multiple pads must be measured independently.

## Optional ready/go coordination

`--start-gate <fresh-directory> --start-nonce <32-hex-characters>` is available only with `--execute`; both options are required together. The directory must not already exist and must be inside `build/bmx`. The harness creates it exclusively. It refuses existing directories, preexisting ready/go files, path escapes and reparse paths. Defaults and fixed input timelines are unchanged when these options are absent.

Use this option to construct and verify the neutral controller while the game console remains open. After bootstrap, the harness writes an exclusive `ready.json` containing schema1/event `bmx_virtual_gamepad_ready`, PID, exact creation FILETIME string, executable/runtime identities, nonce, scenario, source hash and UTC. It keeps the neutral device and checks the owned process, focus and Escape every10ms while waiting up to60s. No scheduled scenario report is submitted until a valid trigger arrives. This neutral preflight wait is separate from the scenario's maximum6s duration.

The operator must read and verify the ready receipt, close the console using the supported UI tool, then write a complete temporary JSON file in that gate directory and **atomically rename** it to `go.json`. Do not create or write `go.json` in place. Its exact five fields are:

```json
{
  "schema": 1,
  "event": "bmx_virtual_gamepad_go",
  "process_id": 12345,
  "process_creation_filetime": "REPLACE_WITH_CURRENT_READY_FILETIME",
  "nonce": "REPLACE_WITH_CURRENT_32_HEX_NONCE"
}
```

The PID, FILETIME string and nonce must exactly match the running harness owner and ready receipt. These placeholder values are unusable. Extra fields, malformed or oversized JSON, changed files, mismatches, timeouts, focus loss, Escape or owner failure abort before scenario inputs; cleanup still attempts neutral and device removal. The scenario timer starts after matching go acceptance. Ready/wait/go events are recorded in the ledger. The trigger is an operator assertion, not evidence that the console closed, the native observer armed or the game consumed input. Each retry needs a new gate directory and nonce; old receipts are retained as evidence.

## Fixed scenarios and current controls

All scenarios begin/end neutral, finish within6s and leave virtual D-pad/menu buttons untouched. They do not alter the physical controller's D-pad mappings. Durations below are planned wall timing; the ledger records actual host times and lateness as well.

| Scenario | Inputs requested | Required starting state / evidence boundary |
|---|---|---|
| `native-y` | Neutral240ms, North/Y200ms, release through3.6s | Fresh isolated NativeOnly session, observer already armed, zero BMX activity. A **virtual North** comparison, not a physical-controller baseline. |
| `ground-pump` | Neutral240ms, A to1000ms, coast to1200ms, LS Y=-0.7 for300ms, release and settle to3200ms | Already mounted on clear supported ground. Checks the direct pump channel; reports alone do not establish force submission or handling quality. |
| `ground-pump-switch` | Same pump, brake1500–2300ms, then one LB+Y chord and release through4000ms | Already mounted; normal parking may retain the pump window in a ride trace. Verify the new trace and actual switch separately. |
| `equipment-switch` | LB prehold, LB+Y200ms, release, settle to4.4s | Ordinary Ride session with the current equipment-switch path. Excluded from NativeOnly isolation. |
| `bunnyhop-tailwhip` | A pedal, release, rear stick back340ms then forward80ms, neutral, both triggers+rear stick right180ms | Already mounted, clear ground. Rear-flick hop and tailwhip-right request use the current mapper. Airborne eligibility and completion remain unknown. |
| `quickhop-barspin` | A pedal, release, R3 quick hop120ms, neutral, LB+rear stick back180ms | Already mounted. R3 and left-barspin request use the current mapper. An insufficient hop may refuse the trick. |

The bindings come from `Engine/Game/Input/bmx_input_sample.h` and `Engine/Game/BMX/bmx_input.h`: A pedals, B brakes, R3 requests quick hop, LB+rear-back selects left barspin, and both triggers+rear-right selects right tailwhip. Left stick is available for steering; right stick controls rear gestures. These tests do not change either stick mapping. No arbitrary external timeline, unbounded hold, replay loop or synthetic native state is accepted.

The JSONL ledger records UTC and host monotonic time for each report, the requested full controller state, whether the backend update returned, owner/source/dependency identities, final neutral attempt and disconnect result. `driver_report_count` counts scheduled updates that returned; it excludes the SDK constructor update, explicit bootstrap and final neutral release, and is not a count of submission attempts. A failing update or partial constructor may have delivered an uncertain report, so a zero count is not proof that no driver input occurred. Factory failure records delivery as unknown. `neutral_release_succeeded` means the neutral update returned; `disconnect_succeeded` means the SDK remove/free wrapper returned. The pinned SDK ignores the native removal status, so these fields do not independently verify XInput neutral or post-removal absence. Measure those separately when required. Dry-run explicitly marks timing simulated and every driver submission false. `input_consumed` and `native_quality` stay null in every result. Successful report submission is not evidence of native input consumption, mounting, a trick, a landing, a good pose or a quality score. Join genuine capture and native trace evidence separately before judging any of those outcomes.
