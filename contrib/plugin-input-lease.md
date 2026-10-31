# Selected-stream controller leases and capture

The passive methods reserve and observe one XInput stream already seen by ReSkate. They do not change reports or control an actor. The separately permissioned capture methods suppress that selected stream and retain ownership until the required handback and callback drain are confirmed. Returned-buffer publication and cleanup receipts do not establish what gameplay consumed.

## Passive leases

Declare `input.lease` in `reskate-plugin.json`. The ordinary `input` permission does not grant these methods, and `input.lease` does not grant key binding or other ordinary input methods. All six methods check permission and client phase before validating arguments or reaching a backend. Server copies refuse them.

| Method | Success | Refusal |
| --- | --- | --- |
| `reskate.input.lease(user)` | Opaque handle for integer XInput user 0..3 | `nil, reason` |
| `reskate.input.lease_read(handle)` | Read-only sample with opaque `epoch` | `nil, reason` |
| `reskate.input.lease_renew(handle, sample)` | `true` | `false, reason` |
| `reskate.input.lease_release(handle)` | `true` | `false, reason` |
| `reskate.input.lease_state(handle)` | Read-only receipt | `nil, reason` |
| `reskate.input.lease_acknowledge(handle)` | `true` | `false, reason` |

Wrong argument types/counts, forged capabilities and permission/client violations raise a Lua error. A missing native backend returns `unavailable`. A handle belongs to one VM instance, including across reloads; it cannot be serialized to another plugin. No method takes a raw address, owner ID or caller-created source identity.

### Observations and renewal

A sample has decimal-string `sequence`, `source_epoch`, `observation_serial`, `observed_ms` and `context_generation`; numeric `user` and `packet`; boolean `neutral`; and a read-only `pad` with `buttons`, `left_trigger`, `right_trigger`, `left_x`, `left_y`, `right_x`, `right_y`. The pad uses exact raw XInput integer ranges: `buttons` is an integer bitmask, axes are signed 16-bit values and triggers are unsigned bytes. Every 64-bit field remains a decimal string, including values beyond Lua's exact numeric range.

`sample.epoch` retains the native snapshot issued by that read. Renewal uses that hidden snapshot, not the sample's diagnostic fields. Cloning a sample and changing its public fields cannot manufacture a different observation. The backend requires its exact latest issued sample, no older than 100 ms, and each issued sample can renew once. A renewal grants a 250 ms interval from the current backend time after validating that observation; it does not create a new observation or source identity. The independent 100 ms source freshness guard still applies, so an unchanged stream can expire before the lease deadline.

### Receipts and release

A passive receipt reports decimal-string `sequence`, `requested_ms`, `expires_ms`, `observed_ms`, `source_epoch`, `context_generation`, `last_observation`, `renewals`; numeric `user` and `polls_in_flight` (0..128); `status`, `reason`, `source_released`, `orphaned`, `capture_reserved` and `write_reserved`. A newly admitted `warming` receipt has zero observation/time/renewal strings. `active` has an issued observation. Neither state reports the source released. `released`, `revoked` and `expired` are terminal; their original stop reason remains available. The reason words and fixed numeric bounds are declared in `Engine/Game/Input/input_lease.h`.

A terminal record may still reserve its source while callbacks which already began are draining. `source_released` becomes true only when `polls_in_flight` is zero and neither `capture_reserved` nor `write_reserved` remains. Capture ownership, output cleanup debt or an outstanding callback can retain that reservation. ACK requires terminal state, released source, no capture/write reservation and zero callbacks; it removes that retained passive receipt. It cannot erase capture or writer cleanup duty.

Release, receipt reads and ACK remain available on the original client thread after focus/readiness/menu loss so a plugin can discharge ownership. Other policy/read failures refuse samples and can terminalize the lease. A VM close revokes its touched owner once, marks retained records orphaned and permits only already-started callback drain, never publication into a later lease.

Passive samples and receipts label their scope: `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="observe"`, `native_consumption="unobserved"`. The stream identity is not proof of a physical device or game consumption. These six methods grant neither capture, report writing nor actor control.

The native service has four source slots, at most eight retained records and at most one live lease; an old callback reservation blocks replacement. Each VM can make at most 64 valid calls across the six passive, four capture and four [tracked-output methods](controller-layer-api.md), and one passive lease request in a normal tick interval. These counters reset in the existing VM tick; startup calls share the first interval. Close cleanup is separate from this call budget. A refused or malformed request cannot be retried indefinitely within an interval.

## Selected-stream capture

Declare both `input.capture` and `input.lease`. Ordinary `input` and screen `capture` permissions do not grant these methods. Both permissions and client phase are checked before arguments/backend access. A wrong arity, forged/cross-VM handle or invalid issued epoch raises a Lua error.

| Method | Success | Refusal |
| --- | --- | --- |
| `reskate.input.lease_capture(handle, sample)` | `true` | `false, reason` |
| `reskate.input.lease_capture_state(handle)` | Read-only capture receipt | `nil, "unavailable"` |
| `reskate.input.lease_release_capture(handle)` | `true` | `false, "unavailable"` |
| `reskate.input.lease_acknowledge_capture(handle)` | `true` | `false, "unavailable"` |

All use the same opaque passive handle. Admission uses the stored native snapshot in the whole plain sample table's issued opaque epoch, rather than rebuilding it from visible fields. A clone carrying that same epoch cannot change the native observation. Capture starts `waiting_neutral`; only a later qualified neutral raw original can publish capture. The 100 ms sample freshness and 250 ms lease bounds remain. The 1000 ms release boundary marks unresolved cleanup quarantined and retained; it never permits timed disposal, forced completion, ACK or new admission while duty remains. No argument grants arbitrary input writes or chooses a native key/owner.

A capture receipt has decimal-string `sequence`, `source_epoch`, `context_generation`, `generation`, `revision`, `expires_ms`, `publications`, `neutral_releases`; numeric `user` (0..3) and known `polls_in_flight` (0..128); `phase`, `reason`; boolean `debt`, `quarantined`, `orphaned`, `callbacks_unknown`. Native owner/key data is not exposed. Scope is `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="capture"`, `native_consumption="unobserved"`. Publication counts mean confirmed returned-buffer writes, not gameplay consumption.

Phases are `waiting_neutral`, `capturing`, `releasing` and `complete`. Waiting has no debt/publications; capturing/releasing have cleanup debt. Known held, lost, replaced or partial/unconfirmed output duty stays releasing, reserved and in debt after 1000 ms; quarantine never authorizes timed completion or ACK. After any owned output, completion requires physical rest and a fully confirmed same-stream raw packet-and-Pad handback that ends translation. Cancelling waiting before any owned output is the separate no-output completion case. Complete has no debt but may still have exact callbacks pending. Native ACK also requires the translator inactive, no unknown callbacks and zero matching callbacks.

A complete capture receipt remains readable after passive ACK until capture ACK; these are separate duties. Release/state/ACK inspect prior duty without requiring current focus/menu/lease authority, while owner/client checks still apply.

Callback capacity/counter failure sets `callbacks_unknown=true`, `quarantined=true` and releasing/`debt=true`. The bounded known count is not a total-drain proof. ACK/new admission remain blocked; timeout or VM disappearance does not clear unknown duty. Owner close revokes and marks orphaned records while preserving exact callback/neutral obligations. State DTO corruption is refused as `invalid input capture backend receipt`; inconsistent successful request results are refused as `invalid input capture backend result`.

### Compatibility with older observations

Pass the whole issued sample returned by `lease_read` to methods which require a sample, rather than passing only `sample.epoch` or rebuilding a sample from selected fields. Retain the issued sample when associating work with its epoch. Public diagnostic strings cannot create or replace the opaque epoch, and copying or editing those strings does not refresh an observation.

Raw `sample.pad.buttons` is an integer bitmask. It is not the named boolean table accepted by tracked output, whose axes and triggers also use normalized ranges. Convert those control representations explicitly without replacing the original issued sample used for admission.

Read cleanup from the actual receipt fields: passive `capture_reserved`/`write_reserved` and `source_released`, capture `debt`, writer `output_debt`/`output_released`, and the relevant callback counts/unknown flag. Do not manufacture epoch, terminal or debt fields, substitute a missing field with `false`, or infer completed cleanup from an older receipt shape. Missing or unavailable facts do not authorize release of retained ownership.

## Validation scope

Windows Release builds completed for the runtime, launcher and server plus six relevant test executables. All 62 selected CTest entries passed. The plugin test entry skipped 18 unavailable optional contrib subcases. The Cargo-built Spotify component was disabled; optional asset and shader warnings were reported.

This is not a full configured CTest-suite result. Native hardware and in-game validation, concurrent callback runs, garbage-collection runs and counter-exhaustion runs remain unrun. The selected tests do not establish physical-device identity, gameplay consumption or native cleanup behavior under real controller polling.
