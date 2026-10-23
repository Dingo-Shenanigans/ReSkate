# Animator Studio

An in-game animation editor (F8, or L3 + R3 held on a controller). The current runtime applies
saved authored keys over native gameplay motion; saving a recorded clip does not replace the
complete native animation. Source clips remain inspectable in the studio, where raw native
clips can lack the final board/IK constraints. See `animation-native-playback-v2.md` for that
backend, and `animation-native-clip-pipeline.md` for the new offline full native-source replacement
experiment. Native compilation is not yet connected to the F8 Save action.

## How to use it

1. F8 (L3 + R3): a loading screen, then the studio in the game's own lit studio room, the skater
   standing on his board.
2. TRICKS > Record a trick: the studio steps out (a strip across the top says it records). Skate and land
   the trick; you are back in the studio with it at its first frame. Esc or Back: back without one.
3. JOINTS: pick a joint or the Board and turn it (drag, or the left stick); G moves the hips or the board,
   X / Y / Z lock an axis. Keys sit on whole frames.
4. KEYS: key this frame, Select all keys (Ctrl+A; Shift+click adds one), drag one to move all, Del takes them off.
5. Save (Ctrl+S / Menu). Do the trick in game to test the authored keys over native motion.

## How it works

The implementation notes below describe the historical capture/render replacement prototype;
the current gameplay edit backend and native compilation experiment are documented above.

- **Which trick**: the game keeps the name of the trick in the air ("ollie", "kickflip", "heelflip") at
  the skater's animation holder (component +0xa0) +0x20 -> +0x250 -> +0x98. It is there from about
  0.05 s after takeoff. A saved animation carries that name.
- **Recording**: the skater and his board are kept as the game draws them, at their render publication
  (after the body physics and the feet's placement on the board), from 0.7 s before the takeoff to 0.7 s
  after the landing. So the board is where the game had it, under the feet, in every frame.
- **Pose**: the skater's evaluated pose (395 joints, `read_native_pose_layout`) is written when it is
  handed to the renderer (the render pose hook in `native_skater.cpp`); written right after the
  animation update it does not survive the body physics. Only Hips and the joints below it are replaced.
- **Board**: the board rig's place in the world (bone 1) is set at the board's render publication, so
  the deck, trucks, wheels and a pack deck's pieces all move together. The board's keys and its fields
  work from where the board starts: 0, 0, 0 is the board at the trick's first frame, on the ground, as it
  lies there (Y up).
- **Pop**: at takeoff the board's upward speed (its root physics body) gives the length of the air; the
  saved animation's air is stretched to it, and an animation saved for that pop (low, mid, high) is chosen
  over one for any pop.
- **The room**: the studio shows the skater in the character room of the game menu's Skaters page (lit
  only while that page is open). On open it walks the game's menu there with the menu's own navigation
  events and RB on a scripted pad (after PR #230 by Norms: `studio_room.*`, `Extension/UI/Overlay/scripted_pad.*`),
  hides the menu's own skater and board, and draws ours on the room's floor; on close it walks back. If
  the walk fails the studio opens where the skater is (PLAYBACK > Studio room turns the room off).
- **On the floor**: the studio pins the board's lowest point (its wheels, or a tip when it tilts) to the
  floor before the pop and after the landing, follows the clip's own rise in the air, and never lets the
  board under the floor. Until a trick is picked the skater stands on his board in a casual stance.
- **Studio**: Freecam with an orbit camera around the skater, the game's HUD hidden, the skater slowed to
  a stop (horizontal body velocity, every physics step), the hands' attach joints made tiny (the props the
  game hangs there while he idles). Everything is given back on close.
- **Other players**: a saved trick playing in game goes into the pose ReSkate sends of the local skater
  (`multiplayer::set_local_pose_filter`, after PR #230).
- **Loading screen**: skate.'s look, with its own pictures read from the game's data into the overlay's
  atlas (`animation_studio_images.*`): the grain, the brush bar, the rough tile and the kickflipping board
  of skate.'s loading screen (`flip_Skateboard_Kickflip_256`, 27 frames, 9 x 3).

## Files

- `Extension/AnimationStudio/anim_core.*`: keys, blending, the profile (JSON) and clip (`FAS3`) files.
  Tests: `DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS`.
- `Extension/AnimationStudio/anim_studio.*`: the game side. `studio_room.*`: the walk to the room.
- `Extension/UI/Overlay/animation_studio_overlay.cpp`: the screen (the game's Trick List look), the
  loading screen and the recording strip. `animation_studio_images.*`: the game's pictures for them.
- `contrib/animator-studio/make_trick_library.py`: makes a list from the game's own clips with
  `reskate_cli animation-export` (98 clips; read from `ReSkate.Tricks/library/`). The list is hidden for
  now: in flips the game places the board with its physics, which the clips do not hold.

Saved animations go to `ReSkate.Tricks/` beside Skate.exe.

## Local prototype: animation packs and transition research

ANIMATIONS > Create a new pack copies the current pack's choices, or starts with all saved
animations. Active pack cycles the available packs. Variant playback chooses Fixed (one chosen
variation per trick/pop) or Alternate (one of the linked variations at random on each attempt, never the same one
twice running).
PLAYBACK > Save as new variation preserves the original file. Include saved variation in pack
changes membership; Choose this fixed variation changes that slot's preferred choice. These
actions use the saved animation, so save edits first. A new variation is not automatically added
to an existing pack. Each base stance/switch combination has its own fixed choice. Gameplay
plays only recordings with the same base stance and switch state at takeoff. In another stance the
trick keeps its native animation, because the mirroring between stances is in development.
Native bone positions/scales are retained except for explicitly authored
offsets; board keys are added over the native board in the placement frame (and stay native
on rails). This transfers edits, not all of
the recorded base motion. Unknown live switch state keeps native animation. Legacy recordings
without switch metadata are treated as normal stance.

To know if the skater is riding switch, the studio compares two things:

- which foot leads right now, read from the game (`left_foot_leads_mask` in `grind_exit.h`)
- the skater's own stance, from the stance setting (`grindexit stance goofy|regular`, unset counts as
  regular)

When the two differ, the skater is riding switch.

Recordings made for regular-footed skaters before 2026-10-20 carry inverted switch tags. Record them
again, or fix them (about 1 minute):

1. Run `python contrib/animator-studio/fix_regular_skater_tags.py --tricks ReSkate.Tricks --output NEW_FOLDER`
2. Check `fix-regular-skater-tags-report.txt` in `NEW_FOLDER`
3. Close the game and back up `ReSkate.Tricks`
4. Replace `ReSkate.Tricks` with `NEW_FOLDER`

The script never changes `ReSkate.Tricks` itself.

SAVED > NEW PACK NAME accepts a pack name such as Steezy; Create a new pack creates and selects
it without replacing an existing pack. These are local packs, not an online marketplace.

TRICKS now exposes decoded native clips and a Native asset tree. The tree reads
`library/native-catalog.json`, generated by `index_native_assets.py` from a CLI find-name dump.
It is paged (50 leaves) and includes graph states/transitions/parameters as well as candidate
clips. Categories and kinds are name-based hints, not verified engine types. Editable means
there is a converted clip; Indexed means decoding/binding is still needed. Native clips may
lack physics-driven board placement and graph/IK constraints. Grind preview clips can be edited
and saved, but are not automatically installed as live grind phase overrides.

Packs live in `ReSkate.Tricks/presets/`, with the selected pack name in `current.txt`. They reference
the existing `.json`/`.clip` animation pairs by name; sharing a pack also requires those pairs.
Fixed choices follow the pack's order, skipping unavailable files. Alternation follows stable
filename order and advances once per named trick per attempt; it resets when the game restarts.
An exact-pop variant takes precedence over an any-pop fallback. With no pack, one alphabetical
variant per trick/pop is used. Switching packs does not change a trick already in flight.

`animstudio trace start` records a local, bounded 60-second observation window;
`animstudio trace stop` exports early and `animstudio trace status` reports progress. CSVs go under
`ReSkate.Tricks/research/`. State/name/mode changes are kept every engine tick, with periodic
samples at up to 20 Hz. Export happens on stop, not on animation/render workers. No new game
memory is written by the recorder. A failed export retains samples for a retry.

The trace observes the already guarded physics selector, trick-name string and board vertical
velocity. It does not yet read current graph node/phase, grind enum, rail constraints, IK weights
or blend weights. `rail_range` means only a physics-state value in 400..499. `other` must not be
interpreted as ground. `custom_playback_enabled` is an option, not proof an override was applied.
The goofy flag is the configured stance, not a newly measured runtime stance. Physics changes
between engine ticks can be missed. See `animation-research.md` for the next measurements.

## Later

- Recording every trick in one go (a practice stage, the game's own stick gestures, a low, mid and high
  pop each), to bring the trick list back with the board as the game draws it.
- Grabs, grinds and manuals: the game names them too.
