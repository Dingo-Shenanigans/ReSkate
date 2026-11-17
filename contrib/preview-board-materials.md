# Board material parameters in the native preview

Board parameter APIs accept an optional scope. Omitting it keeps the existing gameplay behaviour.

```lua
-- Permissions: "board", "room"
local scopes = reskate.board.parameter_scopes() -- {"gameplay", "preview"}
local parameters = reskate.board.parameters("deck", "preview")
if parameters then
    -- Use a colour key declared by the preview item's own mod.
    local ok, why = reskate.board.set_parameter("deck", key, {r=.5, g=.5, b=.5}, "preview")
end
reskate.board.clear_parameter("deck", key, "preview")
```

`board.parameter_scopes()` requires `board`. Reading, setting or clearing the preview scope requires both `board` and `room`. An unsupported scope is an argument error. Existing calls without a scope still address only gameplay; plugin teardown releases both scopes.

The preview target is the freshly observed native room board. Its actual cosmetic recipe determines the items and their declarations. The highlighted deck can differ from the equipped deck. Menu labels and plugin-supplied pointers do not grant access to a material.

Preview access fails closed when the actor or its recipe cannot be validated. Ambiguous slots and missing deck identities are refused. Restricted items cannot grant preview writes, including writes to existing colour keys. Material and item changes must settle before access resumes. Reads return `nil` for unavailable or blocked parts; setters return a reason.

Changing the preview recipe discards the previous recipe's held parameters. Gameplay and preview holds are separate. Plugins should clear their preview scope when leaving the preview, changing effects or losing the pose. Integers and switches remain read-only, and declarations retain their existing ownership and type checks.

For motion-driven effects, `reskate.room.pose("board")` supplies the preview pose. Update the preview's declared material inputs from that pose; do not move the equipped board into the room. This changes neither the preview's geometry nor its native camera controls.

Validation covers scope dispatch and permissions, preview owner validation, recipe slot mapping and transitions, blocked writes, and the existing material hold/restore behaviour. A local in-game check confirmed material-driven motion on the normal inventory preview using a custom deck.
