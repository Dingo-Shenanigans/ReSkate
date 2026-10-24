# Dynamic QuickDrop API port

**In-game regression testing remains required before installing this over the working mod.** The author has
verified the original Live Controller and Dynamic QuickDrop in game. This new
API/plugin refactor has not been verified in game. Keep the original installation
and its sources unchanged while reviewing this contribution.

## Implemented here

- Client-only `objects.physics` permission: bounded, exclusive claims on existing
  local placed objects, dynamic/fixed motion, mass, bounded velocity, live rigid
  pose sampling, and pose restoration without losing quaternion rotation.
- `marker.read`: poll accepted native marker saves/returns, filtered to the local
  skater. No input-binding guesses, raw entities, native handles or memory API.
- A Luau example with session-local include/exclude pins, native-marker snapshot
  save/return, and file-backed Favorites using the existing catalog/placement API.
- Native client/server replica identity validation, process-wide claim arbitration,
  row lifetime/map-generation checks, bounded event history and unload cleanup.
- Native moving-surface query views, transformed authored edges/chain continuity,
  and contact-point-relative grind friction for opted-in local bodies only.
- Native dropper selection samples the current physics pose, freezes selected
  bodies, consumes preview/commit/cancel transforms, and hands control back to the
  solver after editing. Edits are consumed before authoritative pose mirroring.
- The original mass/classification and time-scaled impact/pressure rules in a Lua
  policy module; short native collision rays confirm actual body contact.
- Stable hold-RB/R1 targeting with one toggle per deliberate 600 ms hold, reset on
  target/device/focus changes, plus a permission-gated overlay prompt/progress.

## Operation and scope

Rails, ledges and ramps are no longer excluded. Placeholder 1 kg donor masses are
replaced with the original's item-based estimates; authored non-placeholder masses
are respected. The SDK does not contain QuickDrop's classifications or force policy.
Moving surface support is an explicit generic per-claim option and can be disabled
in the plugin page. The solver's real motion type is unchanged by query adapters:
only the original allowlisted surface/material call sites see a different view.
Cached grind geometry validates shape generation and preserves native identities.
Borrowed chain allocations stay stable for the process lifetime, bounded to 65536
retained edges; exhaustion falls back to native queries rather than freeing a
pointer a contact may still hold.

Physics is **off by default on every plugin load**. Enable it explicitly in the
example's plugin page in an isolated offline development installation. Native APIs
refuse multiplayer sessions and relinquish claims on layout clear/level changes.
The ReSkate menu and park editor temporarily freeze claimed objects; native
dropper edits use the handoff above instead of requiring the plugin to be disabled.
Committed edits update marker snapshots without changing opt-outs. Marker recall
retries are bounded to three seconds; they never replay indefinitely after an
editing conflict. Do not use a valuable saved layout for first-run validation.
No runtime is installed automatically.

Favorites are an example plugin page, not the original integrated native catalog
category. Existing profile favorites and marker pins are not imported, overwritten
or granted entitlements. Placing a favorite goes through `objects.place`, with its
normal paid-item refusal, session policy, limits and edit-rate gate.

## API

Only manifests with `"side": "client"` may use these APIs (not `both` or `server`).
IDs are decimal strings from `reskate.objects.list()`; numeric IDs, zero, signs,
overflow, embedded NULs and trailing text are refused.

```lua
local ok, why = reskate.objects.physics(id, {dynamic = true, mass = 25, surface = true})
local motion = reskate.objects.motion(id)
-- motion = {position=vector, rotation={x,y,z,w}, velocity=vector,
--           dynamic=boolean, generation=opaque_string, editing=boolean,
--           edit_revision=opaque_string, mass=number, authored_mass=number,
--           surface=boolean}, or nil
reskate.objects.velocity(id, vector.create(1, 0, 0))
reskate.objects.pose(id, motion.position, motion.rotation)
reskate.objects.physics(id, nil) -- release this plugin's claim
local hit = reskate.objects.contact(from, to) -- {id=string, position=vector}, or nil
local actor = reskate.objects.actor() -- also needs skater.read AND input
-- {velocity, walking_velocity, wish, grounded, on_foot, active, device, physics_state}

local events, overflow = reskate.marker.events()
-- {"saved", "returned", ...}, boolean; or nil, "unavailable here"
```

Mass is finite, 0.1–20000 kg; speed is a finite magnitude at most 45 m/s, not 45
per axis. Pose positions are finite within 100 km and rotations are near-unit
quaternions (squared norm strictly between .95 and 1.05). Setting a pose stops
linear/angular motion. Velocity writes are at most one per object per 16 ms;
pose writes one per object per 250 ms; changing an existing configuration is at
most once per 16 ms (identical requests do not rewrite native state). Native failures return `false, reason`;
bad Lua types/ranges raise an argument error. Sampling returns nil when a claim
or native body is unavailable. Pose application is not transactional across the
two realms: a native failure after the client update can require a retry.
Pose/velocity writes refuse active native editing. Contact rays are at most 10 m
and share the host's existing 64-ray/tick budget. They expose only this plugin's
claims, and never see through a blocking world hit. The actor sampler is read-only,
validates native walking ownership/state, reads movement intent without consuming
input, and reports inactive during menu/editing/free-camera/focus loss. It exposes
neither skater pointers nor native controller handles. `device` is a controller
index used only to invalidate a hold across device changes.

Claims are limited to 32 objects/plugin and 128/process. Only initially fixed
local bodies may be claimed; an occupied claim cannot be stolen, released,
sampled, moved or accelerated by another plugin. Release restores the original
inverse mass and fixed motion, at the current physics pose. The authoritative
copy follows the validated client pose while claimed; stale samples are ignored.
Layout ID, row token, map generation, replica identity and body generation are
revalidated before use. The opaque motion generation allows plugins to discard
snapshots when a layout ID refers to a replacement object.

Marker consumers have independent cursors (up to 128). The bounded history holds
32 operations. A first poll subscribes without replaying old operations; a slow
consumer is told `overflow=true`, so it can discard stale actions. Unload removes
the consumer; a level transition clears pending operations. Only the accepted
return-to-saved-marker transition produces `returned`, not arbitrary teleports.
Hook targets, prefixes and memory-layout offsets live in the supported build's
`placement_physics.h` and `placement_adapters.h`; hooks use the SDK's Detours wrapper
and grouped install. Empty or released allowlists make installed hooks inert.

## Build and regression testing

Copy `contrib/examples/dynamic-quickdrop` into `Mods/Dynamic_QuickDrop` only in an
isolated development installation built from this branch. Do not combine it with
the old custom DLL's physics hooks.

Build `dingosdk_runtime` and `dingosdk_plugin_tests` with
`DINGOSDK_BUILD_PLUGIN_TESTS=ON`. Host tests cover permission denial, ID parsing,
finite/range/type checks, quaternion validation, claim exclusivity and quotas,
server-side refusal, marker cursor isolation/overflow/reset, unload release, and
the example's opt-in, pin restore/exclusion/replacement/edit-commit, Favorites,
authored mass, RB holds and contact impulse/pressure. Pure geometry tests exercise
edge transforms, overlap bounds and preservation of authored identity/metadata.
These use a fake provider: they do **not** verify native body writes, hooks,
realm synchronization, native editing, physics contacts or in-game UI.

Local validation completed: both Windows Release targets built successfully;
the plugin test executable passed, with 15 absent optional contributed plugins
skipped. The new example and its API/policy tests ran, not skipped. A read-only
check against the local supported executable verified 14 native prefixes and 24
call sites. Reproduce it without installing or extracting game data:

```powershell
python contrib/examples/dynamic-quickdrop/verify_contracts.py 'C:\Games\ReSkate-1.0.0\Skate.exe'
```

In-game regression checklist: save/return after rotation/tumbling,
inclusion/exclusion, replacement/delete,
layout/map changes, disable/reload, pause/editing, contacts and moving grinds,
and multiplayer must continue to fail closed. Native selection/translation/group
movement/rotation/commit/cancel and walking/boarding/grind contacts need explicit
play testing; the executable byte check cannot establish those behaviors.
The original mod's verification must not be presented as verification of this port.
The Live Controller contribution
is independent; neither branch requires the other.
