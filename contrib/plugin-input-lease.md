# Passive controller leases

This proposed client API reserves and observes one XInput stream already seen by ReSkate. It does not capture input, change a controller report, suppress native input, control an actor or establish what the game consumed. Availability of the real hook and its observations still needs native validation.

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

A receipt reports decimal-string `sequence`, `requested_ms`, `expires_ms`, `observed_ms`, `source_epoch`, `context_generation`, `last_observation`, `renewals`; numeric `user`, `polls_in_flight` (0..128); `status`, `reason`, `source_released`, `orphaned`. A newly admitted `warming` receipt has zero observation/time/renewal strings. `active` has an issued observation. Neither state reports the source released. `released`, `revoked` and `expired` are terminal; their original stop reason remains available. The reason words and fixed numeric bounds are declared in `Engine/Game/Input/input_lease.h`.

A terminal record may still reserve its source while callbacks which already began are draining. `source_released` becomes true only when `polls_in_flight` is zero. ACK requires terminal state, released source and zero callbacks; it removes that retained receipt. Release, receipt reads and ACK remain available on the original client thread after focus/readiness/menu loss so a plugin can discharge ownership. Other policy/read failures refuse samples and can terminalize the lease. A VM close revokes its touched owner once, marks retained records orphaned and permits only already-started callback drain, never publication into a later lease.

Every sample and receipt labels its scope: `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="observe"`, `native_consumption="unobserved"`. The stream identity is not proof of a physical device or game consumption. This API has no capture methods, `capture_reserved`, report debt, input writes or actor-control capability.

The native service has four source slots, at most eight retained records and at most one live lease; an old callback reservation blocks replacement. Each VM can make at most 64 valid lease-method calls and one request in a normal tick interval. These counters reset in the existing VM tick; startup calls share the first interval. Close cleanup is separate from this call budget. A refused or malformed request cannot be retried indefinitely within an interval.

Release validation on 2026-10-28 built the runtime, launcher and server plus four relevant test executables. All 12 selected CTest entries passed: plugins, nine constructed input-source scenarios, scripted pad and held input. The plugin entry included nine constructed passive-lease Host/VM groups and skipped 18 unavailable contrib-plugin cases. The earlier full ALL-target build with optional validation targets enabled failed and remains unqualified; this is not a full-build or full-CTest-suite pass. Actual native XInput wrapper execution, hook/report preservation, callback drain races and in-game availability remain unrun. Constructed test results grant no capture, native-consumption or gameplay authority.
