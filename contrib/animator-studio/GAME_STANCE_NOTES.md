# How the game picks and turns its trick animation by stance

Read out of the game's own animation graph (the typed native documents the library export writes:
`native-catalog.json`, `native-documents/`, `native-links.jsonl`), 2026-10-22.

## One switch mirrors the whole skater

- `Bool.Anim.Mirrored` is the only condition of all 35 `MirrorableControllerAsset` nodes. Those wrap
  whole layers: `SuperLayer_Skate_InAir_Trick_(Mirrorable)`, `SuperLayer_Skate_OnBoard_(Mirrorable)`,
  `SuperLayer_Skate_OnBoard_Land_(Mirrorable)`, `.SF_OffBoard_(Mirrorable)`, the idle blendspaces, and more.
  When it is on, everything under them (body and board) is played mirrored.
- It is not worked out by the graph. Physics writes it: `SkaterCorePhysicsInputConfig.AnimMirrored ->
  Bool.Anim.Mirrored`, next to `IsGoofySkater -> Bool.Style.SkaterGoofy`, `IsSetupFakie`, `IsSetupSwitch`,
  `AnimCurrentFlipTrick`, `AnimBoardBackwards`.
- `CAS_Stance_Regular` requires `Bool.Anim.Mirrored` and plays `SuperLayer_CAS_Stance_(Mirror)`;
  `CAS_Stance_Goofy` plays the unmirrored layer. **The game's clips are authored goofy; regular is the
  mirror of them.** (Matches what was seen on screen.)
- When mirrored, `Board_Joint_RotateBoardBackwards` turns the board joint half a turn about up
  (quaternion 0, 1, 0, 0) in the Skatepedia view.

## Nollie is other clips, fakie is a layer, switch is the mirror

- Nollie: `EnumGS.Anim.CurrentFlipTrick` (from physics) picks the clip family. Flip tricks come as
  `c_proto_onb_trick_o_*` (ollie, popped off the tail) and `c_proto_onb_trick_n_*` (nollie, off the nose),
  each in low / mid / high with light / dark / uflip variants. There are no fakie or switch flip clips.
- Fakie: not a mirror. `EBool.FakieTrickLayer = Bool.Physics.RidingFakie AND Bool.State.Is.Trick` enables
  layer 10 of `SuperLayer_Skate_InAir_Trick`: an additive, upper-body-masked
  `BlendSpace_OnBoard_Idle_Regular_To_Fakie_Pose_Addi`, weighted by `Float.Anim.Layer.FakieTrick.Weight`,
  a curve each trick clip writes (450 clips do). Same trick clip, same feet, the upper body turned.
- Switch: the other foot forward, so the other mirror state of the same clips.

## What that means for the studio

- Mirror a recording when the foot it had at the board's nose differs from the foot at the nose now.
- Never mirror for fakie: same clip, same feet; the game's own fakie layer is already in the native pose
  our additive keys ride on.
- Nollie and ollie tricks are different clips and stay separate recordings.
- Open question, measured by `animstudio stance` (writes `research/stance-readout.txt`): which physical
  fact physics uses for `AnimMirrored`, and whether our stance bit (`grind_exit::left_foot_leads_mask`)
  flips when riding fakie. If it does, it is not the mirror bit and must not decide mirroring on its own.

## Measured 2026-10-22 (stance readout, regular setting)

- The bit we used as "left foot leads" (stance byte `0x20` at context+0x13c4) is NOT the stance. It stayed
  set riding normal, fakie and switch alike, and only cleared during some pushes from switch or fakie
  (byte `10`/`50`). `0x10` and `0x40` come and go while pushing. Nothing should decide mirroring from it.
- Riding fakie the feet stay at the same ends of the board and only the travel reverses; a 180 shuvit turns
  the board under the feet. Neither changes which foot the game treats as front, so neither may mirror.
- Next readout (v2) logs the feet in the placement frame (where the game mirrors the body) and snapshots
  the physics context, to find the game's own Mirrored / RidingFakie / BoardBackwards flags.

## Found in physics memory 2026-10-22 (readout v2, physics context = core+0x3c0)

- `context+0x13c5` bit `0x20`: **board backwards**. Set exactly between two pop shuvits, clear otherwise.
  Bit `0x40` of the same byte: in the air.
- `context+0x17fc` float: **forward speed relative to the body**. Negative when riding fakie, including
  with the board backwards (where the board itself travels nose-first). So fakie = this < 0.
- In the pose after the animation update the regular rider's left foot is on placement +X (mirrored) in
  normal, fakie and after shuvits. Recordings, taken from the pose at render time, almost all show the
  right foot on +X. Readout v3 logs both at once to see whether the game mirrors between the two.

## Readout v3 (2026-10-22): the mirror is the 0x20 bit after all

- Both copies of the pose (after the animation update, and at render) agree on the side, one frame apart.
  The game does not mirror between them.
- Riding switch (reached by pushing while fakie) the 0x20 bit is clear and the right foot is on +X
  (unmirrored); normal and fakie have it set and the left foot on +X (mirrored). So 0x20 at context+0x13c4
  IS the game's mirror for a regular rider, as Norms read it. The first readout was misread: what looked
  like switch there was the board turned backwards. Entering switch the game also sets board backwards and
  the body-relative speed changes sign: the body turns round on the board.
- The skeleton has `MirrorByBehavior: false`: the engine mirrors by reflection, not by copying channels.
- Joint names are in the skeleton's NameTable (0 Reference, 1 AITrajectory, 7 Hips, 10 RightFoot, 343
  LeftFoot, 375 Ground, 376-379 ankle/wrist effectors, 380 Skateboard_Root, 390-394 deck effectors).
- Still to check: that our mirror maths matches the game's. Every one of the 299 recordings so far was made
  unmirrored, so one recording made riding normal is needed to compare against the library.

## The game's mirror plane (2026-10-22, heelflip recorded riding normal)

- The recording matches the library heelflip reflected across the skater's **Z = 0** plane (AITrajectory
  frame) exactly: median distance 0.0018, minimum 0.0000. Reflected across X = 0 (what the studio did) it
  scores 0.275. Per joint that is rotation (x, y, z, w) -> (-x, -y, z, w), position z -> -z, left/right
  swapped; it equals reflecting the whole skeleton (the left/right table keeps the hierarchy).
- Z = 0 holds the board's long axis: the feet keep their ends of the board, swap, and he faces the other
  side. The board is not turned end for end; a board key (board frame) reflects across.
- The clip's board is the animation's Skateboard_Root, which the game turns round when mirrored; the board it
  draws (physics) has its nose along the skater's +X unless the board is backwards. Recordings made mirrored
  or backwards (most old ones were made in switch reached by pushing from fakie, which also sets board
  backwards) therefore carry a board pointing the other way; the studio turns such a clip's board round.

## The pipeline, as it stands (2026-10-22)

At takeoff the studio reads everything the game gives and keeps it with a recording too:

| Trigger | Read from | What it does |
| --- | --- | --- |
| Lead foot (regular / goofy / switch) | live: stance bit `0x20` at context+0x13c4; recording: its clip (`clip_leads_right`) | the game's mirror; a recording on the other foot is mirrored (Z plane) |
| Fakie | body-relative forward speed (context+0x17fc) while riding, < -0.3 | never a mirror; recordings keep `fakie`, and a fakie one is preferred rolling fakie |
| Nollie | the game's trick name (`nollie`, `n_kickflip`...) | its own trick, its own recordings |
| Board backwards (true switch vs fakie-push switch, after a shuvit) | context+0x13c5 bit `0x20` | the board's key frame and the recording's board are turned with the game's board |
| Pop height | the game's grade | the graded pop's recordings come first |
| Entry | ground or rail before takeoff | must match |

One rule picks the recording (`choose_stance_variants`, tested over every combination): exact stance takes only
the same lead foot and fakie state, never mirrored; front foot takes any, closest first - pop, then fakie state,
then lead foot (mirrored if it differs). In game the whole skater and the board play the recording with its
keys, spread over the air that happens, keyed joints within the measured limits. These are fixed, not options:
set wrong they broke tricks invisibly.

## Checked in game 2026-10-22 (playback verifier)

A test build reads back, every frame a saved trick is fully on, the pose the game actually draws and the
board as drawn, and compares them with what the studio shows for that moment (the recording with its keys,
mirrored when played mirrored, joints in their limits). Set `RESKATE_PLAYBACK_VERIFY=1`; one line per trick
goes to `ReSkate.Tricks/research/playback-verify.txt`. Tricks were done by the virtual controller
(`RESKATE_TEST_PAD=1`, console `testpad`), on a skater whose right foot leads riding normal.

| Recording | Rider at takeoff | Played | Body max | Hips | Board | Feet on board |
| --- | --- | --- | --- | --- | --- | --- |
| right foot | right foot, normal | as made | 0.06-0.28 deg | 0.0-0.1 cm | 0.00-0.04 deg | 0.0-0.1 cm |
| right foot | right foot, fakie (after a 180) | as made | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |
| right foot | right foot, board backwards (after a pop shuvit) | as made | 0.06 deg | 0.0 cm | 0.04 deg | 0.0 cm |
| left foot | right foot, normal / fakie | mirrored | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |
| left foot | right foot, normal / fakie, board backwards | mirrored | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |
| right foot | left foot (switch from a fakie push), board backwards | mirrored | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |
| right foot | left foot, fakie, board backwards | mirrored | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |
| left foot | left foot (true switch), exact stance | as made | 0.06 deg | 0.0 cm | 0.00 deg | 0.0 cm |

The game's own pose was 16.6-18.8 deg away from the studio's in every case, so the numbers above are ours,
not the game happening to agree. Exact stance left a fakie kickflip and a mid-pop kickflip (only high pop
was recorded) to the game, and a nollie kickflip arrived as `n_kickflip`, so the kickflip recording stayed
off it.

What the game does that matters here:

- Pushing sets `0x10` and flips `0x20`/`0x40` for about a second while the foot kicks. It clears the moment an
  ollie starts, so a trick done straight out of a push still reads the right foot.
- The game's own setting *Hold Fakie Push* (Gameplay > Controls) decides switch: on, pushing while fakie stays
  fakie; off, it puts him in switch with the board backwards. A 180 from there gives true switch (board
  forward). Pushing while switch-fakie brings him back to his own foot with the board backwards.
- The flick sticks follow the lead foot: the kickflip flick riding switch is the heelflip flick riding normal.
