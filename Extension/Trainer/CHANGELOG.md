# Changelog

## v0.8.6 - 2026-10-21

Built on ReSkate 2.1.1, like 0.8.3.

- **Animation timing is 1 by default.** Players found the flat ground landing stutter with more gravity goes away with
  Animation timing at 1, so moving Gravity off 1 now gives tricks and landings their normal-gravity timing at once.
  Advanced gravity is still there to change it. A 0 saved by 0.8.5 is not kept: everyone starts at 1.
- **Tooltips in paragraphs.** The Gravity and Advanced gravity tooltips break into short paragraphs.

## v0.8.5 - 2026-10-21

Built on ReSkate 2.1.1, like 0.8.3.

- **Advanced gravity.** A fold under Gravity (POP AND TRICKS) for the landing stutters players still saw with more
  gravity on flat ground tricks. The game fits a trick into the time to land, but cannot hurry a catch past its own
  speed, so in a shorter flight the catch is still playing when the wheels touch.
  - **Animation timing**, 0 to 1. At 1 the skater's air animations play faster by just as much as the flight is shorter
    than at the game's gravity (slower when it is longer), so tricks, catches and landings look as they do at x1, and
    flips turn faster with it. 0 is the game's own timing; in between goes part of the way. `trainer option gravity_timing <0..1>`.
  - **Landing gravity of its own.** The gravity the game plans landings with: the same as Gravity unless ticked. Less
    than Gravity lands late, more reaches for the ground early. `trainer option gravity_landing_own 1`,
    `trainer option gravity_landing <x>`.
  - Presets carry them, RESET EVERYTHING puts them back, and they let go with Gravity under a host's physics or in a
    session with boosts off. In a session with boosts on, Animation timing speeds up every skater's animations while
    you are in the air (the trainer cannot tell them apart yet).

## v0.8.4 - 2026-10-21

Built on ReSkate 2.1.1, like 0.8.3.

- **Landings at other gravities.** With Gravity at anything but x1, landings could still come late and stutter on real
  jumps (0.8.1 fixed it only for flat ground). The game plans each landing by searching trajectories with its own
  gravity, and the skater follows the trajectory it picks; that search now uses the trainer's gravity, so the skater
  lands where and when the flight actually comes down. Grind searches from the air use it too.
- **Grind lock-on 0 is no lock-on.** The dial now also sets how far grinds reach from the air, so at 0 the board is no
  longer pulled onto a rail in flight (over a gap, over water). Above x1 grinds reach further from the air as well.

## v0.8.3 - 2026-10-20

Built on ReSkate 2.1.1 (same game build as 2.1.0).

- **On ReSkate 2.1.1.** Everything from 0.8.2, with ReSkate 2.1.1's own changes: mod conflicts redone (fewer false
  conflicts, a Resolve conflicts button), the new MY MODS list with official mods in it, other players' objects solid by
  default, announcements in the launcher, and extra GPU crash info. Install it over ReSkate 2.1.1.

## v0.8.2 - 2026-10-19

Built on ReSkate 2.1.0, like 0.8.0.

- **Underflip speed.** A slider under Flip trick speed (POP AND TRICKS > TRICKS) for every underflip. You flick and the
  underflip triggers as always. The game held underflips to 1 to 1.2 times its speed whatever the flip speed was; the
  trainer moves that range with the slider. By itself it sets the whole underflip, and Flip trick speed does not reach
  it. `trainer option underflip_speed <x>`; presets carry it (`trick.underflip_speed`) and RESET EVERYTHING puts it back.
  A session's host shares it with the other physics.
- **Underflip part only.** A switch under Underflip speed: the underflip's flip turns at Flip trick speed and Underflip
  speed takes over where the board starts to turn back. The game sets the rate of the clips playing the turn back as
  the underflip starts, so from that point the trainer scales the rate the game plays them at every tick, until the
  landing. `trainer option underflip_split 1`, preset key `trick.underflip_split`.
- **A speed for each underflip.** At the bottom of Advanced trick speed: kickflip, heelflip, pop shuvit, FS pop shuvit,
  impossible, front foot impossible, nollie impossible and nollie back foot impossible underflips each have a speed,
  times Underflip speed (nollie underflips use their regular trick's, but for the impossibles). The Late flip slider is
  there too (the same setting as Late flip speed). Presets carry them like the other trick speeds.
- **Late flip speed in the TRICKS card.** The late flip's own speed has a slider beside Flip trick speed and counts
  without Advanced trick speed ticked (it moved out of the Advanced list).
- **Dial sliders reach 0.** Grind lock-on and the other dials go down to 0 on the slider.

## v0.8.1 - 2026-10-18

Built on ReSkate 2.1.0, like 0.8.0.

- **Boosts past the sliders.** On SKATER > CHEATS the forward, up and off-board up boosts take any typed number up to
  10000 (the sliders still end at 300 and 25), and Revert Boost strength up to x100 (the slider ends at x5). The
  `forwardvelocity` and `upvelocity` commands take the same.
- **Boosts start at 0 (off).** The forward, up and off-board up boosts started at +20, so RESET EVERYTHING could not read
  STOCK. They now start at 0, which adds nothing until you set a speed, and RESET EVERYTHING puts them back to 0.
- **Cheats and addons count.** Noclip, No Bail, Dark Pop, a boost with a speed above 0, Revert Boost, the
  grind exit triggers and Board Break show at the top of QUICK SETTINGS with what they are set to (Go to, Reset), are
  named on the MODIFIED line at the top of SANDBOX instead of STOCK, and RESET EVERYTHING turns them back. Road Rash
  and Hall of Meat only show bails and still never count.
- **Dials go to 0.** A preset's dial (Grind lock-on, Grind friction, Body flip speed...) refused 0 in its box and in
  `trainer dial`. 0 now puts every value it moves to 0, for example Grind lock-on at 0 for no grind lock-on at all; a
  value it moves the other way stays the game's own.
- **Landings on time with another gravity.** The game plans each flight's landing with its own gravity and the
  skater's air pose reaches for the ground as that countdown runs out. Above x1 the skater came down before the
  countdown ended, so the body sank into the ground before the wheels touched, and the landing came late and
  stuttered (at x3 a flat ollie bailed). The countdown now follows the gravity that is set.

## v0.8.0 - 2026-10-18

Built on ReSkate 2.1.0.

- **In ReSkate 2.1.0's menu.** The trainer is the SANDBOX page, as ReSkate now calls it, with its tabs TUNE, PRACTICE
  and MAP & HUD, and works in the full screen menu made for a controller. Its own MOVEMENT tab is gone: Noclip, No
  Bail, the boosts and Revert Boost are on SKATER > CHEATS, Hall of Meat, Road Rash, Board Break and the grind exits on
  ADDONS. `trainer open movement` opens SKATER > CHEATS.
- **Type any number, past any slider.** Every value has a box beside its slider, in the window and in the full screen
  menu: type a number and press Enter. A slider's ends are no longer a limit for a typed number (game speed, return
  delay, gravity 0.001 to 1000, flip speed to 1000, pump power and the bending boost to a million, the transition and
  grind numbers, the bending rules).
- **Slide or step in the full screen menu.** A click on a value still steps it (left half down, right half up); press
  and move left or right to slide it. The controller works as before and skips the number boxes.
- **Every value says what it is.** The 74 named values that had no explanation have one; hover a name (or select it
  with the controller, in the bottom bar) to read it. Values under EVERY VALUE without one say what they belong to.
- **Hardcore** (ReSkate's own preset, from 2.1.0) is beside Realistic under WHOLE GAME. Skate 3 on Hardcore keeps its
  name, "Skate 3 Hardcore".
- **Board Break's wear and the trainer leave each other alone.** A worn board steers and rolls softer through four of
  the game's tuning values (steering, the trucks' spring, damping and twist), which Board Break sets itself. Any change
  in the trainer used to put those four back to how they were when the level loaded, so a worn board handled like a
  new one again, and a level loaded on a worn board counted the worn numbers as the game's own. Now they stay Board
  Break's until you change one yourself (then yours counts, and Board Break leaves it alone, as it always did), and
  their stock is the game's own.

## v0.7.1 - 2026-10-15

Built on ReSkate 2.0.9, like 0.7.0.

### Fixed

- **Bailing no longer opens the trainer.** The trainer had its own controller combo for the menu, LB + RB + click
  the right stick, which is also how you bail. It is gone, as standard ReSkate has none: open the menu with Insert.
  `trainer option pad_menu` says so; a saved setting for it is ignored.

## v0.7.0 - 2026-10-14 (not released yet)

Built on ReSkate 2.0.9. Measured headlessly on San Vansterdam, 2026-10-13 and 14. Not played by a person yet.

**Install in a new, empty folder.** From ReSkate 2.0.9 on, the launcher refuses to start from Steam's skate. folder: put
`ReSkateLauncher.exe` and `ReSkate.dll` in a folder of their own (for example `C:\Games\ReSkate`). The first start installs
the supported game build there (sign in to Steam when asked); later starts only check it.

### Fixed

- **The trainer no longer changes your game's own settings.** Its three GAME SETTINGS rows (pump assist, pumping
  strength, forward to push) wrote a second copy of the setting into the profile, beside the one the game's Settings
  menu keeps, and the game's scripts read that copy: what you set in the game could be overruled by the trainer's.
  They now only show the game's value; change them in the game's Settings menu. `trainer transition pumpassist`
  (and the other two) only reads.
- **Dark Pop is Dark Pop 3.1** (Sivaes/darkpop, GPL-3.0; the trainer had carried the abandoned 0.4-era "auto release",
  which could not act at all when Steam's overlay answered the controller first). It arms a flight on a real D-pad
  press (Left, Down or Right) with the board upside down, routes the landing through the game's own sink state and
  keeps it from bailing. While the skater is in the air the game's own controller reads see the D-pad let go, so
  skate.'s Quick Drop radial no longer opens and takes the landing; this now also covers the read Steam's overlay
  answers first. One switch under TRICKLINING > DARK POP, saved in the local profile like No Bail: on and off both (3.1 saved only the on, so turning it off did not stick and it came back on by itself). `darkpoprelease` and
  `darkpopextra` are gone.
- **Revert Boost 2.0** (Sivaes) under MOVEMENT > BOOSTS, off by default, with its own strength. It is a second
  implementation of the board bending boost: use it or TRICKLINING's "Board bending boost", not both.
- **Flip speed now changes late flips** (a flick in the air after the pop; reported by a player). While a late flip
  runs, the game sets its speed on every update to the flick's speed held between 0.6 and 1, so the trainer's flip
  speed never reached it (measured: x0.5, x1 and x2 all turned a late kickflip about 260 degrees at 1155 degrees/s).
  The trainer now scales those two ends by the flip speed and the per-trick **Late flip** speed, which until now did
  nothing (the game logs a late flip as a plain Kickflip or Heelflip). Played by the owner: late flips worked.
  New under EVERYTHING: **Late flip start / Start After** (how long into the air before a late flip may start,
  0.25 s) and **Late flip buffer / Flick Held** (how long a flick in the air is remembered, 0.13 s).
- **The search for the game's values is faster and shows in the panel.** It walked all of the game's memory five
  times and checked every word against each of the game states it looks for; it now walks it once (twice on the
  first search) and skips words that start none of them. While it runs, the panel says "Finding the game's tuning
  values..." with its time: changes made meanwhile reach the game when it ends.

### New

- **Wallies.** The game ships with its wallie switched off. POP AND TRICKS now has **Wallie: Wallies** (the
  game's own switch): ride at a kerb, ledge or wall and pop just before it. Off: no wallie in 4 tries; on: every
  time. With it come the numbers the game hard-codes in its wallie script:
  - how early the pop may come on a tall obstacle (18 frames; at 120 a pop half a second early still wallies),
  - extra pop per metre of obstacle and the most extra pop (x3: 11.2 m/s upward against 7.0),
  - the lowest obstacle that adds pop, and the fastest speed a wallie works at (11 m/s; at 3 none went at 5.3 m/s),
  - how early the pop may come on kerbs and steps (16 frames; at 120 a pop well before a building's steps wallied 2 of 2,
    the game's own 0 of 3),
  - the lowest obstacle it looks at (0.225 m; at 0.6 those steps no longer wallie),
  - the forward push as it climbs (-0.2 m/s; at 3 the skater went over at 5.7 m/s against 4.2, at -6 it stalled to 1.7),
  - **Wallie needs a quick ollie** (the game's switch; on, a normal ollie no longer wallies).
  Dials: Big Wallies, Wallie At Speed, Easy Wallies.
- **Double and triple jump on foot** (ON FOOT, the game's own switch "flying"). Press jump again right as you
  land: 0.82 m, then 1.42 m, then 3.11 m. The game's own gives 0.82 m each time.
- **Flips and spins on foot have real speed rows.** Jump, hold LT or RT (tuck) and push the left stick: forward
  or back flips, sideways spins. The limits are numbers in the game's scripts. Fast Foot Flips x2: 1,646 deg
  against 724; Fast Foot Spins x2: 890 against 598.
- **No speed limit on foot.** The game cuts speed on foot to 10 m/s. The cut now rises with the sprint speed you
  ask for: sprint speed 13 runs 13.2 m/s, 20 runs 20.1 (before: 10.0). At the game's own values nothing changes.
- **Jump height on foot raises its own caps.** "On foot: jump height" moves the highest and lowest jump with it
  (3 m alone rose 1.32 m before, 3.19 m now).
- **Jump on release** (ON FOOT, the game's own switch): the jump goes when you let go of the button. Held 3 s, the jump
  came 3 s later and went as high (0.89 m).
- **Wipeout bounce** (ON FOOT, the game's own switch): a slam bounces and slides on. A 30 m drop slid 10 m, against
  half a metre.
- `trainer foot [<switch>|all game|off|on]`: 29 of the game's own hidden switches for moves on foot and on the
  board (tackle, grab and throw, rope swinging, auto mantles, wallrun from a sprint, curb hops ...). Only the five
  measured ones have a row in the menu. Careful with `foot nogetups on`: after a slam the skater stays down until it
  goes back to `game`.

### Removed (they changed nothing when measured)

- **Landing: leg length** and the **Deep Landings** dial: the value belongs to footplants, the game never read it
  in 30 landings and the crouch was the same at 0.3, 0.92 and 1.5.
- **On foot: flip rotation speed** (replaced by the rows above), **jump speed limit**, **steering in the air**,
  **sprint boost speed**, **roll speed** and **roll speed gain** (a roll stays at 7.5 m/s whatever they are).
  They are still under EVERYTHING. Moon Jump and Fast On Foot lost those parts; Fast On Foot now really runs
  11.7 m/s.

### Not measured

- Glide, torpedo, falling and wallrun rows: the scripted pad never got into those moves.
- Wallrides; the straight-on wallie window; the side jump, trick and flip speeds of a wallie; kerb hops.
- Mantles and wallruns from a sprint (no wall that stayed put was found), Splat (no change on a 30 m drop).

## v0.6.0 - 2026-10-12

Built on ReSkate 2.0.7.

### New

- **Gravity.** One slider on POP AND TRICKS for gravity in the air, on the board and on foot. 1 is the game's
  own. Measured with a scripted ollie: at 0.5 the fall is 5.5 m/s2 and the same pop goes 0.70 m and hangs
  1.05 s (the game's own: 0.36 m, 0.58 s); at 1.5 the fall is 15.8 m/s2. On foot a drop fell at 4.9 and 14.3
  m/s2. It acts in the air only: rolling down a hill, grinds and a bailed skater keep the game's own.
  Presets carry it (`trick.gravity`), RESET EVERYTHING puts it back, and it is the game's own under a host's
  physics or with boosts off. `trainer option gravity <x>`.
- **The first section is PRESETS AND QUICK SETTINGS.**
  - **PRESETS** holds every kind of preset in one card: the whole game (skate., Skate 3 Easy, Skate 3,
    Skate 3 Hardcore), the grind feel, the transition feel, Skate 3's tricklining extras, every built-in
    style (Realistic, Super Ollie, Hard To Bail ...) and your own saved presets. One press puts one on, a
    second press takes it off.
  - **QUICK SETTINGS** lists everything that is changed right now, whatever section it lives in: values,
    dials and the trainer's own settings (trick heights, flip speeds, gravity, grind and transition
    settings, bending rules), grouped by section. Change any of them right there. Pick a preset and the
    list fills with everything that preset sets.
  - Each row has **Hide** (off the list, still set; it comes back by itself the next time it changes from
    the game's own, or with "Show hidden"), **Go to** (opens the section it lives in and scrolls to it, the
    value tinted, with the related settings around it) and **Reset** (back to the game's own).
  - The card folds like every other, so the whole list can be put away.
- `trainer setup <key> <value>` sets any one of the trainer's own settings by its preset key
  (`trick.flip_speed`, `grind.push`, `transition.lean` ...).

### Changed

- The section WHOLE GAME is now PRESETS AND QUICK SETTINGS (`trainer open game` still opens it). Its PLAY
  LIKE card is the first row of PRESETS, and Realistic sits with the other styles.
- "Slow flips pop higher" follows the gravity that is set.

### Known

- The game aims an air off a ramp back at the ramp with its own gravity. With another gravity a vert or bowl
  air can come down somewhere else than the game aimed it. Not yet tried on a ramp: tell us.
- Gravity was measured on flat ground with a scripted controller. It has not been played by hand yet.
- Everything under Known for 0.5.0 still holds.

## v0.5.0 - 2026-10-11

Built on ReSkate 2.0.6.

### The menu

- **TUNE is one page of sections.** The REALISTIC / FUN / TRICKLINING / TRANSITION / EVERYTHING lists are gone.
  Everything is sorted by what it is about: WHOLE GAME, POP AND TRICKS, SPEED AND PUSHING, BAILS AND LANDINGS,
  GRINDS, TRICKLINING, TRANSITION, PUMPING, ON FOOT and EVERY VALUE (the whole table). A click on a section's
  bar opens or closes it, a click on a card's title folds that card, on every tab; what you leave open is
  remembered. A section that holds a change says so on its bar ("3 changed") and starts open, and each has
  its own reset that touches nothing outside it (`trainer reset section <name>`). `trainer open <section>`
  opens the menu there. The search box still looks through everything at once.
- **Your presets are above the tabs**, beside RESET EVERYTHING: pick one and Load it, or name what is set now
  and Save it. Manage opens the list (share as a line of text, import, delete, load by itself on this map).
- **The badge says what is modified**, in words: "MODIFIED: 14 values, grind settings, No Bail". STOCK means
  nothing at all is changing how the game plays. Dark Pop and the game speed now count (they did not), and
  RESET EVERYTHING turns them off too, along with a preset set to load on this map.
- **Presets carry the whole setup.** A saved or shared preset used to leave out the grind settings, the
  transition settings, the bending rules and the two flip switches; it now holds all of them. Presets from
  earlier versions still load. Not in a preset: locks, No Bail, Noclip, Dark Pop, the game speed, your
  game's own settings.
- **Rows that are your game's own settings say so.** Pump assist, Pump strength and Forward to push are
  marked GAME SETTING: the game's Settings menu shows the same value, changing it here changes it there,
  and they are never part of presets, resets or the STOCK badge. The grind and transition settings the
  trainer can hold ("held on / held off") show your own value beside them, are never written to your
  profile, are not shown to the game's own Settings screen, and come back the moment you let go.

### New since 0.4.3

- **MOVEMENT tab** on PHYSICS with ReSkate's Noclip, No Bail, Hall of Meat and the boost sliders (one No Bail).
- **Dark Pop** (from Sivaes/darkpop, GPL-3.0) under TRICKLINING: catch with RB, press D-pad Right in the air.
  Start with the release at 0.60 to 0.68 s. Measured with a scripted pad; not yet confirmed by hand.
- **Manual pop height** (x 1 = 0.31 m, x 2 = 0.71 m measured; a riding ollie is untouched).
- **Slow flips pop higher, so they land clean**: with flip trick speed below 1, every pop into a flip trick
  (riding, out of a manual, out of a grind) goes as high as the slowed trick needs. Owner-confirmed.
- **Flick tricks at full speed**: front foot and nollie impossibles and 360 pop shuvits no longer turn slower
  than a kickflip at the same setting. Owner-confirmed.
- **Twenty per-trick speeds** under Advanced trick speed (ollie, nollie impossibles and late flip added), and
  the flip trick speed leaves a plain ollie alone.
- **GRINDS: feel like skate. or Skate 3.** Skate 3 feel: the left stick alone turns the board on a one-truck
  grind (Skate 3's own dead zone, curve and easing), the right stick tips and pops, a pop goes sideways the
  way the left stick is held (1.8 m/s), and what skate. added to leaving a grind is off. Owner-confirmed
  ("feels perfect"). Every setting and switch behind it has its own row; hippy jumps off a grind are boosted
  by the hippy slider.
- **TRANSITION: feel like skate. or Skate 3**, with rows for leaning into the ramp on a coping grind, rolling
  off the coping at your angle, no hang-ups, the coping re-catch delay, the handplant exit, and the game's own
  lip, vert and handplant numbers. First builds from Skate 3's rules: none of the coping, hang-up or
  handplant helps has been measured on a ramp yet. Tell us what they do.
- **PUMPING**: Pump power (owner-confirmed) beside the game's own Pump assist and Pump strength.

### Fixed

- "Reset tricks" also wiped every grind and transition setting. It resets its own card now.
- Taking a Skate 3 feel off could put back values you had set yourself, and a feel that was on by its values
  but not by name could not be taken off. A feel now takes back exactly the values that still hold its numbers.
- Turning Tricklining extras off reset bending boost and footplant heights you had raised yourself.
- The badge read MODIFIED for per-trick speeds with Advanced unticked and for a lock on an untouched value.
- A held grind switch could stay where the trainer put it after a new skater was built.
- The stick the game is shown on a grind or a coping, the held switches and the held settings now expire by
  themselves within half a second if the trainer stops running.
- `trainer reset everything` from the console is refused under a host's physics, like the button.

### Removed

- "Board turn / tip speed on a grind" and "Left stick turns the board on a grind" (the tick box): they did
  nothing. The working control is "Left stick turn range".
- The dials "On board: gravity" and "Get-on-board speed" (measured: no effect), "Catch flips at a set point",
  and the old PUMPING card's strength rows unless Legacy pumping is held on (the game's newer pumping does not
  read them).
- Values the game was never seen reading are no longer on a section; they are all still under EVERY VALUE.

### Known

- The Skate 3 grind stick, the lean into a ramp and Dark Pop work by changing what the game reads from an
  Xbox-style (XInput) controller. The game reads a PlayStation controller another way unless Steam Input
  presents it as an Xbox one.
- A slowed flip without "Slow flips pop higher" still hangs in the air.
- Flip speeds, pump power and the grind switches take hold about 15 seconds after every level load.

## v0.4.3 - 2026-10-07

Built on ReSkate 1.1.4 (0.4.2 was built on 1.1.3). The trainer itself is unchanged.

## v0.4.2 - 2026-10-07

- **Flip trick speed really sets the speed, both ways, and each trick can have its own.** The
  slider now multiplies the speed the trick's animation plays at while the board is in the air
  (the game state Float.Anim.TrickFlipSpeed), not the curves in front of it: x 0.5 is half
  speed, x 2 is double (faster than the game's own never worked before), for every flip trick,
  shuvits, 360 flips and varials included, and the pop is untouched. **Advanced trick speed**
  adds a speed for each of 16 tricks (kickflip, heelflip, the shuvits, varials, 360 flip, laser
  flip, hardflip, inward heelflip, impossibles; a nollie uses its regular trick's), multiplied
  with the main one. Presets carry them (`trick.flip.<trick>`, `trick.flip_advanced`).
- **A slow flip is slow from the start.** Below x 1 the game's finish-before-landing rule is held
  off by itself, so the board is not hurried round: with too little air you land on a board that
  is still turning. The switch "Let slow flips stay slow" of 0.4.1 is gone: this does its job
  (a preset that carries it still loads).
- **Pump power.** A new slider under TRICKS sizes what pumping a transition gives: 1 is the
  game's own, x 2 is about one more of the game's pumps on top of each of yours, x 10 and up is
  a rocket, under 1 takes speed away. The game's own pumping state (Bool.Intent.Pumping) says
  when you are pumping, and for every physics step it does the trainer adds speed along your
  travel (`trainer option pump_power <x>`, preset key `trick.pump_power`). Like the flip speed
  it is found about 15 seconds after a level loads. Off under a host's enforced physics or with
  boosts off. It replaces the dial "Pumping strength" (Pump Power), which changed nothing: the
  game's own gain is small (0.3 to 1 m/s a pump, measured) and fades the faster you already go.
- **Whole setups live at the bottom of MAP & HUD.** "Play like" (skate., Skate 3 Easy, Skate 3,
  Skate 3 Hardcore) and YOUR PRESETS moved there from TUNE: the setup you have now is saved under
  a name, turned on and off, shared as one line of text and imported from one (`trainer open
  presets`). TUNE keeps the presets that each change one thing (the dials and switches);
  TRICKLINING keeps its "Play like" row. Saving now also works when only a trick setting is
  changed.
- **The camera tab is gone.** The camera controls did not work reliably, so they are out of this
  version: the CAMERA tab, `trainer camera` and the saved camera numbers (ignored if present).
- **The dial "On foot: flip and roll speed" is gone.** It changed nothing in the game. The three
  values are still listed under EVERYTHING.
- **Far fewer memory searches.** Every bail builds a new skater and each one started a full search
  of the game's memory (about 13 seconds of a background thread), so a session of slow flips
  searched over and over. A search that a new skater asked for and that found nothing new now
  doubles the wait before the next (20 s up to 10 minutes); a level load or a real change puts it
  back.
- **It takes about 15 seconds after every level load.** The game loads its flip animation states
  with the level, and the trainer has to find them in memory again; until it has, the main
  slider falls back to the old, weaker way and the per-trick speeds do nothing. The TRICKS card
  says so while it is looking.
- **Catch flips at a set point of the jump** (TRICKS, for realism): a switch and a percent. Every
  flip trick is round and caught at that part of the air time, on a small ollie and a big one
  alike (options catch_at, catch_percent; presets carry them). The useful range is about 55 to 100.
- **A banner when the host controls physics.** In a multiplayer game whose host enforces physics a
  card above the tabs says the menu will not work there; another says so when the host has turned
  boosts off. The rules are unchanged.
- **Push speed goes past the game's ceiling.** The push class speeds never lifted the speed model's
  top speed (about 11 m/s whatever they were set to). A push is now carried on to the dialled
  speed on the physics step and held there for as long as the game holds a pushed speed.
- **Board bending boost: no more speed from ordinary 180s.** 0.4.1 also counted a landing whose
  board and body were together 12 degrees or more out of line with the travel. The game
  straightens plain short or crooked 180s (flip trick 180s too) that way, so they got a boost,
  and a bigger one than a real auto revert. That rule is now off as shipped ("or off the travel"
  reads "off" at 90): a bend is the board landing 40 degrees or more off your body, as in
  AutoRevertBoost. Settings saved by 0.4.1 with the old 12 are moved to off. Turn the rule on
  yourself and a landing that counts by it alone gets the auto revert boost at most. Found and
  measured by AutoRevertBoost's authors (Sivaes, jaq and OVM).

## v0.4.1 - 2026-10-06

Built on ReSkate 1.1.3. Everything of the 0.4.0 test build, plus:

- **Let slow flips stay slow.** The game speeds a flip trick up so the board is round 1/6 s before
  the landing it predicts, however slow the flip speed is set: that is why a slowed flip still
  came round on a small pop. The new switch under TRICKS turns that rule off (`trainer option
  flip_gate 0`): the board turns as slowly as set and lands however far round it got. It is saved
  and shared with your presets, and off under a host's enforced physics.
- **Help on everything.** Every button, slider and switch of the trainer says what it does when
  you point at it, and the tips wrap instead of running off the screen.

## v0.4.0 - 2026-10-06 (test build)

Built on ReSkate 1.1.3. From what players asked for in the ReSkate Discord. Everything marked "untested" is in so that it
can be tried: tell us what it does.

- **Camera tab.** Move the game's own cameras: distance, height and side for the low camera, the
  high camera and the on-foot camera, with a "SunJay's Low Cam" button (the numbers of SunJay's
  mod, which showed where the game keeps them). The cameras keep their smoothing and stay out of
  walls. Also an experimental camera of your own that replaces the game's.
- **Skate 3 presets.** Skate 3, Skate 3 Hardcore and Skate 3 Easy set every value this game still
  shares with Skate 3 by name (77 of them differ) to Skate 3's number: pop, grind pops, pushing,
  pumping, steering, manuals, body spins and flips, bails. Not yet measured against Skate 3.
- **Pop out of grinds, finished.** All six values are named (grinds, board slides, nose and tail
  slides; the highest pop and the quick-pop minimum) and one dial moves them together.
- **More dials:** on-board gravity, get-on-board speed (untested), body flip air time (untested),
  landing speed you can take, landing compression (untested), revert friction, pumping strength.
  Manual pop, the stick boost when popping, and the bad-landing strictness are named values.
- **Search finds everything:** dials, switches, trick sliders and every value, on all three lists.
- **The menu works from the controller.** LB + RB + click the right stick opens and closes it;
  D-pad or left stick moves, A presses, B goes back. It can be switched off on the Practice tab.
- **Share presets.** "Share" copies a preset as one line of text; "Import from clipboard" adds
  one someone sent you. Your own presets now carry the trick sliders too.
- **You can see when you are stock:** a line beside Reset everything says STOCK or MODIFIED.
- **Plainer words:** named values explain themselves, with units, when you point at them.
- **Flip trick speed keeps working, and slows flips properly.** The game builds its tuning objects
  again with a new skater (a respawn, a teleport, a session change) and they came back at its own
  numbers, so a slowed flip (and every value of the game's tuning classes: push speeds, on-foot
  values) quietly stopped applying. The trainer now checks once a second, writes back what the
  game undid and looks for the new objects. The slider also scales the three flick-influence
  curves it used to leave alone, which is why x0.4 only slowed a flip by a third. And below 1 it
  stretches the flip catch times by the same factor: the game hurries any flip that would not
  finish inside its catch time, so slowed flips used to snap back to full speed.
- **Tricklining** (a fourth list on the Tune tab). Everything tricklining needs in one place (reverts and powerslides,
  pumping, pops, spins and flips, manuals and grinds), under a "play like" choice:
  skate., Skate 3 Easy, Skate 3 or Skate 3 Hardcore (the choice heads every list of the Tune tab), plus
  Skate 3's tricklining extras. The revert boost's rules are yours to change there: how much spin
  counts, how far out of line the board must land, what each kind of revert is worth.
- **Skate 3 presets grind the older way.** They also set 21 grind and pumping values from the unused
  tuning set the game still carries (Skate 3's wherever the two can be compared): grind friction,
  slide angles, no automatic turning between grinds. Each tricklining card says what it is for.
- **Board bending boost** (TRICKS; it was first called the revert speed boost). Land a spin the game has to auto revert and get speed back, as
  earlier builds of the game did; chain reverts to build speed. 0 is off. The measuring and the
  numbers are AutoRevertBoost's, by Sivaes, jaq and OVM (github.com/Sivaes/AutoRevertBoost). It also
  counts the common revert theirs did not: board and body together landing out of line with the
  direction of travel.
- **No limits of the trainer's own.** A slider's ends are only where the slider stops: every value,
  dial, trick slider and camera number takes any typed number (flip trick speed 0.01 to 100, camera
  numbers have a box beside the slider).
- No comply and boneless heights now also apply to weak upward launches (riding downhill).
- The menu tab is called PHYSICS.

## In ReSkate

Changes made when the trainer moved into ReSkate itself.

- **Hardcore preset**, beside Realistic on the REALISTIC list: Realistic with the game's help
  turned down, for players coming from skating games without it. Everything Realistic changes,
  at least as far, and grinds that only take a close, lined-up landing (lock-on distance x0.5,
  landing angle x0.6), grinds and powerslides that lose speed (friction x1.3 and x1.5), slower
  body flips and spins, and earlier bails (bail limits x0.7, bad landings bail). The amounts are
  a starting point and were not measured in game.
  `trainer preset apply hardcore`, or `trainer dial <x> hardcore` for part of it (0.75 is the
  preset). The `trainer_presets` test checks that it never goes easier than Realistic.
- **Your presets no longer hide a built-in one.** A preset of yours saved before a built-in
  preset took its name (yours first, so the built-in's button applied yours) is renamed when the
  trainer loads, to "Hardcore (yours)" for example, and the maps that apply it follow. The log
  says so. Saving under a built-in preset's name was already refused.
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
