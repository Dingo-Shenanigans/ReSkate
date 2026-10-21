# Measured BMX Streets comparison

The opt-in `streets_observed_v1()` profile improves measured hop height, rider proportions and braking. It remains an independent C++ reduction with procedural animation; these recordings do not establish complete movement parity.

Actual recordings used the installed virtual XInput controller with identical requested timelines in both programs. Every report matched its XInput readback, neutral cleanup succeeded, and removal returned 1167. Native full/short runs contain 1,434/577 verified reports; final C++ full/short runs contain 1,454/583. Native observation changes were restored after capture. No physical controller was tested.

## Workspace-only recording history

The module-relative paths below refer to preserved workspace history. These session01 videos and reports are intentionally excluded from the distribution.

| Comparison | Preview | Video | Coverage |
| --- | --- | --- | --- |
| Before tuning | GIF: `recordings/comparison-session01/comparison-baseline02/comparison.gif` | AVI: `recordings/comparison-session01/comparison-baseline02/comparison.avi` | Historical full run, includes capture buffer |
| Final full sequence | GIF: `recordings/comparison-session01/comparison-tuned02/comparison.gif` | AVI: `recordings/comparison-session01/comparison-tuned02/comparison.avi` | 299 paired frames, through 30 seconds |
| Final flat acceleration/brake/hop | GIF: `recordings/comparison-session01/comparison-flat04/comparison.gif` | AVI: `recordings/comparison-session01/comparison-flat04/comparison.avi` | 119 paired frames, through 12 seconds |

All panels use actual Unity/OpenGL framebuffer samples. Audits retain source hashes, timestamps and selected-frame mappings, including the complete source inventories beyond the presentation cut. Images are resized and preceding frames may be held; no rider motion is synthesized. Cameras and body origins differ.

## Before and after

Full-sequence height uses native chassis COM and standalone combined-body COM, each measured relative to its own phase start.

| Measurement | Native | Before | Final |
| --- | ---: | ---: | ---: |
| Quick-hop rise | 2.3476 m | 0.9960 m | 2.3385 m, −0.39% |
| Quick-hop engine airtime | 1.3700 s | 0.9167 s | 1.2400 s, −9.49% |
| Barspin-request rise | 2.6314 m | 0.9390 m | 2.3385 m, −11.13% |
| Tailwhip-request rise | 2.3428 m | 0.9960 m | 2.3385 m, −0.18% |
| Arm/leg segment-length error | Measured humanoid bone origins | +11.0% to +44.9% | Absolute relative error below 0.001% |

The segment result is a direct geometry fit, not animation accuracy. Final hip-to-head distance differs by −0.58%, shoulder-root span by +0.17%, and hand separation remains +17.39%. Native wrists are not proven grip anchors. Actual rider measurements: `recordings/comparison-session01/rider-comparison-tuned02.json` preserve both sides, sample gates and source hashes.

## Flat braking and the two clocks

The native full run crosses a drop during braking: chassis height changes by −0.980 m. Its brake phase is excluded from calibration. The short native run remains near level: pedal/brake height spans 0.0131/0.0094 m with no observed loss of support.

Actual short sessions give early engine-clock acceleration 2.4911 m/s² native versus 2.5572 m/s² C++ (+2.65%). Moving-brake OLS over wall 3.08–4.3 s is 3.7318 versus 3.9346 m/s²; later 3.4–4.3 s is 3.1465 versus 3.1959 m/s². Actual wall-aligned moving-brake speed RMSE is 0.1841 m/s. Short-hop rise is 2.3279 m native chassis, 2.3997 m native combined COM, and 2.3385 m C++. Live measurements: `recordings/comparison-session01/actual-flat04-fits.json` retain raw ranges and fit quality.

A separate **headless numeric replay** feeds the same 1,272 native fixed-sample requests into `FlatWorld` at 100 Hz. It has no controller or video and explicitly records `actual_controller_recording:false`. The fitted brake gain 0.64 reduces moving-brake RMSE from 0.7545 to 0.1297 m/s (82.8%). Prefix 3.08–3.4 s improves 0.5655→0.1900; later 3.4–4.3 s improves 0.8101→0.1004. Gain selection excluded the later interval, but initial bounds used this capture: it is a within-run holdout, not independent-session validation. Replay evidence: `recordings/comparison-session01/headless-brake-final04/brake-sweep.json`.

Actual sessions share wall-time inputs while native simulation advances more slowly under observation. Native short pedal engine/wall ratio is 0.9267 versus 0.9962 in C++. Live and replay states/clocks differ; the remaining difference between their brake fits is not attributed to a proven cause. In the full run, whole-overlap wall-speed RMSE changes 0.4806→0.5301 m/s; this global metric includes the excluded drop and does not show an overall speed-match improvement.

## Remaining coverage

Native V2 timer resets and on-screen labels validate two requests: tuple `(set 3, slot 4)` is Barspin Left; `(set 5, slot 2)` is Whip Right. They are tuples, not integer IDs 34/52. The final C++ full recording has three takeoffs and two landed combos. Native landed-score equivalence and the remaining trick catalog are unverified.

Hop airtime, barspin height and staged launch timing still differ. The 18–19 s right-stick phase is labeled **RS balance request**; successful native manual behavior is not established. Native multi-body joints, force weights and per-body inertia are not reproduced by the independent 106 kg combined body; the observed velocity comes from a 10 kg chassis component. Full animation retargeting, contact-anchor parity, ragdoll/bail, world obstacles and terrain behavior remain outside this flat-host comparison.
