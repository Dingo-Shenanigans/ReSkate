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
