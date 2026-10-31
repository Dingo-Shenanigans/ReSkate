# A world ray that says what it found: `reskate.world.probe`

`reskate.world.probe(from, to)` needs the `world.raycast` permission. Both arguments are Luau
vectors in world metres; there are no native handles, ignored bodies or entity arguments.

The result is a table with one of three statuses:

| `status` | Meaning | Other fields |
|---|---|---|
| `unavailable` | This call did not establish a valid query result. | none |
| `clear` | The query ran and the segment met no collision. | none |
| `hit` | The query ran and found a valid closest hit. | `position` (vector), `fraction` (0 to 1) |

```lua
local result = reskate.world.probe(from, to)
if result.status == "hit" then
    reskate.log(tostring(result.position) .. " at " .. tostring(result.fraction))
elseif result.status == "unavailable" then
    -- Defer anything that needs real collision evidence.
end
```

There is no normal or surface velocity, and no world epoch, owner, entity, body or native address.
A clear ray does not show that collision for the whole surrounding region has streamed in.

Both positions must be finite and within 100 km of the map centre, and the segment at most 1000 m
long under the default limits; anything else is a Lua error. A degenerate segment (squared length
below 1e-10 m^2) returns `unavailable` without calling the game, but still counts as a raycast.

Each call is one of the plugin's raycasts, which it shares with `world.raycast`, `world.pick`,
`session.raycast` and [owned-body ray batches](plugin-body-query.md): 64 a tick under the default
limits, past which the call is a Lua error. Separately, `world.probe` and the ray batches share a
hard limit of 64 actual native casts a tick, failed casts included; once that is spent this call
returns `unavailable` without casting. The game's ray makes at most one cast per request, so in
the game the 64 raycasts run out first, and the native limit bounds the backend rather than the
plugin. `world.raycast`, `world.pick` and `session.raycast` do not count against it.

Only a finite hit within the map, with its fraction in [0, 1] and its position on the segment at
that fraction within 25 mm on each axis, is returned. Anything malformed is `unavailable`, never a
clear ray. The 25 mm checks the result's shape; it is not a claim about collision accuracy.

In the game the ray is the park editor's native ray, cast once through `cast_surface_counted`
(`probe_local_surface_counted` in `Extension/Profile/local_profile_objects.cpp`) on the client
thread under the profile's native mutex, with the current client context and world resolved on
each call. It sends the camera filter that `world.raycast` sends, makes one native cast, looks up
no owner, ignores nothing and may hit any collision the native query tests.

## The rays plugins already had are unchanged

`world.raycast`, `world.pick` and `session.raycast` keep their own code paths
(`local_surface_hit` and `local_world_pick`) and return what they always did: `nil` still covers
both a clear ray and a query that could not run, which is what `world.probe` tells apart. They are
not counted against the native cast limit.

The counted cast's stricter rules apply only to a caller that asks for its typed result
(`world.probe` and the ray batches): a non-finite, out-of-map or zero-length segment is refused
before the native call, and a malformed fraction makes the whole result unknown instead of being
skipped. The park editor's own casts, and so its placement and picking, the trainer's ground
checks, `world.entities` and the rays above, keep the rules they had.

## Tests

`plugins_distinguish_clear_unavailable_and_hit_world_probes` in the plugin tests covers the
permission, the three statuses, malformed hits, bad arguments and the shared raycast limit against
a constructed game, and `owned_body_batches_share_actual_and_logical_query_limits` shows
`world.raycast`, `world.pick` and `session.raycast` answering as before once the native casts are
spent. `Extension/Objects/ParkEditor/Test/surface_probe_tests.cpp` covers the native ray's result
handling, release of its native allocation, the park editor's exclusions and its own rules, and
the counted cast, over constructed results. Neither runs the game's ray.
