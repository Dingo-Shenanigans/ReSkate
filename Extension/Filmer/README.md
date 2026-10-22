# Filmer

skate. filmed the way a skater with a VX1000 films a line.

## Modes

- **Filmer** (`filmer mode follow`; in the menu just "Filmer"): another skater rides with a skater and
  films them while you play normally. With **Filmer in the camera cycle** on (CONTROLS tab, on by
  default) the game's camera height button (the right stick click) goes high, low, ReSkate's first
  person (when that is in the cycle), the Filmer, and back to high.
  It rides behind you or out to your side, never in front. A way planner looks a few times a second at
  the spots it could take (the side it keeps to, a quarter round, straight behind on your own line, the
  other side) where you are and where you are heading over the next second or so, and keeps to the one
  with ground at its level, no wall between it and you, and a clear way there: it moves over or tucks
  in behind before the wall, bench or drop, not once it has hit it. When you stand still it settles on
  a spot behind you (left or right of your back) and stays still, so you can still see ahead; turn
  round and it moves round behind you again. Too far behind for good footy, it eases off and rolls to
  a stop, and cuts back in behind you once you are well away. Back to a marker or teleported to
  someone, it starts straight in behind you, a little to its side. It rides the trail the skater left, checking every step against the game's
  own collision: real ground under it, no walls, nothing a board would not roll over. It looks ahead
  as it rides and steers round poles, bins, benches and low walls, keeping to one side until past. It stays at
  the level it rides at: up banks and slopes with you, but alongside a ledge or rail rather than up
  on it. Where you came down from its level in one go (a set, a drop) or jumped a gap, it stops (and, once
  stopped, holds its framing like a locked-off shot) at
  the edge, films you land and ride away, and cuts back in behind you once you are 25 m off. This
  goes by your trail, so it works even where the game's collision gives no answer. It pushes to keep up, picks angles through a line (Auto), keeps your board in frame (Tilt),
  and points the camera like a person: a beat late, a loose frame, a hand that is never still, but
  it whips back before a trick leaves the frame. It picks its own side, or keeps to the one you send it
  to (D-pad left / right, the left stick click, or the switch side bind). Film yourself, or anyone in the session
  (`filmer subject <name>`). Click the left stick to send it over to your other side (left / right), where it stays; the slap button
  (LB by default, CONTROLS tab) slaps the lens.
- **You film** (`filmer mode film`): your skater is the filmer. The left stick rides; the right
  stick points the camera (the game stops seeing it, so no flicks while you film). The aim assist
  keeps a friend in frame (the closest one ahead, or `filmer target <name>`) and the stick shifts
  the framing. The slap button slaps the lens. Your skater always wears the camcorder and holds it
  in both hands, crouched (Knee bend, 0.22 m) and hunched (Hunch, 60 deg) over it; on foot the upper
  body holds still and the board is out of sight. Where it is held follows the aim, and the picture
  is taken from there: out in front at the chest aimed level, down at the stomach aimed at the
  ground, up over the head aimed at the sky. The menu's tabs: FILMER, IN HAND, EFFECTS, CLIPS,
  CONTROLS (with a reset to defaults, and the switch that shows DEBUG).

## How the Filmer rides

- **Way finding**: a few times a second it scores the spots it could take round you (its side, a
  quarter round, straight behind, the other side, each closer or further out) where you are and where
  you are heading, and the way there a body wide: ground level with your line beside it, no wall
  between it and you, nothing in the way (a trunk, a post, a building's corner). With something
  coming up on its side the clearest side and distance win, held a few seconds and carved back from
  slowly. Steering round things looks up to about 3 m ahead; through a tight gap it follows your own
  line. Wedged with nowhere clear, it waits for you to come past, then pushes off after you. With no
  room behind you (a wall at your back at the top of a ramp), it goes out to your side, the side with
  more room. It never gets closer than about a metre and a bit, and speeds up a push at a time. On
  foot it runs with you, close behind, quicker round corners, with the camera up a little (off its
  board too).
- **The lens and the ground**: the lens rides the ground under itself, starting up a hump or a kerb
  just before it gets there, eased so one lift handing over to another is never a hitch; it never
  goes into the ground, nor far down below you (off an edge into water, a hole with no ground). Off
  something more than about 4 m high, the camera only starts up once you have been in the air half a
  second, then quicker the longer you fall. Placed afresh (a cut), it is put somewhere with ground at
  your level and you in sight.
- **Turns**: they carve with you, as hard as a skater can, without having to push to turn (only to
  go faster), and through a held carve they keep to the inside of it, close to you, rather than round
  the outside falling back, riding your line's own curve, out from it square to it and closer the
  sharper you turn, their idea of the way you are heading keeping up with you closely. No swapping
  sides mid-turn. Knocked back by something they brush, or dropped behind, they push hard to get back.
  They cut back in only when you are a good way off and out of sight, never with you in view. A 180
  (even slowed right down for it): they come straight round after you, no pause.
- **Ollies after you**: something low you ollied over (a bin, a hurdle, a kerb-high box, up to knee
  high) or a gap you ollied (landing level or up onto something higher), it ollies too, near where you
  popped: a jolt, the camera up quickly over it and back down.
- **Stops by itself** (still panning with you; the left stick puts it straight back behind you in
  one click): a drop too high to huck after an ollie or a flip (it eases off as you near a lip, so
  it stops gently, over a second or so, and eases off the moment you pop off something with the ground
  well below; rolling down a hill or bank, or over a gap onto something level or higher, it
  follows), too far behind, a slam (they carve off to the side as they slow, never on towards you;
  with **Slam zoom**, a slow zoom in if you are in the shot with nothing in the way and small in the
  frame, and 1 in 10 a zoom right in on your face), or wedged. How high a drop it hucks goes by
  **Drop stop** (DEBUG tab, lenient to strict, `filmer set dropstrict 0..1`; 0.6 by default: drops
  over about 1.9 m, once you have been falling below your takeoff for about half a second; strict, 1.1 m).
  Stopped at a drop with you skating away, it zooms in on you (**Zoom when they stop**) at a hand's
  pace, quicker the further off you are; once the zoom is in, the pan drifts on the way you went and
  down to the ground, and then it cuts back in behind you. Drop and too-far stops also end when you
  push or step off the board.
- **Height**: hidden behind a wall or fence you went over, after a beat the camera rises gently,
  as far as it takes to see you, up to about 7 ft. On a ledge you ollied onto it either rides round
  beside it or (a low one, now and then) waits at its foot with the camera raised over it. On a
  grind it pushes up beside you (level, a touch back or a touch ahead, picked each grind) on the side
  it is on, with the camera up a little; down a hubba it rides the stairs beside you, never along the
  ledge itself. Just up on something (ollied up after you), it keeps to its side for a few seconds.
  It knows the moment you lock into a grind or slide (the game's own state) and sprints up beside
  you, pushing hard from the moment you lock in. The camera comes up to about the board's height on
  the rail, a touch over it. Whether it stops where the grind starts and films you along it goes by
  how long the thing is (looked along for its end) and what it goes down over (**Stop at downrails**,
  separate from **Stop at drops**): a long downrail over stairs 6 times in 10, anything under about 4 m
  3 times in 10, otherwise never (a rail down a bank or a ramp: always down it with you). Through a
  grind or slide (from the moment the game says you are locked in) they go straight out to your side,
  square to you, on the side they were on, your board across the frame; 1 in 25 they follow you along
  it from behind instead, as if doing it too. Stopped at the top of one, the zoom onto you is only now and then. Ollie up onto something
  low and wide (a ledge, a manual pad, up to about 0.8 m) and it ollies up after you, on your line,
  camera going up with it, rides it, and comes down off the end after you, or stops at the end if
  that drop is too high to huck; the board sits in the
  middle of the frame, easing back to your body after. Ollies are a soft rise and fall of the frame.
  Down a hill it stays down by the ground.
- **Side buttons** (CONTROLS, `filmer set sidebuttons 1`): D-pad left / right sends the filmer to
  that side of you and keeps them there (you pick the way round a bin or a wall); the same again hands
  the choice back. Picking their own side, they look about 1.5 s ahead and keep elbow room off walls
  and bins, moving over gradually.

## Stopping

- **Stopping** (the left stick, or at a drop): the Filmer rolls to a halt short of the first
  step (it looks along its line for the drop), rises a little (`filmer set lift`, 0.3 m) and pans
  down over the set or rail, still panning with you (never letting the set out of frame) until the
  framing settles, then holds it. Turn it after with the mouse.
- **Angles** (`filmer angle next|auto|behind|side|toggle <angle>`, bindable): switch on the
  angles you want in the menu; one is held, with both the filmer moves between them through a line.
  (Ahead is gone: the Filmer never rides in front of you. Saved as ahead, it is out to the side.)
- **Variety** (`filmer set distvariety|heightvariety 0..1`): the filmer drifts closer and further,
  lower and higher, slowly through a line.

## Cruise control, mouse look and zoom

- **Cruise control** (film mode, framing a friend; `filmer cruise`, bindable): you ride at the speed
  that keeps you the gap behind them (`filmer set gap`, 3 m by default). You steer and aim; your own
  push and brake still work, and riding up on them it eases you off. It moves your speed the way the
  trainer's auto push does, so push once to get rolling. A speed boost: off in a session whose host
  has turned boosts off.
- **Mouse look** (always on): you filming, the mouse aims; the Filmer stopped (at a drop, or
  stopped by you), the mouse turns its camera (`filmer set mousesens`).
- **Zoom**: with the long lens, while the camera is held still, the mouse wheel zooms (up to 4x), as
  do `filmer zoom in|out|reset` and their binds. Riding again, it eases back out.

## The look (EFFECTS)

Three presets (`filmer preset clean|vx|worn`): **Clean** (no look), **VX1000** (the look as it comes)
and **Worn tape** (an old, dubbed-to-death tape, with the date stamp). Each part can still be
switched under Advanced (shown as Custom then).

Drawn over the game's frame by the overlay while the Filmer has the camera, one switch each:
the Century MK1's fisheye bend and its dark hood edge, 4:3, the VX's colour, standard-definition
softness with the camcorder's sharpening halo and colour bleed, interlacing, tape grain, a date
stamp and the viewfinder's REC. HDR displays get the geometry (bend, 4:3, hood, interlacing); the
colour and grain are SDR only. The game's HUD is hidden while filming (switchable).

## Marking clips

Slap the lens: a hand rushes at it and smacks it, the camera is knocked back, the iris shuts while
the palm is on the glass and overshoots as it comes off, and the clip ends. Clips are numbered and logged with their end time in
`%LOCALAPPDATA%\ReSkate\filmer\clips.csv`, so they can be found in a recording.

- While the Filmer has the camera the game's Start (pause) and View / Back (replay) buttons are off.
- A game menu up (the phone's radial menu and the rest, told from the game's own menu flag, not from
  buttons): the Filmer keeps filming, the game's interface shows over the picture without the VX look
  drawn over it, and the pad is the game's until it closes. (D-pad up, back to your marker, is no menu.) Full-screen menus are to give the camera
  back until they close once the game's flag for them is pinned down (DEBUG shows the menu data that
  changes; `filmer set menuflag <byte>` to try one).
- You film with keyboard and mouse: the mouse aims the camera whether you stand, walk or ride; WASD
  moves you. The nametags over the others are off while you film (**Hide nametags**, `filmer set hidenames`).
- DEBUG shows what the Filmer costs a frame (milliseconds and questions to the game's collision);
  its questions are spread over frames, never asked in one burst.
- The slap: LB by default (**Filmer: slap the lens** in Settings > Binds, `filmer slap`). Its button
  is kept from the game only while the Filmer has the camera.

**Filmer on / off** and **Filmer: stop and film / ride again** can be bound too.

The date and the viewfinder text are drawn by the shader in a 5x7 camcorder font, burnt in after the
lens (straight, over the hood) and before the tape (softened, interlaced, grainy).

The tape can be worn (Tape age): dropouts, head-switching noise along the bottom, a tracking band
rolling up the picture as each clip starts, and reds smearing to the right. The auto-iris lags the
light: into the sun the picture blows out for a moment, into shade it goes murky, then settles. With
the fisheye the Filmer lunges in close and low under an air, then pulls back for the landing.
Each of these has its own switch.

**Over any camera** draws the look whatever has the camera, including the replay editor's. The
Filmer's own camera does not run in replays yet.

## Commands

| Command | |
| --- | --- |
| `filmer toggle [follow\|film]` | On (as last used) or off |
| `filmer mode off\|follow\|film` | |
| `filmer subject me\|<player>` | Whom the Filmer films |
| `filmer target auto\|<player>` | Whom the assist frames while you film |
| `filmer set <option> <value>` | `style auto\|behind\|side`, `lens fisheye\|long`, `distance`, `height`, `tilt`, `human`, `reaction`, `sensitivity`, `mousesens`, `crouchdepth`, `hunch`, `bend`, `age`, `dropstrict`; switches `debug`, `cycle`, `assist`, `invert`, `recenter`, `hidehud`, `stick`, `clicks`, `drops`, `lunge`, `look`, `anywhere`, `dropouts`, `headswitch`, `tracking`, `smear`, `iris`, `fisheye`, `bars`, `hood`, `grade`, `soft`, `scanlines`, `grain`, `date`, `viewfinder` |
| `filmer plant [on\|off]` | The Filmer stops and films from where it is, or rides again |
| `filmer slap` | End the clip |
| `filmer preset clean\|vx\|worn` | The EFFECTS preset |
| `filmer clips` | This session's clips |
| `filmer trace` | Records what the filmer sees for 30 seconds to `%LOCALAPPDATA%\ReSkate\filmer\trace.csv` (for tuning) |
| `filmer status`, `filmer reset` | |

Settings are kept in `%LOCALAPPDATA%\ReSkate\filmer\filmer.json`.

## Limits

- The camera takes over the native free camera, so the Filmer, Freecam, Noclip, First person and
  Spectate take turns.
- The right stick is kept from the game through XInput. A PlayStation pad read by the game
  directly (no Steam Input or DS4Windows) still flicks while you film; with Steam Input it is fine.
- Nametags are placed for the unbent picture, so they sit a little off with the fisheye bend on.

## Files

| File | |
| --- | --- |
| `filmer_rig.*` | The operator: where they ride and where they point. Pure maths. |
| `filmer.cpp` | The game side: skaters, the camera lease, the right stick, settings, clips. |
| `filmer_commands.cpp` | The `filmer` console commands. |
| `filmer_view.cpp` | The snapshot the overlay reads (part of the overlay library). |
| (the look) | Drawn by the overlay's screen look (`Extension/UI/Overlay/screen_look.*`, also `reskate.screen.look` for plugins); `filmer.cpp` asks for it each tick. |
| `filmer_hold.*`, `filmer_held.*` | The arm holding the camera: the maths, and bending your skater's pose (sent to the others too). |
| `filmer_outfit.*` | The camcorder cosmetic in your hand while you film. |
| `filmer_page.cpp` | The FILMER menu page. |
