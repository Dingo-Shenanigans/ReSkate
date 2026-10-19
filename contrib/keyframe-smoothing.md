# Keyframe smoothing

The replay editor's free camera moves through its keys without the changes of speed the game makes
at each one. Every key is still met exactly, at its own time: only the way the camera gets from
one key to the next changes. Off unless the player turns it on; the other camera modes stay on the
game's own curve.

Code: [replay_curve.cpp](../Extension/ReplayKeyframes/replay_curve.cpp) (the curve, apart from the
game) with [tests](../Extension/ReplayKeyframes/Test/replay_keyframes_tests.cpp),
[replay_keyframes.cpp](../Extension/ReplayKeyframes/replay_keyframes.cpp) (the hooks, the rebuild and
the counters), [replay_settings.cpp](../Extension/ReplayKeyframes/replay_settings.cpp) (the saved
settings), [replay_keyframes_commands.cpp](../Extension/ReplayKeyframes/replay_keyframes_commands.cpp)
(console), and the build's addresses in
[replay_keyframes.h](../Engine/Game/Build/20260929/replay_keyframes.h).

## What the player gets

- `keysmooth on|off`, also a switch Keyframe smoothing in the pause menu's Mod Options > Visuals and
  in the overlay (Insert) under Skater > Freecam > Replay editor. A change applies to the replay open
  in the editor at once.
- `keysmooth amount 0..1`: 1 (the default) is the smoothest curve that still meets every key, 0 the
  game's own.
- `keysmooth status`: the settings, and counts of the segments built, by camera mode.
- The amount is also in the menus: a slider beside the switch in the overlay, and Keyframe
  smoothness (100, 75, 50, 25%) beside the switch in Mod Options > Visuals.
- Kept in `%LOCALAPPDATA%\ReSkate\KeySmooth.settings`.

## How it works

- **The game's curve.** The replay camera's keys are framing keys. Whenever a key changes, the game
  builds one segment per key (`segment_build`): the key, the next one and their two neighbours as
  slots, plus a factor k per channel. The player (`segment_eval`) plays a segment as a cubic
  Hermite from key to key, with the tangent at a key the mean of the velocities into and out of it,
  times (1 − k). So the speed carries on through a key, but the acceleration jumps at every one;
  k brakes a channel that slows down or turns back at a key (k = 1 stops it there); and the first
  and last keys are left and reached at half speed. That is what shows as the camera surging and
  settling around each key, and what makes a follow shot's skater drift in frame.
- **The smooth curve.** The curve through the same keys at the same times whose speed and
  acceleration both carry on through every key (C2) is the natural cubic spline. With the keys and
  their times fixed, it is the one that bends the least, so nothing smoother is possible without
  missing a key or moving its time. Its velocity at every key, per channel, comes from one
  tridiagonal system (Thomas algorithm), with uneven gaps between keys taken into account. The
  amount mixes the game's tangents with these.
- **Written into the game's own segments.** The hook calls the game's builder, then rewrites the
  segment it built: for each channel the Behaviour plays, the neighbour slots go to `p2 − 2·m1` and
  `p1 + 2·m2` and k to 0, so the game's unchanged player plays the tangents m1 and m2. To have every
  key, the hook walks the replay's key map and builds each key's slot with the game's own slot
  builder (`slot_build`) and angle unwrapping (`slot_unwrap`), the way the game builds the
  neighbours. A segment whose own keys don't match those is left as the game built it and counted.
  Nothing runs per frame: the segments are built when keys change.
- **Once per rebuild.** A rebuild builds one segment per key, and each needs every key's slot: the
  game's rebuild is hooked too, and the slots built for its first segment are kept for the rest of
  it (per Behaviour and Subject), so a rebuild builds each key's slot once instead of once per
  segment. A segment built outside a rebuild builds its own.
- **A change applies at once.** A new setting calls the game's own rebuild (`session_rebuild`) for
  the replay the builder saw last, after checking the editor still has it. Console commands run on
  the game thread.
- **Camera modes.** Each segment carries the Behaviour (the evaluator its camera mode plays through)
  and Subject. The free camera is Behaviour 1, Subject 0; the board camera Behaviour 0, Subject 2.
  Only Behaviour 1 is smoothed. Behaviour 1 plays the camera's
  position, its three rotation angles (so pans, tilts and roll) and its field of view; all of them
  are smoothed, each on its own.
- Every function the hook calls or patches is checked against a 32-byte fingerprint first; a game
  build that differs gets no hook, and the commands say smoothing is unavailable.

## Seen working in game

- The free camera's follow shots lose the surge at each key with smoothing on, by eye.
- The path between keys changes too, not only the speed along it: on a cinematic shot through the
  same keys, the game's curve takes the camera through a bush that the smoothed one goes round.
- An export plays like the playback. Smoothing stays on after leaving the editor and when a draft
  is reopened in the same run. A key added at the playhead mid-segment doesn't make the camera jump.
- Off by default; on after a restart once turned on. The board camera's keys stay on the game's
  curve with smoothing on.
- After a rebuild, every segment the hook rebuilt matched the game's own keys ("not matching 0").

## Not tried yet

- The editor's preview of a key (the values shown at the playhead when stepping between keys) is the
  game's own and isn't smoothed; no difference was seen.
- On an out-and-back path the smooth curve swings a little past the turning key, as a spline does.
- A switch inside the replay editor's own menus would sit better than Mod Options, but needs the
  editor's menu definition worked out.
