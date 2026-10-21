# BMX Streets observation and standalone comparison

Version 0.3.0 adds an opt-in coupled bike/rider model, measured joint commands, all 58 available trick posture tracks, bike articulation, and literal IK control tracks. The C++ library runs without BMX Streets, Unity, a controller driver, or a game installation. The included Windows demo uses the installed virtual Xbox controller and captures its OpenGL framebuffer.

**Final actual recordings:** all five v2i runs use demo `e0b08145…` and library `44f4ef89…`. They include the source-bound RS flick. The earlier v2g recordings (`6214da3a…` / `01676c4f…`) remain the complete pre-flick baseline; failed or interrupted runs remain excluded.

## Native coverage

The active vehicle provider exposes eight sets of eight slots: 58 available selections and six empty slots. Separate reset-per-selection recordings observed every available selection fire and return through its exit to Idle. All six empty slots produced no trick. This proves observed selection and animation coverage; the observer does not expose an authoritative native landed-score result.

The primary four matrices and a Mc Griddle retry retain fixed physics, body/joint commands, trick state, humanoid poses, and sampled Unity frames. A separate fast matrix supplies independent repeat poses and additional bike/IK channels. Its start-marker sharing failure left fixed-physics/trick channels unusable; `streets-visual-matrix01/capture-channel-limitation.json` records that failure. It is used only for channels that were actually recorded. Fresh V3.3 recordings have healthy channels and close the Nothing and Bus Driver Left visual gaps, plus moving balance, air control, and suspension references.

Native capture changes have been removed and the original CatMod restored. Driver receipts pin the executable, process identity, exact requested timeline, every XInput readback, neutral release, and disconnection. Only the installed virtual controller was tested.

## What was reconstructed

The physical model uses an explicit 30 kg aggregate bike and an independent 80 kg rider body, coupled by an implicit six-axis joint. Measured acceleration-mode drives, radial linear limits, principal inertia orientation, target commands and four-pulse hop scheduling inform the observed profile. The engine host must provide real contacts and apply returned forces and impulses exactly once. The supplied host is a flat-plane demonstration, so it does not establish matching behavior on game terrain.

All 58 available selections have observed posture, 13 bike/control-root channels and four IK limb-control tracks. Of the 55 humanoid bone identifiers, 52 had bound transforms; missing channels remain unavailable. Bike presentation uses measured relative motion on independently created geometry. The rider presentation holder is an explicit reduction driven by the standalone rider, rather than an asserted native skeleton ancestor. IK weights retain their literal native meanings and are not interpreted as `1 - hand release`. The v2 input mapper uses the recovered manual/nose-manual bands, activation delays and hysteresis; full initial stick travel remains preload rather than a latched manual.

Observed command reconstruction fits source samples; it is separate from runtime parity. Humanoid reconstruction has maximum 19.64 mm / 4.95° error; vehicle reconstruction has maximum 2.00 mm / 1.00°. An independent native repeat has 74.58 mm / 18.30° RMS variation across mutually observed phases and 52 bones. Provenance retains source hashes, clocks, availability and per-track boundaries.

The observed input option now recognizes the recovered Ollie/Nollie processed-stick predicates: 0.6 preload, 0.45 opposite crossing and 0.2 s timeout, with neutral rearming. Nominal flick strength is 4.5; R3 remains 6.4. Three new ID 83 command phases cover full-negative preload, the observed Ollie transition, and an independently recorded full-negative-to-neutral cancellation without a hop. The preceding 110 command descriptors and key arrays are unchanged. Recovery command offsets fade to the measured baseline to join the different capture contexts; no body position or velocity is replayed. Intermediate flick amplitudes and a dedicated Nollie command curve are unmeasured.

## Actual controller comparisons

The final **v2i actual recordings** have **19,464** matching controller reports with verified neutral release/disconnection, **45,285** valid and converged simulation samples, 4,259 actual frames and 11,347 rider poses. [The capture audit](recordings/comparison-session02/actual-capture-quality-v2i.json) pins their hashes and counts. Camera, geometry, terrain and engine-clock pacing differ.

| Measurement | Native observation | Recorded standalone v2i |
| --- | --- | --- |
| Available trick selections | 58 fire and reach animation Idle across the primary matrices/retry | 58/58 adopted; 48 caught and scored as landed by the module |
| Presentation coverage | 52 bound humanoid bones; 13 bike/control nodes; four IK limbs | All 58 IDs carry those channels; all 21,195 visible-landmark samples equal their recorded playback bones |
| Independent-repeat posture error | Separate native repeat, including the two visual retries | 71.64 mm / 15.52° RMS, absolute holder-local, over 55,640 paired bone samples |
| Manny peak Chassis / Spring pitch magnitude | 4.846° / 7.819° | 4.415° / 7.111°; the prior module baseline remained nearly level |
| Nosey peak Spring pitch | 3.883° | 3.395°; rear-wheel unloading remains different |
| First R3 hop, rise / engine airtime | 2.510 m / 1.430 s | 1.898 m / 1.120 s |
| Second R3 hop, rise / engine airtime | 2.404 m / 1.390 s | 1.898 m / 1.120 s |
| RS Ollie flick, rise / engine airtime | 1.452 m / 1.140 s | 1.008 m / 0.820 s; one accepted Ollie and four impulse parts |
| Airborne left-stick X net yaw, opposite pulses | −157.64° / +167.56°, reflected into module coordinates | −198.31° / +202.28° |

Native landed scoring is unavailable; animation Idle cannot be equated with module scoring. Posture errors remove holder rigid motion and normalize mutually observed phases; they do not measure world trajectories. Native-repeat and module comparisons use different weighting and posture domains. Runtime vehicle playback reconstructs its fitted samples within 1.954 mm / 0.974°; literal IK weights differ by less than 8.1×10⁻⁷ with no selector mismatch. These are reconstruction checks, separate from independent parity.

Airborne X-pulse yaw improved from the earlier actual v2b values of −346.65° / +326.70° to the v2i values above, following recovered air angular drag and numerical corrections. Overshoot remains about 35–41°.

The input transfer matches observed partial-stick values: ±0.4 HID requests produce preload ±44/127 and manual/nose-manual 56/127; full initial travel produces preload ±1 with no manual latch. The complete moving-controls run has [zero accepted hops or pulse samples](recordings/comparison-session02/actual-controls-hop-audit-v2i.json), including full negative-to-neutral cancellation. Native captures include substantial banking. Native per-wheel flags and module **pre-integration** queries differ: Manny lifts the module front query transiently; Nosey does not reproduce native rear-wheel unloading.

| Paired recording | Scope and source availability |
| --- | --- |
| [All-slot AVI](recordings/comparison-session02/comparison-fast-v2i/comparison.avi), [58-trick contact sheet](recordings/comparison-session02/comparison-fast-v2i/all-58-tricks.jpg) | 288 s timeline; native fast capture supports video/pose/IK, **not fixed physics**. Nothing did not fire there and Bus Driver Left was censored; those panels remain unchanged. |
| [Visual-gap retry AVI](recordings/comparison-session02/comparison-visual-retry-v2i/comparison.avi) | Four actual attempts of Nothing / Bus Driver Left; healthy native channels and 4/4 module catches/lands. |
| [Suspension AVI](recordings/comparison-session02/comparison-suspension-v2i/comparison.avi) | 18 s; one RS flick and two R3 requests at 6 s / 11 s. Workspace history `recordings/comparison-session02/comparison-suspension-v2g/comparison.avi` preserves the earlier missing flick and is excluded from the distribution. |
| [Moving manual/nose AVI](recordings/comparison-session02/comparison-controls-v2i/comparison.avi) | 32 s; partial/full stick requests, supplied contact queries and separate Chassis/rider pitch. |
| [Air-control AVI](recordings/comparison-session02/comparison-air-v2i/comparison.avi) | 32 s; four directional pulses. Yaw overshoot and near-inverted collision differences remain visible. |

Each folder also contains a short GIF, contact sheet and source-frame hash/mapping audit. Videos retain actual frames with resizing, labels and explicit held-frame ages. Original source captures, including buffer time, remain intact.

## Frozen source-flick numerical checks

These checks use library `44f4ef89…`, without a controller or native game. The [headless manifest](recordings/comparison-session02/headless-release-v2h-manifest.json) pins every executable, fixture and report. All outputs are valid and converged:

- Canonical 58-slot sweep: 58 adopted, 48 caught/landed under module rules; 37,700 fixed samples.
- Timing grid: 20 cases, 1,160 slot attempts and 754,000 samples; zero invalid or nonconverged output.
- Shared 32 s controls scenario: 3,201 samples, zero hop requests, accepted requests or pulses. Partial manual values remain unchanged; full negative-to-neutral and initial full positive stay grounded.
- Shared 18 s suspension scenario: 1,801 samples, exactly one accepted Ollie and two accepted R3 hops, with four pulse parts each.
- Older pilot identification: measured-command and autonomous replays both have 1,601 valid/converged samples. Measured first/heldout hop rises are 1.906 / 1.854 m versus native 2.003 / 2.465 m. This is a different capture context from V33.

The deterministic Ollie matches the recorded module rise/time above. Airtime measures each ground flag; the native prelaunch override becomes false before its impulse. Four-pulse rig-COM velocity increments closely match: 1.1023 versus 1.0975 m/s per tick. Headless pre-pulse upward velocity is 0.432 versus 1.355 m/s native, localizing remaining lift/work/context differences before the impulse. See the [pulse audit](recordings/comparison-session02/headless-suspension-v2h/flick-pulse-localization.json). The native observed 0.18 s BeginJump-to-pulse interval includes render/coroutine scheduling; it is not a gesture dwell or a recovered fixed-clock constant. Higher uniform hop-speed candidates were rejected; source strengths remain unchanged.

## Practical limits

This is an independent C++ adaptation, not complete recovered game code or a verified exact clone. Bodies, contacts, presentation hierarchy and geometry differ. Spring stabilization and the source manual actuator are active; the native Chassis PID's general animation-weighted target is not reconstructed. The Manny Manual branch is a configurable inference because its native movement mode was not captured; the Nosey Manual branch was observed live. Native fixed time can advance slower than wall time, so identical timed requests can reach different simulation phases. Live R3 hops remain 21–24% lower and 19–22% shorter; flick rise is about 31% lower, with different ground-flag timing. Air rotation, banking and wheel articulation differ. Older pilot RS samples were zero; V33 partial RS inputs were active. Near-inverted manoeuvres can penetrate the flat host floor because full rig collisions are absent. Short observations do not verify arbitrary sustained loops. Bail/ragdoll, walking, obstacles, full rig collisions and third-party engine integration remain outside the supplied flat-host validation.

The earlier single-body v1 measurements and videos are preserved separately in [the v1 report](reference/comparison-v1-history.md). Their hop/braking errors describe that earlier profile and are not presented as v2 results.
