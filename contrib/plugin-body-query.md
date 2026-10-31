# Ray batches beside an owned body: `reskate.world.physics.probe_batch`

`reskate.world.physics.probe_batch(body, epoch, rays, options?)` needs both `world.physics` and
`world.raycast`, and is client phase only. `body` is an opaque handle from `physics.create`
([owned native bodies](plugin-owned-bodies.md)); `epoch` is the opaque token from that body's
`physics.read` sample, which keeps the whole sample (position, rotation, velocities, mass and
inertia) inside. A batch grants no force, motion, pose or ownership.

The native side sends no ignored-owner IDs or ignored entities and looks up no owner. Each actual
nearest hit must pass the pinned native shape reader, the current shape-to-body association, the
body's own capacity and allocation check and its current generation. An excluded or unqualified
nearest hit makes the whole batch unavailable; the batch never skips it, casts through it or
substitutes a farther surface. The reason says which: `owned_body_hit` or
`hit_identity_unqualified`.

```lua
local sample = assert(reskate.world.physics.read(body))
local batch = reskate.world.physics.probe_batch(body, sample.epoch, {
    {from = sample.position, to = sample.position - vector.create(0, 1, 0)},
})
if batch.status == "available" then
    local ray = batch.rays[1]
    -- What a contact means, and what to do about it, is the plugin's decision.
elseif batch.reason == "body_epoch_or_observation_changed" then
    -- Decide later. The SDK does not relabel or replay the query or a command.
elseif batch.reason == "body_exclusions_unavailable" then
    -- The native association data is unavailable; no ray was cast.
end
```

## Rays and options

There are 1 to 32 rays, each exactly `{from = vector, to = vector}` in world metres. Positions must
be finite and within 100 km of the map origin; a segment must be at least 10 micrometres and at
most 1000 m long (or the host's lower limit). Invalid arguments are Lua errors. Lists must be
dense arrays, and any other field is refused.

The options are `exclude_bodies`, `exclude_local_actor`, `require_contact`, `query_preset`,
`include_shape_categories`, `require_named_actor_categories_absent` and `include_hit_owner`, each
described below; every boolean option takes only an actual boolean.

`exclude_bodies = {other_owned_body, ...}` adds at most eight more of this VM's bodies, each in the
same native world and still owned. The anchoring body is always excluded. The native ray filter
takes owner indices, not body or shape handles, and owned capsules have no native owner, so the
batch sends an empty filter and checks the nearest hit's body against the owned exclusions
instead. Plugins cannot supply addresses, native indices or actor IDs.

`exclude_local_actor` defaults to false. `true` returns `local_actor_exclusion_unavailable` before
any cast: the placement root lookup takes a packed entity key, while the local skater is known
here only as a SkaterEntity pointer, and a verified conversion does not exist yet. Leaving it out
does not make a batch safe from hitting the local skater.

## The result

The result has `status = "available"|"unavailable"`, `native_casts`, `budget_exhausted`, the exact
`epoch` passed in, and a dense `rays` list of the requested length; an unavailable batch also has
a `reason`. `local_actor_excluded` reports whether the backend established that exclusion for an
available result that asked for it. It is the backend's fact, never a copy of the request's flag,
and the native backend never sets it, so it is false today. A backend result that claims it, or
`named_actor_categories_absent`, without the matching request, or claims both, is refused as
`invalid_query_backend_result`. `owned_bodies_excluded` on an available batch says the nearest hits
were checked against the owned bodies; it is not native filtering, and not proof that a whole ray
is empty.

Each available ray has `status = "clear"|"hit"`. A hit adds its `position` and `fraction` on the
segment, the flags `normal_available` and `surface_velocity_available`, and `motion`. With the
default options the measurements are left out and `motion` is `"unknown"`. A clear ray has no
geometry or measurements. No hit names the body, collider, face or feature it met.

## Measured contact

`require_contact = true` turns on the contact readers (their bytes are compared first), the reads
of the hit's result vectors and of the hit body's motion, and their closing checks. Every hit then
needs both a validated `normal` and `surface_velocity`, or the whole batch is
`contact_measurement_unavailable` with its actual cast count. A clear ray stays clear.

The hit point and normal are copied while the native ray result is still alive. The point must lie
on the segment within 2 cm; a malformed, non-finite, non-unit or backfacing normal is refused,
never normalised or flipped. The point's motion is read from the hit body's own current arrays as
`linear + angular x (contact_point - body_position)`. An available value has `motion =
"measured"`; zero motion is an instantaneous measurement, not proof that a surface never moves.
Every accepted hit's rings are read again in the final whole-batch check, even when several shapes
share one body. These are endpoint observations: they do not pin a native allocation or rule out
reuse between reads.

## Query presets

`query_preset` chooses the fixed native filter every ray in the batch sends. `"camera"`, the
default, is the filter the game's camera ray sends, as `world.raycast` and `world.probe` do. `"terrain"`
sends the game's own physics query preset of that name: one category bit. `"all"` is a diagnostic
with every filter bit set, so the ray tests every shape whose categories are nonzero, actors'
shapes included; it also needs `world.physics.observe`, checked before any budget or cast is
spent. An `"all"` hit may be the local skater's own shape. Any other value, a number or another
spelling is a Lua error before anything is spent, and no raw mask can be passed: the native values
live in `Engine/Game/Build/20260929/park_editor.h`.

A preset selects which shapes the ray tests; it does not say what a hit belongs to. A `"terrain"`
hit is not proof of terrain, static geometry or a surface free of the skater and board, and a
`"terrain"` clear means only that no shape with that one bit was on the segment. The game's Static
preset lacks the Terrain bit, so some ramps and props are not tested at all. Every other rule is
the same, and a refused terrain ray falls back to nothing.

A batch that names a preset gets one more field on every result, `requested_query_preset`, which
repeats the request; `status`, `native_casts` and each ray say what actually ran. A batch without
the option does not have that field.

## Shape categories

`include_shape_categories = true` asks for `in_categories` on each qualified hit: the hit shape's
InCategories mask as read after the ray, a whole number from 0 to 4294967295. It needs
`world.physics.observe` as well, checked only when the option is true and before anything is
spent; without it the call is a Lua error. Leaving it out reads nothing and adds no field.

`in_categories` appears only on a hit row of an available batch that asked for it. Missing means
unavailable; `0` is a known empty mask. It is missing when the build's category getter no longer
has its pinned bytes, when the array or element cannot be read, or when either changed between the
paired reads or before the closing check. It is read only after the hit passed the shape reader,
the association and the owned-body exclusion, at the hit's own shape index (so two shapes of one
body keep their own values), and copied into the result only after the closing check. An identity
change seen then still refuses the whole batch as `body_epoch_or_observation_changed`; a change to
the category alone drops that ray's value.

This is one mask as read after the ray. It does not identify an actor, a floor or static geometry,
says nothing about collision-time filtering, excludes nothing and grants no force.

## Named actor categories

`require_named_actor_categories_absent = true` is a refusal-only check on the hits a batch returns.
It needs only `world.physics` and `world.raycast` and shows no category. A call that names the
option, true or false, gets `named_actor_categories_absent` on every result. The named actor
categories are the build's category bits whose definitions name a character, ragdoll, character
proxy, skateboard or skater (bits 2, 3, 21, 25, 26, 28 and 29), plus bit 31, which only the
SkateboardCollisionHelper shape template carries: `0xB620000C`, in `park_editor.h`. Bits 6 and 12
of the Character template are left out, because the Terrain and DynamicGameplay templates carry
them too and real floors would be refused.

Before any cast the batch is refused as `named_actor_categories_unavailable` when the preset's
filter shares a bit with those categories (`"all"`) or the category getter's bytes do not match;
`exclude_local_actor = true` is still refused first. Otherwise each hit's InCategories is read as
`include_shape_categories` reads it, and after the closing check every hit row is judged in order:
an unreadable or dropped value refuses the batch as `hit_categories_unavailable`, a value sharing
no bit with the filter as `hit_categories_inconsistent`, and a value with a named bit as
`hit_named_actor_category`. Each refusal keeps its actual cast count. Clear rays pass.

`named_actor_categories_absent = true` means the filter the ray sent shares no bit with the named
categories, every returned hit had a readable mask unchanged at the closing check, and each mask
overlapped the filter and had no named bit. It is not actor exclusion and never sets
`local_actor_excluded`: a shape of an actor or prop that carries only world-like bits passes, and
`"camera"` also tests bits that other players, vehicles or Build Kit pieces may carry.

## Hit owners

`include_hit_owner = true` asks for each qualified hit's shape owner slot, read and checked exactly
as `in_categories` is. It needs `world.physics.observe`. A hit row then has `owner_assigned`
(false for the build's unassigned slot) and, only when that is true, `owner`: a locked userdata
that supports `==` and nothing else. Both are missing when the slot could not be read or changed
before the closing check. Two `owner` tokens are equal when they come from the same plugin
instance, SDK world generation and measured epoch serial and their slots were equal, so single-ray
batches against one epoch can be compared. The slot, its index, kind and generation never reach
Lua, and equal tokens say nothing about whose owner it is.

## The observation window

Before and after the rays, the batch checks the owned anchor, the client context, the native world
and state, the SDK world and body generations, the owned exclusions and the exact step epoch of the
sample. Both checks also need the latest published sample and a fresh read-only native read to
match every lane of the token's sample exactly, so even a same-epoch `read` or `receipt` that
replaced the latest sample cannot rebind an older token. A changed observation discards **every**
ray. These checks qualify one local observation window; they do not lock the physics world or
freeze its arrays between reads, and a clear result does not prove a whole region has streamed.

A batch publishes no sample, observes no successor, consumes no pending command and acknowledges
nothing. A later command from the same epoch still has to pass `submit`'s own checks.

## Budgets and reasons

Each ray spends one of the VM's 64 raycasts per tick, the limit `world.raycast`, `world.pick`,
`world.probe` and `session.raycast` share, even when the backend is unavailable and casts nothing.
Separately, batch rays and `world.probe` share a hard limit of 64 **actual native casts** per
tick, and each batch ray may use at most two. The game's ray uses one: it ignores nothing, so it
never casts again, and in the game the 64 raycasts therefore run out before the native casts do.
The native limit and the two-cast allowance bound what a backend may do; `world.raycast`,
`world.pick` and `session.raycast` are not counted against them. `native_cast_budget_exhausted`
means the shared casts ran out; `ray_cast_limit_exhausted` means a ray's two casts could not settle
a result. The game's one-cast ray can cause neither today; the plugin tests reach them with a
constructed backend that casts twice per ray. Both refuse the whole batch and set
`budget_exhausted`.

Before any cast: `client_phase_unavailable`, `local_actor_exclusion_unavailable`,
`named_actor_categories_unavailable`, `body_exclusions_unavailable` (association data
unavailable) and `query_preconditions_unavailable` (the backend could not run before any ray). A
C++ exception after the first cast is `query_backend_exception`, which closes the rest of the
tick's casts and leaves out `native_casts`; a malformed backend count does the same as
`invalid_query_backend_result`. After casting: `query_unavailable`, `owned_body_hit`,
`hit_identity_unqualified` (each with the cast that found it), `contact_measurement_unavailable`,
`body_epoch_or_observation_changed`, `invalid_query_backend_result`, and the three named-category
reasons. An identity lost while reading a hit's category or owner is
`body_epoch_or_observation_changed`, because the closing check sees the same loss.

## Tests, and the game

`Engine/Game/World/Test/body_query_tests.cpp`, `body_contact_read_tests.cpp` and
`body_shape_categories_tests.cpp` cover the batch contract, contact reads and categories over
constructed values and fake memory. `Extension/World/Test/owned_body_source_tests.cpp` runs the
service's `probe_batch` over a constructed world. The plugin tests cover the binding:
`owned_body_batches_*`, `body_contact_details_are_known_and_epoch_bound`, the shape-category and
exclusion-fact fixtures, and the measured-contact fixture
(`Extension/Plugins/Test/fixtures/measured-contact`), a diagnostic mod that can also be run in the
game.

None of these tests runs the game's ray or physics, and this code has not been run in the game:
ray batches, measured normals and surface velocity, the `"terrain"` and `"all"` presets,
categories, the named-category check and hit owners are checked only against constructed values.
