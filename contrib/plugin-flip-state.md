# Reading the local skater's underflip state

`reskate.skater.flip_state()` requires `skater.read` and returns a read-only snapshot:

```lua
local state = reskate.skater.flip_state()
if state then
    -- true: observed underflip; false: observed inactive; nil: unavailable.
    local active
    if state.underflip_anim == true or state.underflip_state == true then
        active = true
    elseif state.underflip_anim == false and state.underflip_state == false then
        active = false
    end
    -- state.trick is the native CurrentFlipTrick enum, usually still the BASE flip.
end
```

| Field | Type | Meaning |
|---|---|---|
| `trick` | number | Native CurrentFlipTrick enum, including 0 (None). |
| `underflip_anim` | boolean or nil | Bool.Anim.Is.Underflip. |
| `underflip_state` | boolean or nil | Bool.State.Is.Underflip. |
| `underflip_window` | boolean or nil | Bool.Anim.IsInUnderflipWindow; eligibility is not proof of execution. |

The whole result is nil if the local rider or validated flip state cannot be read.
Individual unavailable flags are omitted (nil); known false flags remain false.

The snapshot uses the trainer's existing asset-ID-based reader because these states need
not appear in the graph properties returned by `animation_values()`. It neither alters
that API nor exposes pointers, write operations or another player's state.

This is a point-in-time read, not an event or a latched attempt result. A plugin can miss
a transition between reads. A future event/serial API needs separate runtime capture and
lifecycle design. The generic flags plus the base enum do not establish every named variant,
particularly Hospital/Flower flips; those need an independently verified discriminator.
Do not infer successful execution from the selected lesson or from the window flag.

For live validation, compare ordinary flips, underflips, nollie variants, Hospital/Flower
variants, loading/respawning and local-rider changes. Host tests cover permission checks,
nil results, partial state, false versus missing, and the Lua return shape; they do not
prove native recognition in a running game.
