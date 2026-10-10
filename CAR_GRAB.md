# Experimental car grab

Adds offline car towing, a grab-status HUD, and procedural arm/hand contact to
ReSkate. Hold **G / RB / R1** near a traffic car; steer with **A/D / left stick**.
Release the grab button to let go. Braking, jumping, lost focus, world changes,
or invalid vehicle data also release the grip. Multiplayer disables the system.

**Status:** development handoff, not a finished gameplay feature. Earlier playtests
reported missing hand contact, unexpected releases, and sideways motion on turns.
The latest corrections still need in-game validation; hand posing requires a
verified reachable collision contact and can be absent while towing remains active.

Targets Steam build **25414733**. Integrated against ReSkate `main` at
`6a02550c7edfd2099a3bd5f2054992c34cb5ef6b` (2026-10-10).

## Files

| Files | Purpose |
| --- | --- |
| `Extension/Skater/CarGrabCore/` (12 files) | Attachment, input continuity, vehicle samples, lifecycle tracking, steering and pose math. |
| `Extension/Skater/car_grab_runtime.{h,cpp}` | Guarded local-client/physics bridge and input handling. |
| `Extension/Skater/traffic_vehicle_provider.{h,cpp}` | Traffic discovery, copied snapshots and lifetime validation. |
| `Extension/Skater/traffic_vehicle_surface.{h,cpp}`, `traffic_vehicle_surface_math.h` | Collision ownership, bumper contact and geometry. |
| `Extension/Skater/car_grab_reach.{h,cpp}` | Procedural arm/hand posing after native animation evaluation. |
| `Extension/UI/Overlay/car_grab_hud.{h,cpp}` | Status label and contact marker. |
| `Runtime/client_tick.cpp`, `Extension/Skater/client_noclip.cpp` | Client readiness and existing guarded physics callback. |
| `Extension/Multiplayer/Remote/native_skater.cpp` | Evaluated-animation callback. |
| `Extension/Profile/local_profile_runtime.cpp`, `Extension/World/local_{park_rotation,population_controls,world_layers}.cpp` | Provider startup, park sampling and teardown invalidation. |
| `Extension/UI/Overlay/overlay_{input,render}.cpp`, `playstation_input.cpp` | Input/HUD wiring and Sony report timestamp-underflow fix. |
| `cmake/Runtime.cmake` | Runtime sources and core include directory. |

Build with the existing Windows MSVC v143 prerequisites:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release --target dingosdk_runtime --parallel 4
```

Native access stays in validated callbacks; physics and pose updates recheck
vehicle lifetime and freshness. Preserve those checks when tuning handling.
No new libraries or game assets are required. This is an independent implementation;
no skate3clone code or assets were copied. Changes dated 2026-10-10 are distributed
under ReSkate's existing GPL-3.0 license; see `LICENSE`.
