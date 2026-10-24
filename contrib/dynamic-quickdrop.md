# Dynamic QuickDrop API port — draft implementation

**Not ready to merge or replace the working combined 2.1.2 mod.** The author has
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

## Deliberately incomplete

The 2.1.2 implementation's moving grind-surface queries, edge/chain continuity,
relative grind friction, grounded-walking pressure/impact policy, native dropper
selection/preview/commit adapters, RB hold targeting and native marker prompts
are **not ported**. Those need their own generic primitives and native regression
coverage before this is a parity replacement. The example excludes names that
look like rails/ledges/ramps and uses a fixed 25 kg demonstration mass, not the
original's authored-mass/classification policy. This filter is not a guarantee
that every remaining prop supports dynamic motion correctly.

Physics is **off by default on every plugin load**. Enable it explicitly in the
example's plugin page for isolated offline testing only. Native APIs refuse
multiplayer sessions and relinquish claims while the ReSkate menu, a park-editor
transaction, a layout clear or a level transition is active. Native dropper
editing while a physics claim is active is not supported yet: disable the example
before opening or manipulating a native object card/dropper. Do not test this
draft on a valuable saved layout. No runtime is installed automatically.

Favorites are an example plugin page, not the original integrated native catalog
category. Existing profile favorites and marker pins are not imported, overwritten
or granted entitlements. Placing a favorite goes through `objects.place`, with its
normal paid-item refusal, session policy, limits and edit-rate gate.

## API

Only manifests with `"side": "client"` may use these APIs (not `both` or `server`).
IDs are decimal strings from `reskate.objects.list()`; numeric IDs, zero, signs,
overflow, embedded NULs and trailing text are refused.

```lua
local ok, why = reskate.objects.physics(id, {dynamic = true, mass = 25})
local motion = reskate.objects.motion(id)
-- motion = {position=vector, rotation={x,y,z,w}, velocity=vector,
--           dynamic=boolean, generation=opaque_string}, or nil
reskate.objects.velocity(id, vector.create(1, 0, 0))
reskate.objects.pose(id, motion.position, motion.rotation)
reskate.objects.physics(id, nil) -- release this plugin's claim

local events, overflow = reskate.marker.events()
-- {"saved", "returned", ...}, boolean; or nil, "unavailable here"
```

Mass is finite, 1–10000 kg; speed is a finite magnitude at most 45 m/s, not 45
per axis. Pose positions are finite within 100 km and rotations are near-unit
quaternions (squared norm strictly between .95 and 1.05). Setting a pose stops
linear/angular motion. Velocity writes are at most one per object per 16 ms;
pose writes one per object per 250 ms; changing an existing configuration is at
most once per 16 ms (identical requests do not rewrite native state). Native failures return `false, reason`;
bad Lua types/ranges raise an argument error. Sampling returns nil when a claim
or native body is unavailable. Pose application is not transactional across the
two realms: a native failure after the client update can require a retry.

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
`placement_physics.h`; hooks use the SDK's Detours wrapper and grouped install.

## Build and regression testing

Copy `contrib/examples/dynamic-quickdrop` into `Mods/Dynamic_QuickDrop` only in an
isolated development installation built from this branch. Do not combine it with
the old custom DLL's physics hooks.

Build `dingosdk_runtime` and `dingosdk_plugin_tests` with
`DINGOSDK_BUILD_PLUGIN_TESTS=ON`. Host tests cover permission denial, ID parsing,
finite/range/type checks, quaternion validation, claim exclusivity and quotas,
server-side refusal, marker cursor isolation/overflow/reset, unload release, and
the example's opt-in, pin restore/exclusion/replacement and Favorites.
These use a fake provider: they do **not** verify native body writes, hooks,
realm synchronization, native editing, physics contacts or in-game UI.

Local validation completed: both Windows Release targets built successfully;
the plugin test executable passed, with 15 absent optional contributed plugins
skipped. The new draft example and its new API/policy tests ran, not skipped.

Before leaving draft: port the missing native adapters and gameplay policies,
test save/return after rotation/tumbling, inclusion/exclusion, replacement/delete,
layout/map changes, disable/reload, pause/editing, contacts and moving grinds,
and verify multiplayer continues to fail closed. The Live Controller contribution
is independent; neither branch requires the other.
