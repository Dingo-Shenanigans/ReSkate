# Read-only menu preview pose

`reskate.room.pose(subject)` requires the existing `room` permission. Subject is `"board"` (the default) or `"skater"`.

Returns `nil` when the game menu is closed or a matching preview has not been observed in the last 250 ms. The first request can return nil: requests enable observation for the next 500 ms. Call again on later ticks while using the sample.

A sample contains `position` (vector), `rotation` (quaternion `{x,y,z,w}`), `age` (seconds since observation), and `sequence` (observation counter). A sequence is not a capture frame number and may repeat across plugin ticks. Rotation describes the native menu preview, not a camera-relative front-facing angle. Plugins choose their own facing reference and sampling rules.

Observation is restricted to the existing room bounds and bone layouts for the pinned game build. It reads the evaluated pose; it does not write transforms or take board/skater ownership. It does not change Board Break or material effects. Fit Check uses it to measure complete rotations independently of animated artwork, and chooses a graphic-facing first frame in its plugin.

`room.turn` remains an expiring stick hold. Its existing XInput route now also reaches identified native DualShock 4/DualSense HID reads. The HID path changes only the held axis, preserving the other controls, sensor data, sequence counters and read padding, and refreshing full Bluetooth report checksums. Overlay input capture takes precedence on that path. These native routes require physical-device validation; the report tests do not prove Wine/Proton/GPTK device routing.
