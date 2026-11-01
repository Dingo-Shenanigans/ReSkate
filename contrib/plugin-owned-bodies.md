# Owned native bodies: `reskate.world.physics`

`world.physics` lets a client plugin create and drive its own physical capsule compound in the
local physics world. It grants no access to the skater's bodies, native pointers, collision
masks, the console, progression or paid content. It is a general building block: what a body is
for (a prop, a cart, a ball, a vehicle) and how it is steered belong in the Luau mod.

Related pages: [preparing the producer](plugin-body-prepare.md), [ray batches beside a
body](plugin-body-query.md) and the [plain world probe](plugin-world-probe.md).

## API

Declare `"world.physics"` in `reskate-plugin.json`. Every call is client phase only; a dedicated
server's copy of a plugin gets "native bodies are client phase only". Creation, observation and
submission refuse in multiplayer and whenever the local native owner is unavailable or changed.
Receipts and acknowledgements stay available for retained records. Nothing activates on load:
the plugin calls `create` itself.

```lua
local P, v = reskate.world.physics, vector.create
local body, why = P.create({
    mass = 12,
    inertia = v(2, 3, 4),      -- principal moments, kg m^2
    position = v(0, 2, 0),
    rotation = {0, 0, 0, 1},   -- unit Hamilton XYZW
    capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}},
})
```

The values are examples, not defaults. The body has a zero local centre of mass and an identity
local frame, and every capsule endpoint is in that frame. Bounds: 1 to 8 capsules, endpoints
within 10 m, radius .01 to 2 m, mass 1 to 1000 kg, principal inertia .01 to 10000 kg m^2, world
position within 100 km and a unit quaternion. Any other field is an error. Collision filtering is
fixed: the capsules have no category of their own and collide with the world only. A mod cannot
supply masks.

`create` returns opaque userdata or `nil, why`. The first call in a session may only request the
producer hook and return `waiting for a fresh qualified 60 Hz physics step` before any allocation;
a later explicit `create` can then use the observed clock. Allocation itself is never retried.

`P.read(body)` returns a measured sample or `nil, why`:

```
{epoch=<opaque>, serial=<decimal string>, time=<accumulated step seconds>, seconds=<step h>,
 position=<vector>, rotation={x,y,z,w}, velocity=<vector>, angular_velocity=<vector>,
 mass=<kg>, inertia=<vector>}
```

The opaque epoch binds the VM instance, the body generation and the world generation. It cannot be
built from numbers, moved to another body or adopted after a plugin reload. The read repeats the
ring headers, values, direct arrays and ownership checks; a raced or changed read publishes no
sample. These repeated reads are not an engine-wide atomic snapshot.

`P.submit(body, sample.epoch, command)` returns `true, nil, sequence` or `false, why`. The
sequence is a decimal string, unique for the process. `command` has optional `force`, `torque`
and `impulse` vectors (zero when left out) and, for a nonzero one-shot impulse, a positive 32-bit
`impulse_id`. The bounds are 40000 N of force, 4000 Nm of torque, 600 Ns of impulse and 40000 N
for `force + impulse / h` together: safety limits, not tuning. Each body has one pending inbox and
a ring of four receipts. Publishing reserves a ring slot before any native setter; a pending
inbox is never replaced, and a full ring refuses without spending the caller's serial or impulse.

A command computed from sample epoch **N** targets only native step **N+1**, with the same step
length and time `sample.time + h`. A step still at N waits; a missed N+1 or 100 ms of wall time
expires the command. One matching native call consumes the command before either setter, so a
repeated call or a partial native call can never replay its impulse. The clock is the local
skater's physics-step serial and accumulated step seconds since its current owner, not a global
world clock, and each step's copy is bound to that step's game context, native world and world
state.

`P.receipt(body)` returns the oldest unacknowledged record (outcome `none` when there is none).
`P.receipt(body, sequence)` selects that body's retained record; an absent, freed or other body's
sequence returns `nil, why`, and the selector must be the exact string `submit` returned:

```
{outcome="none"|"queued"|"applied"|"partial"|"rejected"|"expired",
 sequence=<string>, requested_serial=<string>, produced_serial=<string>,
 force_attempted=<bool>, torque_attempted=<bool>, retained=<bool>, quarantined=<bool>,
 successor=<measured sample, only when one has been observed>}
```

`applied` means both native setters returned and their owner and clock checks held; it does not
say the step integrated them. `successor` is a later, separately read sample of the same body and
world generation, with serial and time strictly past the step that produced the command. Each
record keeps its **first** such successor. Neither the API nor the tests make up a resulting
motion from the command; the plugin reads what actually happened.

`P.acknowledge(body, receipt.sequence)` frees only the oldest finished record, once; an applied
command also needs its measured successor. Selecting a later record never allows acknowledging out
of order. The same observed body can publish for steps N+1, N+2 and N+3 while earlier records
wait, so a plugin that reads and publishes every step can drive the body at 60 Hz; the ring
applies backpressure when receipts are not drained.

A partial or uncertain setter fails the body for good. Its record stays selectable even when an
older applied record never got a successor, and quarantine does not make up that missing proof.
Release and unload cancel the pending inbox and keep the earlier outcomes.

`P.release(body)` revokes its commands, clears pending force, torque and velocity, zeroes and reads
back every shape mask, then parks the body keyframed. Closing the VM (an unload, a reload, or a
plugin stopped for an error) releases all of its bodies: every plugin's close asks for that, and
for a VM that never created a body it finds none and touches nothing native. A successful release
means **retained and parked**, not destroyed. Without fresh ownership no old native handle is
touched and the body is quarantined; such a body is not claimed to be stopped.

## Native boundary

ReSkate supports one game build, and every operation's bytes and layout are in
`Engine/Game/Build/20260929/rigid_bodies.h` and `remote_collision.h`. The service compares all of
them before it hooks or calls anything. The hook on the game's body synchronizer calls the
original once, then tries the bounded command consumer, keeping the original's LastError. It is
installed with Detours in a HookBatch, and refuses a synchronizer that is already patched. The
step clock comes from No Bail's physics-step listener (`add_no_bail_step_listener`), which hands
on the published local skater.

Creation and parking run in the qualified local client/TLS phase, as the remote skater collision
proxies (`Extension/Multiplayer/Remote/remote_collision.cpp`) do. That phase and the repeated
ownership checks do not prove a physics-world mutex. The consumer is qualified by the local
skater's body wrappers, the current world and state, the allocation and dynamic bits, body and
shape generations, the configuration, the watched owner chain and the 60 Hz step clock; it is not
a world-wide phase guarantee.

Native callbacks use fixed process-lifetime storage and try-locks only: no Lua, JSON, allocation,
formatting, logging or blocking lock. Eight allocation attempts are kept for the process lifetime,
failed and partial ones included, and no slot or old-world handle is reused. The eight are shared
by every plugin, every reload and every level: once they are used, `create` refuses until the game
restarts, so a mod iterated with hot reload runs out after eight bodies. A native destructor is
not known, so running out of slots is an explicit refusal, never a hidden allocation or a fake
deletion. An uncertain constructor result stays quarantined even when the confirmed part can be
parked, and a partial creation prohibits another allocation in that world.

Until a plugin calls `prepare` or `create`, the service's client tick only notes the client phase:
it installs no hook, reads no multiplayer state and calls nothing native.

## What is tested, and what is not

`Engine/Game/World/Test/owned_body_tests.cpp` covers the definitions, the command mailbox and its
receipts. `Extension/World/Test/owned_body_source_tests.cpp` runs the service itself over a
constructed client, physics world and TLS. `Extension/Plugins/Test/plugin_tests.cpp` covers the Lua
binding against a constructed game: opaque handles and epochs, bounds, pipelining, receipts,
acknowledgement order, release on close, reload and the permission and server refusals.

None of that runs the game, and this code has not been run in the game. Creation, measured reads,
commands produced on the next step, torque, several compounds, a whole session of control, and the
cleanup of an active body during unload, a world change or a partial failure are checked only
against constructed worlds and values.
