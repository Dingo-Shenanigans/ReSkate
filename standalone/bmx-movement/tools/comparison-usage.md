# Matched controller comparison

`drive_target.py` accepts only the installed BMX Streets executable or the standalone demo executable. It does not launch, activate or navigate either program. Before executing, put the selected game in the foreground, on the intended test surface, with neutral controls. Runs abort on foreground loss or Escape and release/remove the virtual controller in cleanup.

Use the existing private `build/bmx-gamepad-runtime/Scripts/python.exe` for actual input. A normal Python interpreter is sufficient for dry-run. No driver installation is performed.

```text
python drive_target.py --target streets --scenario full

<private-python> -B drive_target.py --target streets --scenario full --execute --pid <game-pid> --output <new-recording>/streets.driver.json --start-marker <new-recording>/start-gate.json
<private-python> -B drive_target.py --target demo --scenario full --execute --pid <demo-pid> --output <new-recording>/demo.driver.json --start-marker <new-recording>/demo-start.json
```

Output and marker paths must be new files inside the standalone package. Both runs use the exact same timeline hash. The start marker records `timeline_start_utc` after neutral bootstrap; the observer can wait for it before recording. The optional `--ready-gate` waits at most 60 seconds for a JSON file containing `{"ready":true,"target":"streets","pid":123}` while keeping the controller neutral and checking the selected foreground process.

Use `--arm-file new-arm.json` to connect the virtual controller and hold it neutral while preparing the scene. The helper writes `<output>.connected.json` with the verified slot/process/neutral proof, then waits at most 180 seconds. Create the arm file with `{"armed":true,"target":"streets","pid":123}` for that exact selected process to start the timeline. Foreground identity, Escape and actual neutral XInput readback are checked throughout the hold; cleanup remains automatic on timeout or failure. A successful connection is not evidence that the timeline has started.

During that hold, a JSON request `{"armed":false,"target":"streets","pid":123,"prelude":"turn90","nonce":"turn-1"}` runs one fixed navigation prelude on the same connected controller, then holds neutral again. Only `confirm`, `turn90` and `creep` are accepted; arbitrary states or durations are rejected. Each nonce is used once, with at most three preludes (`--max-navigation` can lower that cap). Completion/readbacks are published in `<output>.navigation.json`. The default total hold remains 180 seconds; `--arm-timeout` explicitly permits up to 300 seconds including preludes.

The Streets-only `confirm` scenario presses A once after 200 ms, releases after another 150 ms, and ends at 1.5 seconds. It is a menu-navigation request, not a physics comparison scenario.

The Streets-only `turn90` prelude requests R3 from 0.2–0.3 seconds, then full positive LS X from 0.4–0.65 seconds, ending neutral at 3 seconds. Its name is a requested repositioning action, not a guarantee of a 90-degree result. `creep` requests A from 0.2–1.0 seconds and ends neutral at 1.5 seconds.

The 30-second `full` scenario requests acceleration, coast, braking, one simple quick hop, a quick hop plus barspin input, a quick hop plus tailwhip input, and a short manual input. It does not declare those requests successful. Shorter `accel_brake`, `hop`, `tricks`, `manual` and `steer` scenarios are also available.

The shared 12-second `flat_short` scenario uses pedal 0.5–2.5 seconds, coast to 3, brake 3–6, neutral to 8, R3 hop 8–8.1, then neutral to 12. The shorter travel helps keep acceleration and braking on one flat surface. Its shared timeline hash is `dcafc1699132e214224b16c285efea2eaf2594d99cdaccc66d7a4b5786c44e32`.

`comparison.py` requires completed driver receipts with matching scenario/timeline hashes, matching actual XInput readbacks and verified controller cleanup. It aligns body telemetry using `utc_seconds` or ISO `utc`; otherwise supply an explicit clock origin/offset. It reports horizontal speed error, phase acceleration/braking, vertical motion and observed support transitions. Interior flight fits are observations of effective acceleration, not recovered source equations. Selected native chassis and combined standalone rigid bodies may have different mass composition; inspect source identity/mass and native `vehicle_com_*` values before interpreting a fit.

```text
<pillow-python> -B comparison.py --streets-csv streets/telemetry.csv --demo-csv demo.csv --streets-driver streets.driver.json --demo-driver demo.driver.json --streets-frames streets --demo-frames demo-frames --output new-comparison-directory --gif
```

Frame directories need `frames.csv` with index/file/elapsed_seconds/simulation_seconds/width/height. `capture-summary.json` should declare `start_utc_seconds`, the actual origin of the elapsed frame clock. Then source offsets derive automatically from the driver timeline. Explicit `--streets-frames-offset` / `--demo-frames-offset` are accepted when that clock origin is unavailable. MJPEG AVI sources can be used instead of frame directories with `--streets-video` / `--demo-video`; those require explicit presentation offsets.

The side-by-side video contains resized actual source frames with labels and held frames on a common presentation grid. Its audit retains each source hash, capture time, selected source-frame index and source age. It generates no intermediate rider motion. Camera/surface differences remain visible limitations; body telemetry alone does not establish rider animation or all-trick parity.

Pass `--video-max-end 30` for `full` or `--video-max-end 12` for `flat_short` to omit post-input controller-disconnect menus from the final comparison presentation. Entire source frame lists remain in the audit, and numeric metrics retain their declared overlap.

`analyze_reference_run.py <streets-directory> --driver <completed-driver.json> --output <new-analysis.json>` provides a source-only trajectory and skeleton report. Pedal and brake fits use the recorded engine clock; moving brake subwindows avoid averaging in standing time. Hop rises retain separate chassis and combined vehicle COM references, and observed airtime is reported on both clocks. Bone positions use explicit body-local origins; they require retargeting before use in another rig.

`replay_native.cpp` is a headless calibration bench linked to `bmx_movement`. `native_replay.py fixture` converts a native `flat_short` source CSV and completed driver receipt into one raw requested state per native fixed sample, using each report's UTC. The probe uses the opt-in observed profile at100Hz and never accesses a controller or game. `native_replay.py analyze` compares physical samples; `sweep` bounds a hop-speed sweep; `brake-sweep` reports a fixed prefix and later within-run holdout. All headless artifacts explicitly set `actual_controller_recording:false` and cannot replace live matched video. Core defaults remain outside these tools.
