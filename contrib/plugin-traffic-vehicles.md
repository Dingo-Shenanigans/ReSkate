# Taking a traffic vehicle from a plugin

Declare `world.vehicles` in `reskate-plugin.json`. A plugin can hold at most four traffic vehicles.
`take(from, to)` casts the same ray as `reskate.world.pick`, counted against the same per-tick
raycast limit. It returns a handle, or `nil, reason` when nothing is hit, the first hit is not
a traffic vehicle, the game interface is unavailable, or the limit is reached.

```lua
local vehicle, why = reskate.world.vehicles.take(from, to)
if vehicle then
    -- The first updates resolve the collision owner's parent tree and nearby traffic agent.
    if vehicle:state() == "held" then
        local position, rotation = vehicle:pose()
        vehicle:set_pose(position, rotation)
    end
end
```

`state()` returns `"finding"`, `"held"`, `"lost"`, or `"released"`; a lost handle also returns its
reason. `pose()` returns a position vector and a quaternion table with `x`, `y`, `z`, `w` keys and
array entries while held, or `nil` before then. `set_pose(position, rotation)` normalizes a finite,
nonzero quaternion and applies it on the next hierarchy updates. `release()` stops both writes.
Unloading or disabling the plugin also releases every one of its vehicles.

The game continues advancing a traffic agent's lane progress while held. On release, it can jump
back to that progress, the car can race after it, and the game can remove it within seconds. There
is no lane-progress correction in this API.
