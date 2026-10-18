# Animator Studio

An in-game animation editor (F8, or L3 + R3 held on a controller). Record a trick, change the skater's
joints and the board on a timeline, save it, and from then on doing that trick in game plays your
animation instead of the game's. No game file is changed.

## How to use it

1. F8 (L3 + R3): a loading screen, then the studio in the game's own lit studio room, the skater
   standing on his board.
2. TRICKS > Record a trick: the studio steps out (a strip across the top says it records). Skate and land
   the trick; you are back in the studio with it at its first frame. Esc or Back: back without one.
3. JOINTS: pick a joint or the Board and turn it (drag, or the left stick); G moves the hips or the board,
   X / Y / Z lock an axis. Keys sit on whole frames.
4. KEYS: key this frame, Select all keys (Ctrl+A; Shift+click adds one), drag one to move all, Del takes them off.
5. Save (Ctrl+S / Menu). Do the trick in game and yours plays.

## How it works

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

## Later

- Recording every trick in one go (a practice stage, the game's own stick gestures, a low, mid and high
  pop each), to bring the trick list back with the board as the game draws it.
- Grabs, grinds and manuals: the game names them too.
