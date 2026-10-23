# Moving a complete plugin world object

`reskate.entities.set(id, blueprint, transform, hidden, attachment, solid)` already
updates a resident object without recreating it. That must include the blueprint's
other entities, such as its attached effects, as well as its root spatial model.
This correction uses the same public call and `world.entities` permission.

The native creation path makes a private entity bus beneath the local skater's
source bus. Previously the move path only changed the root spatial entity. An
effect created by another object in that blueprint could stay at its creation pose
while the model moved. The correction updates the private blueprint transform
first, then moves the root spatial entity, matching the existing native skateboard
world-transform ordering. It does not recreate the object or restart its effects.

The created bus and source bus are retained separately. Before a move, ownership,
context, native transform type, and animation control are checked. A reused bus,
the skater's bus, a shared parent transform, or an animation-driven transform is
refused. The existing verified native setter is reused with its byte check. No new
hook, raw-memory Lua API, mode name or asset name is introduced.

## Validation

Build `dingosdk_runtime` and `dingosdk_world_entity_transform_tests` in Release with
warnings as errors. Run CTest `world_entity_transform`, plus `plugins` and
`native_flag_session` for the adjacent public API changes.

The offline tests cover ownership isolation and invalid native pointers. They do
not emulate the game renderer. Before merging, test a normal single-model object
and a composite blueprint with an animated effect: translate, rotate and scale
each repeatedly with `entities.set`; confirm the root and effect move together,
the animation does not restart, depth occlusion remains native, and the source
skater stays still. Also remove/hide the object and unload the level. The composite
visual result has not yet been verified in game.
