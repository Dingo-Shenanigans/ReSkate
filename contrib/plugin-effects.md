# Plugin particles and decals

The client `reskate.effects` API renders private mod assets. Declare the `effects` permission
and list each allowed asset in the manifest's `effects` array (at most 128 names).
Every name must also be covered by a valid `resident` root in the shared
`win32/levels/game/dingolevel_root/dingolevel_root` bundle:

```json
{
  "api": 1,
  "side": "client",
  "entry": "plugin/main.lua",
  "permissions": ["effects"],
  "effects": ["effects/reskate/example/ebp_spark", "world/materials/decal/reskate_example/stain_dv"],
  "resident": [{
    "bundle": "win32/levels/game/dingolevel_root/dingolevel_root",
    "roots": ["effects/reskate/example/", "world/materials/decal/reskate_example/"]
  }]
}
```

Particle names must start with `effects/reskate/`; decal names with
`world/materials/decal/reskate_`. Names are lowercase letters, digits, slashes, underscores
and hyphens, without empty path components. Stock/store asset names are refused. The native
bridge also checks the loaded asset class: `EffectBlueprint` for particles and
`EnvironmentDecalVolumeTemplateData` for decals. A declared name alone does not load content.
Compile the effect assets and their dependencies into that shared root bundle. The native
bridge only resolves effects in the level-wide Game compartment and checks the returned asset's
own compartment before creating particles or decals. Assets in streamed sublevels are refused,
even when a manifest claims that their names are resident; declaring a root does not load or pin
its bundle. The global effect service must not retain references to streamed blueprint data.

The manifest's `resident` declarations preserve the roots and shaders when another mod replaces
the shared bundle. Invalid declarations reject the plugin, including malformed shader entries
that would make the merge discard the roots. For example:

```json
"resident": [{
  "bundle": "win32/levels/game/dingolevel_root/dingolevel_root",
  "roots": ["effects/reskate/example/", "world/materials/decal/reskate_example/"],
  "shaders": [{
    "name": "world/materials/decal/reskate_example/shaderblockdepot_splash",
    "roots": ["world/materials/decal/reskate_example/"]
  }]
}]
```

Use the bundle and shader depot names produced when compiling your own assets. The `effects`
allowlist contains exact names; `resident.roots` also accepts prefixes ending in `/`. These
declarations reuse the normal mod merge; they do not create assets or compile shaders at runtime.

## Calls

| Call | Result |
|---|---|
| `particle(asset, transform)` | Opaque numeric handle, or `nil, reason` when unavailable or full |
| `decal(asset, transform, {attach = false, lift = 0})` | Handle, or `nil, reason` |
| `move(handle, transform)` | Move a particle or an unattached decal |
| `stop(handle)` | Stop particle emission while existing particles finish |
| `opacity(handle, alpha)` | Set decal opacity, 0–1 |
| `release(handle)` | Stop and release one owned effect |
| `clear()` | Release all this plugin's effects; safe to repeat |
| `state()` | `ready, epoch`, describing the current native world |

A transform is 16 numbers: three world-space basis vectors with a zero fourth component,
then world position with a fourth component of one. The axes must be orthogonal and have
length .001–20 metres; positions must be within ±100,000 metres. Particle emission points
along its Y axis. Decal Y gives projection depth, X/Z its footprint. A decal with `attach=true`
queries collision around its center, along Y; `lift` (0–1 metre) is the center's offset from
the contact surface. A missed query returns nil. Bound decals follow the receiver's rigid
pose and disappear when its generation-bearing physics handle becomes invalid.

Handles belong to one plugin and are never native addresses. Bad arguments, undeclared assets
and foreign/released handles raise Lua errors. Each plugin can hold 24 particles and 2,048
decals and make 8,192 effect calls per tick. The native service caps all plugins together at
64 particles, 4,096 decals and 512 cached asset names. Native effects also expire after ten
minutes of real time as a cleanup limit; plugins should release them sooner. Expired, detached
and reset effects are pruned from the VM's handle table automatically and return their quota.

Decals add their current live count to the renderer's existing limits, reconciled at most once
per second. Creation can therefore take up to a second to receive additional draw capacity.
Clearing and recreating decals in a callback does not repeatedly resize render buffers. The
allowance uses the in-memory SystemSettings service, separate from saved profile options;
only its own changes are restored, and a later external settings edit is preserved.

Rendering is unavailable without a local skater, during loading, replay, Noclip or park
editing, or when any required native byte check fails. Check `state()` before rendering and
call `clear()` when its epoch changes, then discard local handles and simulation history.
World resets automatically release native references. Expired handles become invalid once
pruned; operations on them raise the same argument error as released handles. Unload, disable and
VM failure release all effects. Effects are local and do not replicate.
The dedicated server has no renderer: `state()` returns false and creation returns nil.

## Physics geometry

`events.physics` keeps its existing timing/contact fields. Also declare
`events.physics.geometry` to receive the following additions to `physics_step`:

- `state`: optional local physics selector state.
- `bodies[name].point` and `.normal`: optional world-space contact geometry. The normal
  accompanies the selected peak impact speed; unknown geometry is absent, not zero.
- `poses[name].transform` and `.velocity`: optional world-space rigid body matrix (16 numbers)
  and linear velocity (three numbers, metres/second), including airborne bodies.

Plugins written before the geometry permission was introduced must add it to their
manifest alongside `events.physics`. Without it, physics events still arrive but have
no poses, contact points or normals, so effects that require those fields cannot detect impacts.

The host constructs the extra geometry JSON only while a plugin requests that permission and
delivers it only to those plugins. These values are copied on the physics thread at the existing contact/skeleton boundary.
Queued samples from another local entity/rig or a level transition are discarded. Missing
fields mean unavailable data; do not synthesize a collision from them. The event's `at` is
in the same milliseconds clock as `reskate.time.now()` and `seconds` is simulation time.
