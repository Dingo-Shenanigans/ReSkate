# Controller layers for Luau plugins

These API 1 additions let any plugin implement control remapping without a companion DLL. Control-scheme decisions remain in Luau.

## Physical controller input and output

**reskate.input.gamepad(controller?)** needs **input**. No argument keeps the existing combined-controller behavior. An integer 0..3 reads one physical XInput controller; "playstation" reads the existing single native PlayStation controller path. Nil means unavailable. Host replacements are bypassed by physical reads.

**reskate.input.write(pad, controller?)** needs the separate **input.write** permission. Default controller: 0. It publishes a complete replacement for that real controller. Missing controls are neutral. Fields left_x, left_y, right_x and right_y are finite numbers in -1..1; left_trigger and right_trigger are in 0..1. The buttons table has strict booleans named up, down, left, right, start, back, left_stick, right_stick, lb, rb, a, b, x and y. Values are copied from the VM. Passing nil releases this caller's replacement. Returns true, or false and a reason.

One plugin owns each controller at a time. Renew every tick: replacements expire after 100 ms. Another plugin cannot steal or release a live lease. Unload, reload and limit stops release leases. The input path invalidates all replacements when focus or menu/cursor ownership prevents gameplay. Existing input-hiding filters retain priority. Only successful physical reads are replaced; this does not invent a connected controller. PlayStation output has HID axis precision and preserves system, sequence, sensor and touch data. Changed Bluetooth reports receive fresh CRCs.

    reskate.on("tick", function()
        if not reskate.input.allowed() then reskate.input.write(nil, 0); return end
        local pad = reskate.input.gamepad(0)
        if not pad then reskate.input.write(nil, 0); return end
        pad.left_x = -pad.left_x
        reskate.input.write(pad, 0)
    end)

No Luau callback runs on an input thread. Physical sampling and publishing happen on plugin ticks; rapid changes between ticks can be missed. Hardware latency and slow-frame behavior require game testing.

## Local rider observations

All calls below need **skater.read** and expose no native addresses.

- **reskate.skater.frame()** returns nil or a table with serial, position, forward, backwards, mirrored, fakie and basis. These are native observations, not inferred anatomical foot roles. Serial is an opaque local owner identity for equality checks. Source and exported flags must agree, and the live local owner is resolved before and after reading.
- **reskate.skater.animation_values()** returns a table keyed by full descriptor name. Each entry has value (boolean, enum integer or finite float) and output (boolean). Only occupied per-rider pages with recognized types and bounded names are read. Unrecognized values and descriptor defaults are omitted.
- **reskate.skater.rail_target()** returns nil or a table with available, targeting and direction. Available means the optional selector hook has been prepared; target observations come from the local rider's exporter.

## Native physics requests

**reskate.skater.set_animation_value(name, number)** needs **skater.animation**. It writes one occupied Float.Physics output property of the current local rider. Inputs, booleans, enums, descriptor defaults and exported flags cannot be written. Values must be finite and within +/-1,000,000. The owner, descriptor, encoded key and page are resolved twice. Gameplay input must be allowed. This is an immediate request, not a persisted or continuously forced value. Returns true, or false and a reason.

**reskate.skater.select_rail_target(type, negative_alignment?)** needs **skater.control**. Type is a native rail target class, integer 0..6; the optional boolean distinguishes alignment-cache sign identity. Nil releases this caller's request. One owner holds a 100 ms lease, renewed by calls; plugin stop releases it. The local rider's ordinary airborne approach must have a live target and pass the normal native request gate. Candidate geometry and acceptance tests still run. Original request fields and alignment-cache bytes are restored afterward; newer native request writers are retained. A stale or different owner/context never applies. Build-specific entry bytes are checked, and HookBatch/Detours installs the hook. No control scheme is known to it.

## Temporary gameplay preferences

**reskate.settings.gameplay(name)** needs **settings.read**. It returns the player's original boolean, or nil until that name has been observed at the game's getter.

**reskate.settings.hold_gameplay(name, boolean_or_nil)** needs **settings.write**. It temporarily overrides an observed boolean during gameplay, or releases the caller's hold with nil. Names contain 1..120 characters. Values are strict booleans. Returns true, or false and a reason.

No preference is saved to disk. The native Settings screen receives the player's original value. Trainer overrides retain priority. Holds expire after 300 ms and stop releases them. One owner per option; at most 32 live holds per plugin. Unknown option names cannot be created.

## Validation and remaining game checks

Native tests cover permission denial, selectors/ranges, leases, expiry/release, original preference/UI behavior, occupied typed animation pages, and PlayStation report layout preservation. Runtime and plugin tests compile with MSVC /W4 /WX. This checkout lacks maintainer assets/emotes, producing CMake missing-dependency warnings; 15 optional contrib plugin fixtures are skipped.

In-game validation is still required for XInput and native DS4/DualSense USB/Bluetooth, menu/replay handoff, slow frames, graph availability, rail acceptance and other control plugins. These APIs do not provide per-poll script execution or sub-tick input sampling.

## Consumer integration

The standalone Dual Stick 6.6 Lua port is maintained in the separate [Dual Stick repository](https://github.com/GoalTrains/reskatedualstick/tree/codex/lua-controls-6.6/lua). Its foot mapping, grind-state handling, pop-to-air handoff and rail-circle logic stay in Luau. This SDK branch contains only reusable controller and local-rider primitives; no Dual Stick mode or native companion is required by the plugin once the APIs are available.
