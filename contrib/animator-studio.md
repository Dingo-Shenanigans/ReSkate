# Animator Studio

An in-game animation editor (F8, or L3 + R3 held on a controller). Pick one of the game's tricks from
the list, change the skater's joints and the board on a timeline, save it, and from then on doing
that trick in game plays your animation instead of the game's. No game file is changed.

## How it works

- **Which trick**: the game keeps the name of the trick in the air ("ollie", "kickflip", "heelflip") at
  the skater's animation holder (component +0xa0) +0x20 -> +0x250 -> +0x98. It is there from about
  0.05 s after takeoff. A saved animation carries that name.
- **Pose**: the skater's evaluated pose (395 joints, `read_native_pose_layout`) is written when it is
  handed to the renderer (the render pose hook in `native_skater.cpp`); written right after the
  animation update it does not survive the body physics. Only Hips and the joints below it are replaced.
- **Board**: the board rig's place in the world (bone 1) is set at the board's render publication, so
  the deck, trucks, wheels and a pack deck's pieces all move together.
- **Pop**: at takeoff the board's upward speed (its root physics body) gives the length of the air; the
  saved animation's air is stretched to it, and an animation saved for that pop (low, mid, high) is chosen
  over one for any pop.
- **Studio**: Freecam with an orbit camera around the skater, the game's HUD hidden, the skater slowed to
  a stop (horizontal body velocity, every physics step). Everything is given back on close.
- **On the floor**: the game's clips are in place (the physics makes the jump), so the studio pins the
  clip's board to the floor (the skater's Ground joint, 375, taken when the studio opens; the board's mount
  rides 0.086 m above it) before the pop and after the landing, and moves it along a jump's arc as long as
  the clip's air in between. The skater shows on his board on the ground however the studio was opened.

## Files

- `Extension/AnimationStudio/anim_core.*`: keys, blending, the profile (JSON) and clip (`FAS3`) files.
  Tests: `DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS`.
- `Extension/AnimationStudio/anim_studio.*`: the game side.
- `Extension/UI/Overlay/animation_studio_overlay.cpp`: the screen (the game's Trick List look).
- `contrib/animator-studio/make_trick_library.py`: makes the TRICKS list from the game's own clips with
  `reskate_cli animation-export` (98 clips: 33 flip and nollie tricks, low / mid / high; about 37 MB). The
  studio reads it from `ReSkate.Tricks/library/` beside Skate.exe.

Saved animations go to `ReSkate.Tricks/` beside Skate.exe.

## Seen working (test copy of the game)

- Opening the game's kickflip from the list, changing it, saving: real kickflips then play it, with one
  board; other tricks stay the game's.
- A boned ollie: 8 of 8 ollies played it, 4 kickflips stayed the game's.
- Opening on foot, riding and mid-air (always shown on the board on the ground), scrubbing, keys on whole frames, undo, save and load, the close warning, every screen size
  from the layout's scale.

## Not yet

- Controller input in the studio is written but was not tried on a real controller.
- The pop guess is close on mid pops (0.58 vs 0.57 s) and long on high ones (0.83 vs 0.62 s).
- Grabs, grinds and manuals: the game names them too, but the list only has flip and nollie tricks so far.
- Other players most likely see the game's own animation.
- `kickflip mid` does not export (`reskate_cli`: "Animation keys must be finite, ordered, and inside the clip").
