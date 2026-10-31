# Tracked controller output

These client APIs require both `input.write` and `input.lease` in the plugin manifest. They reserve and replace **one observed XInput stream (user 0..3)** through an existing opaque input lease. They do not identify a physical controller, prove that the game consumed input, or control a native actor. `input.capture` grants neutral capture and does not substitute for `input.write`.

The four methods below share ownership with the [passive lease and capture APIs](plugin-input-lease.md). They do not expose a `reskate.input.write` method or PlayStation output, and they do not change the existing `input` permission.

| Method | Result |
| --- | --- |
| `reskate.input.lease_write(handle, sample, pad)` | An opaque `WriteVersion`, or `nil, reason`. Exactly three arguments; `sample` must carry the matching VM-issued opaque observation. Success queues the desired version, without claiming publication. |
| `reskate.input.lease_write_state(handle, version)` | A read-only current-version receipt, or `nil, reason`. Exactly two arguments. A superseded version cannot query a newer version's result. |
| `reskate.input.lease_release_write(handle)` | `true`, or `false, reason`. Exactly one argument; idempotently stops new desired output for this retained writer and requests cleanup. |
| `reskate.input.lease_acknowledge_write(handle)` | `true`, or `false, reason`. Exactly one argument; removes only a positively completed, drained writer. |

All fourteen passive/capture/write methods share the limit of 64 valid input-lease calls per VM tick. Missing permission and server use are refused before inspecting arguments or calling the backend. Handles, issued samples and versions belong to their VM and original lease; public strings or copied diagnostic fields cannot recreate them. A version is allocated before native admission and has no output-producing garbage collector. Passing `nil` to `lease_write` is an error, **not** a release overload. Release and acknowledgement use the original handle even after its observation expires; they do not require a fresh sample.

## Pad values

`pad` is a plain table with these optional fields. Missing controls are neutral. Unknown or nonstring keys, metatables, coercions, NaN and infinities are refused before a backend call.

- `left_x`, `left_y`, `right_x`, `right_y`: finite numbers in `[-1, 1]`.
- `left_trigger`, `right_trigger`: finite numbers in `[0, 1]`.
- `buttons`: a plain table of optional strict booleans named `up`, `down`, `left`, `right`, `start`, `back`, `left_stick`, `right_stick`, `lb`, `rb`, `a`, `b`, `x`, `y`.

Axes are rounded to signed 16-bit values using `round(v * (v < 0 ? 32768 : 32767))`; triggers use `round(v * 255)`. Values outside the stated range are rejected, not clipped. The fixed Pad is copied before entering the native publication gate. No caller table is retained by native code. The write table's normalized controls and named button booleans differ from the raw XInput integers in a `lease_read` sample.

## Desired versions and receipts

A desired version lasts 100 ms; renewing the separate 250 ms passive lease does not renew the desired Pad. An identical Pad is still a new version. A new version is accepted only while authority remains live and the preceding desired version has a confirmed publication. Before then it refuses as busy without replacing or extending that version. Once cleanup starts, another write cannot revive the retained writer. Keep the newest successfully returned version.

A read-only writer receipt contains the original `sequence`, `user`, `source_epoch` and `context_generation`; writer `generation` and queried `revision`; `desired_revision`, `published_revision`, `write_expires_ms`, `source_expires_ms`, `version_publications`, total `publications`, `cleanup_publications`, `handback_publications`; `phase`, `reason`, `last_written_kind`, `output_debt`, `output_released`, `quarantined`, `orphaned`, `polls_in_flight` and `callbacks_unknown`. Every 64-bit value is a canonical decimal string. Scope is `transport="xinput"`, `source_identity="observed_stream"`, `physical_device_identity="unobserved"`, `mode="write"`, `native_consumption="unobserved"`.

`phase` is `pending`, `active`, `releasing` or `complete`; `last_written_kind` is `none`, `desired`, `cleanup` or `handback`. Pending means the queried current version has no confirmed desired copy, even if older output debt exists. Active means that version has a confirmed desired copy. Only the exact version's `version_publications` proves its output-buffer publication. A queued version, storage success, physically equal input or another filter's output is not such a witness.

## Cleanup and owner close

Neutral capture and tracked output share one reservation; neither can adopt the other's state, release or acknowledgement. The passive receipt reports `write_reserved`, retained until writer acknowledgement. A terminal passive `source_released` requires no callbacks and neither `capture_reserved` nor `write_reserved`; passive acknowledgement cannot erase either duty. Unload, reload, limits and owner close stop assertions through the owner-release path while preserving unresolved cleanup.

Release, expiry, menu/focus/context/source changes and higher-priority input owners stop desired assertions. They never clear debt by timeout. The 1000 ms cleanup boundary records quarantine/retained duty rather than forcing completion. After any attempted owned write, completion needs actual full, confirmed raw packet-and-Pad handback on the same clean stream, inactive translation and known callback drain. Lost/replaced streams, partial writes, unknown callback capacity and held controls retain cleanup; a watchdog cannot acknowledge them. Cancellation before any attempted output can complete separately once no admitted callback can still publish. Completion does not imply the desired version ever published.

A plugin should publish its final desired neutral version, observe that version's actual receipt, release the writer, wait for `output_released`, acknowledge the writer, and then release/acknowledge the passive lease. Output publication and controller handback remain separate from any game action or equipment transition a plugin observes afterward.

Plugins using older observation or receipt shapes must retain the whole issued sample and use the current reservation, debt and callback fields. Raw sample button integers are not write-table button booleans. Do not invent epochs or missing cleanup fields, and do not treat absent fields as proof of released output. See [compatibility guidance](plugin-input-lease.md#compatibility-with-older-observations).

## Validation scope

Windows Release builds completed for the runtime, launcher and server plus six relevant test executables. All 62 selected CTest entries passed; the plugin test entry skipped 18 unavailable optional contrib subcases. The Cargo-built Spotify component was disabled, and optional asset/shader warnings were reported.

The full configured CTest suite was not run. Native hardware and in-game validation, concurrent callback runs, garbage-collection runs and counter-exhaustion runs remain unrun. These results do not prove physical-device identity, game consumption or controller handback under real polling.
