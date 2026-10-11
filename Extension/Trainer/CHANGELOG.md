# Changelog

## In ReSkate

Changes made when the trainer moved into ReSkate itself.

- **Nothing happens until you ask.** The jump read-out and the controller shortcuts start off,
  jumps are written to the log only while the read-out or telemetry recording is on, and trick
  launches only when the trainer scaled them.
- **The memory search runs on demand.** It used to run for every player after a level loaded,
  and up to seven times when it found nothing. Now it runs once one of the tuning classes'
  values or the flip speed is changed (yours, or a host's you skate with), or the EVERYTHING list
  is open, and at most three times per level. It also runs again on each level for those
  players: a level brings fresh copies.
- **A host's whole setup reaches its guests.** While a session's host sets everyone's physics,
  guests get its class values, trick multipliers and auto push as well as its tuning, and their
  own stand down; a player who joins later gets them too. Before, only the tuning travelled, so
  a host could push or jump further than the guests it was holding to the game's own.
- **Guests are locked by the session's own switch**, from the moment they join, rather than by
  whether the host's tuning had arrived yet. A guest's class values also no longer come back
  after a map change in such a session.
- **Teleport to your map waypoint.** PRACTICE > TELEPORT shows the waypoint you placed on the
  pause map, with a button to go there (`trainer waypoint [info]`). You land on the topmost
  surface there, not at the waypoint's own height, which isn't the ground and could drop you
  through the map. If collision there is still loading, a short watch puts you back on the
  surface once it arrives. `trainer ground <x> <z>` does the same for any spot.
  The map's registry is only read while the TELEPORT card is on screen or the command runs.

## v0.3.0 - 2026-10-04

Built on ReSkate 1.0.7.

- **One Tune screen.** The Presets tab is gone: presets, trick sliders, your own presets and the
  values are one screen, still split into REALISTIC, FUN and EVERYTHING. They all show the same
  values, so a preset can no longer say "on" while a slider says something else.
- **Presets are dials.** Each built-in preset is a slider: 1 is the game's own, the preset's
  number is the preset as it shipped (Super Ollie = Ollie height x3), and anything between or
  beyond works. Below 1 turns the same things down, which is what the Realistic list is for.
  The preset's button still switches it on and off, and lights up whenever the values match
  it, whatever set them. `trainer dial <multiplier> <preset name>` from the console.
- **Reset everything**, above the tabs: every value, lock, preset and trick slider back to the
  game as it shipped. Locked values used to survive every reset (a locked ollie height kept
  Super Ollie alive), and the trick sliders had no reset at all. Each part also has its own:
  Reset values (keeps locks, and says how many), Reset tricks.
- **More values confirmed, by hand.** A player ground, pumped, climbed, vaulted and bailed while
  the trainer watched which values the game read: 56 more are confirmed (climbing 23, vault
  and mantle 10, the speed model 8, push 7, bails 5, grind control 3), 243 of 314 in all. The
  71 that were never read in any of it (all of pumping, the double-stick flip metrics, grind
  lean and transition, strong-impact bounces) stay listed as "no use found".
- **Type any number.** Every dial and trick slider has a box beside it: type a multiplier and
  press Enter to go past the slider's end (Ollie height x10000 if you like). Curve and graph
  multipliers and the trick heights no longer stop at x100.
- The short lists only show values the game was found or seen to read.
- Torpedo Boost no longer names a value the game does not have.

## v0.2.0 - 2026-10-04

Built on ReSkate 1.0.7.

- **Realistic, Fun and Everything.** The Tune tab opens on one of two short lists: REALISTIC
  (pop, push speeds, flip catch times, bail limits, on-foot jump and sprint, all draggable below
  the game's own values, with the Realistic preset one click away) and FUN (the big switches
  and everything on foot and in the air). EVERYTHING is the whole table with search.
  `trainer open realistic|fun|everything` opens them from the console.
- **The game's other tuning.** Much of the game is not tuned by its physics tuning asset but by
  data-defined classes: push speeds, on-foot jump, sprint, flips and rolls, wallrun, vault and
  mantle, dive, torpedo and glide, bail fall speeds, flip catch times, grind control, pumping,
  powerslide, the speed model. The trainer now finds 24 of them in memory (by the defaults and
  field order the game's data ships) and lists their 314 values by name, next to the 3801 of
  the tuning asset.
- **Push speed scales every push.** "Push speed" now drives the game's own push speeds (a
  tapped push, a held one, the top), so taps cruise faster or slower too. The three speeds are
  on the Realistic list by themselves. Measured: x2 cruises at 8.2 m/s from taps (4.0 stock),
  the Realistic preset at 3.0.
- **On foot:** jump height (also as a multiplier beside the trick heights), sprint speed, flip
  rotation and roll speed, wallrun boosts; in the air: glide gravity, air resistance and
  steering, torpedo and dive steering. New presets: Moon Jump, Fast On Foot, Fast Parkour
  Flips, Super Glide, Torpedo Boost. Measured: Moon Jump 0.9 m to 2.6 m, Fast On Foot 6.6 to
  9.8 m/s. The flip, glide, torpedo and dive values were not exercised by a test.
- **Powerslide** forward force and friction, for the speed tricks slides used to allow.
- Trick height sliders go from x0.1 to x50 (Ctrl+click to type); the console takes 0.05 to 100.
- Return after a bail no longer ignores the first bail of a session, and says in the log what
  it did. (The switch is on the Practice tab.)
- **Flip trick speed** (the TRICKS card). The game keeps board flip speed in eight curves; the
  trainer finds them and scales the four speed curves. It slows flips (x0.4: a kickflip turns at
  about 950 deg/s instead of 1300 to 1400); above 1 the game's own limit on how fast a board
  turns takes over, so the slider stops at x3.
- **Evidence behind every class value.** A scripted skater rode, pushed, ollied, flipped, did no
  complies and bonelesses, ran, jumped and sprinted while hardware read watches sat on every copy
  of each of the 314 values: the game read 187. The rest (grind control, pumping, climbing,
  vaults, hard impacts: moves the script does not do) are marked "no use found" and hidden unless
  asked for, like the tuning asset's unread values.
- **A locked value keeps what it drives.** Stock and Reset all used to leave a locked ollie
  height showing its number while resetting the graphs behind it.
- The jump read-out, the log and the telemetry CSV carry the board's fastest turn rate.
- Checked by measurement, with a scripted controller: push speed (x2 cruises at 8.2 m/s, the
  Realistic preset at 3.0), auto push, ollie height, Mega Pop (0.39 to 0.79 m), Fast Spins, no
  comply and boneless heights, on-foot jump (Moon Jump 0.9 to 2.6 m), sprint (Fast On Foot 6.6
  to 9.8 m/s), flip trick speed, game speed (0.5x doubles an ollie's air time), markers and
  their pad shortcuts, teleport. No effect could be measured for the speed wobble start speed
  (heading at 7.5 m/s) or the sideways bail limit (0.01 did not cause a bail); spread-eagle,
  torpedo, on-foot flips, grinds, bails and return-after-bail were not reached by the script.

## v0.1.4 - 2026-10-03

- **No comply height and boneless height.** Two new sliders beside the hippy jump's (Tune tab,
  top of Essentials, and the Presets tab). The game's trick scripts launch these jumps; the
  trainer multiplies the launch where the game sets the jump's trajectory, so the skater and
  the board go up together. Measured: no comply 0.50 m stock, 1.70 m at x4; boneless 0.46 m
  stock, 3.39 m at x9. Console: `trainer option nocomply_height|boneless_height <x>`.
- **Top pushing speed works.** The game skips its own push tuning (pushes aim for speeds the
  trick scripts pick), so the value did nothing. Now, holding push carries on past the game's
  9.1 m/s to your number, and a lower number caps pushing. Taps still cruise at the game's
  4 m/s. The Fast and Realistic presets use it.
- **Auto push works.** The game hands its auto push flag to the animation and nothing comes of it
  (measured: the same coast-down with it on). With it on, a rolling skater that is not braking
  now gains speed up to the auto push speed (8 m/s; both are on the Essentials list).
- **Push strength is hidden:** nothing in this game build reads it while pushes are scripted.
- Checked without a player, with a scripted controller: marker pad shortcuts (LB+RB+Up saves,
  LB+RB+Down returns), ollie height, Fast Spins.

## v0.1.3 - 2026-10-03

- **Hippy jump height.** A slider on the Tune tab (top of Essentials) and the Presets tab. The game
  sets this jump in its trick scripts, not in its tuning, so the trainer recognises a hippy jump
  starting and scales the skater's upward speed.
- **Realistic preset:** lower pop, slower pushing and rotation, earlier speed wobble, easier bails.
- **Spin and flip in the jump read-out:** degrees turned about the vertical (and the peak rate) and
  degrees the body tumbled, in the HUD card, the Map & HUD tab and the log.
- More tuning values are recognised as used: the table now also includes everything the game read
  in traced play sessions (584 of 930 rows, was 527).
- No comply and boneless heights are not adjustable yet: the game drives those jumps along a
  scripted path that a velocity change does not move.

## v0.1.2 - 2026-10-03

- **Tune no longer offers values that do nothing.** A pass over the game's code found which tuning
  values it reads (527 of the 930 rows). The rest, including the Mode ollie heights and the hippy
  jump heights people tried, are hidden unless you tick "Values with no use found", and are marked.
- **Ollie height and body spin speed are plain values again.** The game ignores its own
  `JumpMaxHeight`, `JumpMinHeight` and `MaxSpinSpeed`; the trainer now links them to the graphs the
  game does read, so setting ollie height to twice its stock value doubles the jump graphs.
- The Essentials list only holds values that do something: ollie height, body flip speed, body
  spin speed, pushing speed, grind lock-on and more.
- A preset says when it skipped values you locked.
- Every preset can be switched off again by itself; presets that are still on keep their values.
- Presets no longer contain rules for values the game does not read. Mega Pop and No Speed Wobble
  were rebuilt on the ones it does.
- The Tune tab says that changes apply as you drag and that the box only locks a value.

## v0.1.1 - 2026-10-03

Packaging only (Thunderstore): `Install.bat` and `Uninstall.bat`. The trainer was unchanged.

## v0.1.0 - 2026-10-03

First release, built on ReSkate 1.0.3.

- TRAINER page in the ReSkate menu: Tune, Presets, Practice, Map & HUD.
- Live editing of the game's physics tuning (3,801 values, 89 curves, 80 graphs), named from the
  player's own game data; search, groups, an Essentials list, freeze, reset.
- Quick switches: super high ollie, fast flips (front and back flips), fast spins, never bail.
- Stackable presets, user presets, a preset per map.
- Game speed and pause, five marker slots per map, return after a bail, teleport.
- Speed / air HUD and a read-out after every jump; telemetry recording to CSV.
- `trainer.json` in a mod folder: a map author's spots and recommended preset.
- Every action is a `trainer ...` console command; `trainer selftest` checks a build in game.
