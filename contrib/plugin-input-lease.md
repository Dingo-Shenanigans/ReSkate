# Selected-stream controller leases and capture

The six passive methods reserve and observe one XInput stream already seen by ReSkate. They do not change reports or control an actor. A separate permissioned capture extension below can suppress that selected stream with neutral-release cleanup; it does not establish what gameplay consumed. Actual hook/capture execution still needs native validation.

Declare the independent `input.lease` permission in `reskate-plugin.json`. The ordinary `input` permission does not grant these methods, and `input.lease` does not grant key binding or other ordinary input methods. All six methods check permission and client phase before validating arguments or reaching a backend. Server copies refuse them.

| Method | Success | Refusal |
| --- | --- | --- |
| `reskate.input.lease(user)` | Opaque handle for integer XInput user 0..3 | `nil, reason` |
| `reskate.input.lease_read(handle)` | Read-only sample with opaque `epoch` | `nil, reason` |
| `reskate.input.lease_renew(handle, sample)` | `true` | `false, reason` |
| `reskate.input.lease_release(handle)` | `true` | `false, reason` |
| `reskate.input.lease_state(handle)` | Read-only receipt | `nil, reason` |
| `reskate.input.lease_acknowledge(handle)` | `true` | `false, reason` |

Wrong argument types/counts, forged capabilities and permission/client violations raise a Lua error. A missing native backend returns `unavailable`. A handle belongs to one VM instance, including across reloads; it cannot be serialized to another plugin. No method takes a raw address, owner ID or caller-created source identity.

A sample has decimal-string `sequence`, `source_epoch`, `observation_serial`, `observed_ms` and `context_generation`; numeric `user` and `packet`; boolean `neutral`; and a read-only `pad` with `buttons`, `left_trigger`, `right_trigger`, `left_x`, `left_y`, `right_x`, `right_y`. The integer pad fields retain their exact XInput ranges. Every 64-bit field remains a decimal string, including values beyond Lua's exact numeric range.

`sample.epoch` retains the native snapshot issued by that read. Renewal uses that hidden snapshot, not the sample's diagnostic fields. Cloning a sample and changing its public fields cannot manufacture a different observation. The backend requires its exact latest issued sample, no older than 100 ms, and each issued sample can renew once. A renewal grants a 250 ms interval from the current backend time after validating that observation; it does not create a new observation or source identity. The independent 100 ms source freshness guard still applies, so an unchanged stream can expire before the lease deadline.

A receipt reports decimal-string `sequence`, `requested_ms`, `expires_ms`, `observed_ms`, `source_epoch`, `context_generation`, `last_observation`, `renewals`; numeric `user`, `polls_in_flight` (0..128); `status`, `reason`, `source_released`, `orphaned`, `capture_reserved`. A newly admitted `warming` receipt has zero observation/time/renewal strings. `active` has an issued observation. Neither state reports the source released. `released`, `revoked` and `expired` are terminal; their original stop reason remains available. The reason words and fixed numeric bounds are declared in `Engine/Game/Input/input_lease.h`.

A terminal record may still reserve its source while callbacks which already began are draining. `source_released` becomes true only when `polls_in_flight` is zero and `capture_reserved` is false. Capture ownership, neutral-release debt or an outstanding captured callback can retain that reservation. ACK requires terminal state, released source, no capture reservation and zero callbacks; it removes that retained receipt. Release, receipt reads and ACK remain available on the original client thread after focus/readiness/menu loss so a plugin can discharge ownership. Other policy/read failures refuse samples and can terminalize the lease. A VM close revokes its touched owner once, marks retained records orphaned and permits only already-started callback drain, never publication into a later lease.

Every sample and receipt labels its scope: `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="observe"`, `native_consumption="unobserved"`. The stream identity is not proof of a physical device or game consumption. Those six passive methods do not grant capture, report writing or actor control. Their receipt now reports `capture_reserved`; the capture methods below require an additional permission and expose their distinct debt.

The native service has four source slots, at most eight retained records and at most one live lease; an old callback reservation blocks replacement. Each VM can make at most 64 valid calls across all ten selected-stream methods and one passive lease request in a normal tick interval. These counters reset in the existing VM tick; startup calls share the first interval. Close cleanup is separate from this call budget. A refused or malformed request cannot be retried indefinitely within an interval.

## Selected-stream capture extension (source proposal, unrun)

Declare both `input.capture` and `input.lease`. Ordinary `input` and screen `capture` permissions do not grant it. Both permissions and client phase are checked before arguments/backend access. A wrong arity, forged/cross-VM handle or invalid issued epoch raises a Lua error.

| Method | Success | Refusal |
| --- | --- | --- |
| `reskate.input.lease_capture(handle, sample)` | `true` | `false, reason` |
| `reskate.input.lease_capture_state(handle)` | read-only capture receipt | `nil, "unavailable"` |
| `reskate.input.lease_release_capture(handle)` | `true` | `false, "unavailable"` |
| `reskate.input.lease_acknowledge_capture(handle)` | `true` | `false, "unavailable"` |

All use the same opaque passive handle. Admission sends the stored native Snapshot in the plain sample table's issued opaque epoch, rather than rebuilding it from visible fields. A clone carrying that same epoch cannot change the native observation. Capture starts `waiting_neutral`; only a later qualified neutral raw original can publish capture. The ordinary100ms sample freshness/250ms lease bounds remain. The unchanged1000ms release boundary marks unresolved cleanup quarantined and retained; it never permits timed disposal, forced completion, ACK or new admission while duty remains. No argument grants arbitrary input writes or chooses a native key/owner.

Capture state has decimal-string `sequence`, `source_epoch`, `context_generation`, `generation`, `revision`, `expires_ms`, `publications`, `neutral_releases`; numeric `user`0..3 and known `polls_in_flight`0..128; `phase`, `reason`; Boolean `debt`, `quarantined`, `orphaned`, `callbacks_unknown`. Native owner/key data is not exposed. Scope is xinput/observed_stream, mode capture, with physical_device_identity and native_consumption still unobserved. Publication counts mean confirmed returned-buffer writes, not gameplay consumption.

Phases are waiting_neutral/capturing/releasing/complete. Waiting has no debt/publications; capturing/releasing have cleanup debt. Known held, lost, replaced or partial/unconfirmed output duty stays releasing, reserved and in debt after1000ms; quarantine never authorizes timed completion or ACK. Under the current native successor, completion after any owned output requires physical rest and a fully confirmed same-stream raw packet+Pad handback that ends translation. Cancelling waiting before any owned output is the separate no-output completion case. Complete has no debt but may still have exact callbacks pending. Native ACK also requires the translator inactive, no unknown callbacks and zero matching callbacks. The current native producer no longer emits the former lost-source complete+quarantined disposition. A complete capture receipt remains readable after passive ACK until capture ACK; these are separate duties. Release/state/ACK inspect prior duty without requiring current focus/menu/lease authority, while owner/client checks still apply.

Callback capacity/counter failure sets callbacks_unknown=true, quarantined=true and releasing/debt=true. The bounded known count is not a total-drain proof. ACK/new admission remain blocked; timeout or VM disappearance does not clear unknown duty. Existing owner close revokes and marks orphaned records while preserving exact callback/neutral obligations. State DTO corruption is refused as `invalid input capture backend receipt`; inconsistent successful request results are refused as `invalid input capture backend result`.

### Consumer compatibility

The proven private R3 consumer is a separate contract. Source7681/Core9398 already forwards raw.epoch through varargs, so an arity incompatibility alone is not established. The new API requires the whole issued sample table: a reviewed adapter can privately retain the original readonly sample for its exact epoch, never reconstruct it from projections. Raw pad.buttons is an integer bitmask, not the private consumer's named Boolean table. The private Source also expects passive capturing/report_debt/terminal fields that this public DTO does not supply and interprets retained callbacks differently. A separately reviewed Source successor must use actual reservation/debt/drain fields rather than invented false cleanup values. No current R3 package is changed or qualified here; eventual SDK/binary/package version binding remains separate and closed until concrete.

### Validation scope

The new capture bindings/native extension and authored capture Host tests remain UNRUN. Prior passive-only SDK8a3e7cac Release validation on 2026-10-28 built the runtime, launcher and server plus four relevant test executables. All 12 selected CTest entries passed: plugins, nine constructed input-source scenarios, scripted pad and held input. The plugin entry included nine constructed passive-lease Host/VM groups and skipped 18 unavailable contrib-plugin cases. The earlier full ALL-target build with optional validation targets enabled failed and remains unqualified; this is not a full-build or full-CTest-suite pass. Actual native XInput wrapper execution, hook/report preservation, callback drain races and in-game availability remain unrun. Constructed test results grant no capture, native-consumption or gameplay authority.
