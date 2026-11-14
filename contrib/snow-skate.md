# Snow Skate: the SDK changes it needs

Snow Skate is a plugin mod (not in this repository) that puts every map under snow and turns the board
into a snowskate. This change adds the general building blocks it needs. None of them knows about snow
or snow skating, so other mods can use them for sand, dust, other weather or other surfaces.

## `reskate.effects.cover(settings | nil)`, `reskate.effects.keep_clear(places)` and `reskate.effects.trail(point)`

Permission: `effects`.

```lua
reskate.effects.cover({colour = {r = 0.95, g = 0.97, b = 1}, amount = 1, facing = 0.6, softness = 0.12,
    brightness = 2.2, breakup = 0.4, sparkle = 0, snowfall = 0.6, fall_speed = 1,
    left = 1, snowfall_stopping = false})
reskate.effects.keep_clear({{position = skater_position, radius = 0.6, height = 2, snow = 0}}) -- every tick
reskate.effects.trail({position = deck_position, width = 0.22, fade = 20}) -- as it moves
reskate.effects.cover(nil)
```

The call lays a cover over every upward-facing surface of the world, with falling snow in front of it. It
stays until `cover(nil)`, and one plugin may have a cover at a time.

- **`keep_clear`:** keeps up to three places clear of it for 0.3 s at a time.
  - **Upright cylinders:** with `snow` (0 to 1), whatever stands in one shows a patchy dusting of the cover
    on every side instead.
  - **Board-shaped places:** with a `length`, `forward` and `up`, the place is board-shaped instead. In its
    own plane the cover is `snow` of what it would be, and no trail is pressed into it.
- **`trail`:** adds a point on a trail through the cover. Along the last 256 points the cover is pressed
  down, thinner and greyer, `width` wide (each point its own), and fills back in over `fade` seconds.
  - **`under_roofs = false`:** none of the cover where something is overhead (indoors, under a bridge),
    thinning out over about two metres in under the edge, and no flakes falling under one (outside still
    snows, seen from in). A roof map round the camera finds it: one straight-down ray into the client
    collision a cell, a fine grid (0.5 m, 64 m across) and a coarse one (1.5 m, 192 m across). Only cells new
    to the map are measured (the nearest first, about two hundred rays a tick at most), and two dozen again
    a tick; it is forgotten out of play, and copied to the GPU only when it changed. For each fine cell the
    highest surface within 3.5 m is kept too, so most of the ground finds it is under no roof from one read.
- **Tread:** with a `tread` width, a tread's 13 lines show along it (7 lighter, 6 darker ridges).
  - **Slopes:** each stretch is found on the plane through both its points, so a trail follows a slope.
  - **`starts`:** the point begins a new trail, not joined to the point before.

**What Snow Skate uses it for:**

- **Enable Snow and Enable Snow Skating:** the snow itself, and the snow falling.
- **Melting:** `left` melts the snow in patches at the warm times of day, and builds it back up after.
- **The snowfall stopping:** `snowfall_stopping` stops new flakes, and the last ones fall out of the sky
  instead of vanishing.
- **Dynamic snow speed:** `fall_speed` changes gradually. The fall is added up at each moment's speed, so a
  change of speed carries on from where the flakes are.
- **No snow on the player:** `keep_clear` around the skater.
- **Snow on the player after a tumble:** `snow` on that place, building up in a wipeout and fading after.
- **Tracks in the snow:** `trail` under the deck as it rides, as wide as the deck is across its way (so a
  powerslide or a revert sweeps a wide mark), with the snowskate deck's tread when it rolls straight. Also
  where the skater tumbles, slides or rolls off the board.
- **Marks under each part of the deck:** `starts` begins a new trail when another part is on the snow (the
  middle riding, the tail in a manual, the nose in a nose manual) and for the skater's own mark, so they
  are not joined by a line across.
- **No snow indoors:** `under_roofs = false`.
- **The deck:** a board-shaped `keep_clear` place on it. The snowskate deck rides so low that the
  skater's cylinder took its griptape for the ground: it got full snow and the board's own track. Now it
  has a light dusting and no track.

**How it is drawn:** `Extension/UI/Overlay/world_cover.{h,cpp,hlsl}` draws one full-screen pass into the
finished scene, just before the game adds its UI, so menus and the HUD are never under it. It sees the
game's barriers through the existing hook in `depth_tap.cpp`, and frames end through `overlay_render.cpp`.

**Choices made for reliability:**

- **Its own depth copy:** it copies the world's depth as soon as the game finishes it. The game's own depth
  was sometimes reused by the time the cover drew.
- **A rebuilt depth:** a new depth of the same size takes over once it has been seen in three frames in a
  row. Until then the last copy, up to six frames old, is used, so the snow neither blinks nor uses a depth
  the game only needs briefly.
- **The near distance:** the camera's near distance isn't available, so it is measured from the skater. It
  is held over about three seconds of frames drawn with their own depth, and a change is eased in. A value
  that moved with each measurement made the snow's pattern jump.
- **When it is drawn:** it is off in menus, but a single frame of "not in play" no longer switches it off.
  It is drawn in the replay editor too, which the game counts as a menu.
- **The camera:** the game's picture is 0 to 2 frames behind the camera ReSkate reads (it eases its camera
  between updates), so no fixed delay fits. The cover uses the camera the game drew the frame with: its
  compute constant buffers bound at root parameter 0 begin with it (found in the game with the GPU
  diagnostics). Each frame the newest 32 are read; of those close to the camera read from the game, the
  one most hold. Where none is found (the replay editor, whose camera read is not the one drawn with), the
  camera 1.15 frames back, as before.
  - The buffers are read through the GPU memory watch (`gpu_diagnostics`), which is now on without
    `RESKATE_GPU_DIAGNOSTICS` too, light: each heap and buffer's GPU addresses only.
  - The places kept clear (the deck) are taken from as many frames back as the drawn camera is found to be.
- **Its shaders** compile on a thread of their own as the hooks go in: compiled as the snow first came on,
  on the game's drawing thread, the game showed a black screen and left the level.
  The kept-clear places are taken from the same moment, so a turning deck's place stays on the deck drawn.
- **Proton (Steam Deck):**
  - a D3D12 function vkd3d-proton will not let be hooked in place is followed through its interface's
    table slot;
  - the shader avoids a loop Wine's compiler could not unroll.

## `reskate.skater.surface_sound(word | nil[, fields])`

Permission: `skater.control`.

The local skater's sounds play as if on the surface that the physics material `word` names, wherever the
skater actually is. `fields` lists which of the sound's 18 material fields this applies to; leave it out
for all of them. `nil` gives each surface its own sound again.

**What Snow Skate uses it for:** **Grass Rolling Sound**. Rolling and landing sound like grass on every
surface (fields 3, 6, 7, 9, 10 and 11), and grinds keep their own sound.

**How it works:** the change is in the submit hook in `Extension/Multiplayer/Remote/native_audio.cpp`. The
game only sends the fields that changed, so the whole state is sent again when the sound is set and then
every 30 updates; without that, some fields kept the old surface after a respawn. `session.cpp` follows the
local sound outside a multiplayer session too.

## `reskate.world.time()`

Permission: `world.environment`.

Returns the time of day forced on the map: `"default"`, `"morning"`, and so on up to `"weathernight"`. It
returns `nil` when no map is loaded.

**What Snow Skate uses it for:**

- **Melting:** melting and the snowfall stopping at the warm times (default, noon, afternoon), and the snow
  building up again at other times.
- **Weather toggles:** the **Snow Day** and **Snow Night** toggles show the time of day actually set, so they
  agree with ReSkate's own time-of-day menu.

## `reskate.world.controls_text()` and `reskate.world.set_atmosphere({...})`

Permission: `world.environment`.

`controls_text()` returns the fog, sky, wind and atmosphere values the player set in ReSkate's menu, one
`key=value` a line. `set_atmosphere` sets several of those values together. Each value is a number,
`"a,b,c"` lanes, or `"default"`. It takes at most 64 values, and one change in four a second.

**What Snow Skate uses it for:** the snow's own fog is applied when the snow comes on. The player's values,
read with `controls_text()`, are put back when the snow goes.

## `reskate.package.choose(name, {now = false})`

Permission: `variants`.

With `now = false`, the variant is only kept for the game's next launch: the level isn't loaded again, and
the variant in play stays. Without it, `choose` works as before. The live mods log now also says which
variant of each mod is in.

**What Snow Skate uses it for:** **Snow Skate Deck on Reboot**, which chooses whether the next launch starts
with the snowskate deck (the mod's `snowskate` variant) or the standard one, without a reload now.

## `reskate.trainer.hold_preset(name, {[id] = number, ...} | nil)`

Permission: `trainer`.

The plugin's own trainer preset is held, under its own name. While it holds:

- every other preset and change is off;
- the trainer is locked for the player, and its menu says which plugin holds it;
- the player's own setup is not saved over, and their presets are never touched or overwritten.

On `hold_preset(nil)`, or when the plugin stops, the player's presets and changes come back exactly as they
were. Ids are as the trainer's own presets name them: tuning values and its settings. The preset is held
again on each new level's copy of the tuning.

**What Snow Skate uses it for:** a **SnowSkate** preset (jump heights, spin and flip speeds, grind and
transition settings tuned for a snowskate), enforced only while the snowskate deck and tread are in.

**Code:** `trainer::plugin_preset` in `Extension/Trainer/trainer.cpp`, beside `plugin_tune`.

## Time-of-day lighting

This is in `Extension/World/time_of_day_lighting.{h,cpp}` and is not a plugin API.

**The problem:** after a change of time of day, for example from Afternoon to Weather Day, the ground away
from the camera kept the old time's light and glowed white until looked at. This happens in plain ReSkate
too.

**The cause:** the game keeps its distant sun shadows and each place's view of the sky in caches. It
rebuilds them a part at a time, starting with the part in view.

**The fix:** for 8 seconds after a time layer (`TOD_<n>_<name>`) loads or unloads, ReSkate holds
`WorldRender.DistantShadowCacheEnable` and `WorldRender.DynamicSkyVisEnable` off, then puts them back as
they were. A setting already off, or changed from the console meanwhile, is left alone. This was found and
checked with those settings in the console. Frame rate did not change.

**What Snow Skate uses it for:** its melted snow is otherwise covered by that white glow, which looks like
the snow came back.

## Tests

- `Extension/Plugins/Test/plugin_tests.cpp`: one test for each of the new calls above, each also checking
  that a plugin without the permission is refused.
- `Extension/World/Test/time_of_day_lighting_tests.cpp`: when the settings are held off and put back.

All of it was checked in the game on build 20260929 with Snow Skate:

- the snow, melting and the snowfall stopping;
- the grass sound, including after a respawn;
- the fog being restored;
- the white glow gone after a time-of-day change, with no blink of the snow.
