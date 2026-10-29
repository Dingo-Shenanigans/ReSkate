# Tracked controller output

These client APIs require both `input.write` and `input.lease` in the plugin manifest. They reserve and replace **one observed XInput stream (user 0..3)** through an existing opaque input lease. They do not identify a physical controller, prove that the game consumed input, or control a native actor. `input.capture` grants neutral capture rather than arbitrary Pad output and does not substitute for `input.write`.

This change adds only the four methods below. It does not install `reskate.input.write`, add PlayStation output, import a separate controller-output manager, or change the existing `input` permission.

| Method | Result |
|---|---|
| `reskate.input.lease_write(handle, sample, pad)` | An opaque `WriteVersion`, or `nil, reason`. Exactly three arguments; `sample` must carry the matching VM-issued opaque observation. Success queues the desired version, without claiming publication. |
| `reskate.input.lease_write_state(handle, version)` | A readonly current-version receipt, or `nil, reason`. Exactly two arguments. A superseded version cannot query a newer version's result. |
| `reskate.input.lease_release_write(handle)` | `true`, or `false, reason`. Exactly one argument; idempotently stops new desired output for this retained writer and requests cleanup. |
| `reskate.input.lease_acknowledge_write(handle)` | `true`, or `false, reason`. Exactly one argument; removes only a positively completed, drained writer. |

All four share the existing limit of 64 input-lease calls per tick. Missing permission and server use are refused before inspecting arguments or calling the backend. Handles, issued samples and versions belong to their VM and original lease; public strings or copied diagnostic fields cannot recreate them. A version is allocated before native admission and has no output-producing garbage collector. Passing `nil` to `lease_write` is an error, **not** a release overload. Release and acknowledgement use the original handle even after its observation expires; they do not require a fresh sample.

## Pad values

`pad` is a plain table with these optional fields. Missing controls are neutral. Unknown or nonstring keys, metatables, coercions, NaN and infinities are refused before a backend call.

- `left_x`, `left_y`, `right_x`, `right_y`: finite numbers in `[-1, 1]`.
- `left_trigger`, `right_trigger`: finite numbers in `[0, 1]`.
- `buttons`: a plain table of optional strict Booleans named `up`, `down`, `left`, `right`, `start`, `back`, `left_stick`, `right_stick`, `lb`, `rb`, `a`, `b`, `x`, `y`.

Axes are rounded to signed 16-bit values using `round(v * (v < 0 ? 32768 : 32767))`; triggers use `round(v * 255)`. Values outside the stated range are rejected, not clipped. The fixed Pad is copied before entering the native publication gate. No caller table is retained by native code.

## Desired versions and receipts

A desired version lasts 100 ms; renewing the separate 250 ms passive lease does not renew the desired Pad. An identical Pad is still a new version. A new version is accepted only while authority remains live and the preceding desired version has a confirmed publication. Before then it refuses as busy without replacing or extending that version. Once cleanup starts, another write cannot revive the retained writer. Keep the newest successfully returned version.

A readonly writer receipt contains the original `sequence`, `user`, `source_epoch` and `context_generation`; writer `generation` and queried `revision`; `desired_revision`, `published_revision`, `write_expires_ms`, `source_expires_ms`, `version_publications`, total `publications`, `cleanup_publications`, `handback_publications`; `phase`, `reason`, `last_written_kind`, `output_debt`, `output_released`, `quarantined`, `orphaned`, `polls_in_flight` and `callbacks_unknown`. Every 64-bit value is a canonical decimal string. Scope is `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="write"`, `native_consumption="unobserved"`.

`phase` is `pending`, `active`, `releasing` or `complete`; `last_written_kind` is `none`, `desired`, `cleanup` or `handback`. Pending means the queried current version has no confirmed desired copy, even if older output debt exists. Active means that version has a confirmed desired copy. Only the exact version's `version_publications` proves its output-buffer publication. A queued version, storage success, physically equal input or another filter's output is not such a witness.

## Cleanup and owner close

Neutral capture and tracked output share one reservation; neither can adopt the other's state, release or acknowledgement. The passive receipt adds `write_reserved`, retained until writer acknowledgement. A terminal passive `source_released` requires no callbacks and neither `capture_reserved` nor `write_reserved`; passive acknowledgement cannot erase either duty. Unload, reload, limits and owner close stop assertions through the existing owner-release path while preserving unresolved cleanup.

Release, expiry, menu/focus/context/source changes and existing higher-priority input owners stop desired assertions. They never clear debt by timeout. The existing 1000 ms cleanup boundary records quarantine/retained duty rather than forcing completion. After any attempted owned write, completion needs actual full, confirmed raw packet-and-Pad handback on the same clean stream, inactive translation and known callback drain. Lost/replaced streams, partial writes, unknown callback capacity and held controls retain cleanup; a watchdog cannot acknowledge them. Cancellation before any attempted output can complete separately once no admitted callback can still publish. Completion does not imply the desired version ever published.

The plugin should publish its final desired neutral version, observe its actual version receipt, release, wait for `output_released`, acknowledge the writer, and then release/acknowledge the passive lease. Equipment completion, stock settling, native actor lifetime and BMX behavior belong to the Lua consumer and require separate observation.

This is a new explicit consumer contract. The existing private R3 bike is unchanged: forwarding an issued epoch through its varargs is not alone an incompatibility, but its Pad projection and passive/capture cleanup receipt expectations differ. No legacy cleanup aliases or runtime-version proof are fabricated here.

Validation status: source authored only. No build, Host test, actual wrapper, controller, game, performance or physical-device qualification is claimed by this document.
