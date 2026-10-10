# Skate 3 Hall of Meat

An optional Skate 3 style for Hall of Meat, alongside ReSkate's own (`Extension/HallOfMeat`). It does
nothing until its asset pack is installed and switched on in the MODS tab: the pack (on Thunderstore)
holds Skate 3's X-ray bone meshes, bone texture, HUD art, fonts and sounds; this repository ships none
of them. It steps aside whenever ReSkate's own Hall of Meat is switched on, so only one shows.

## What it does

- **The X-ray**: Skate 3's own bone meshes, placed on the ragdoll's physics bodies every frame from
  the crash until the skater is up. Hurt bones heat from white to orange; breaks burn red-orange.
- **The score**: Skate 3's damage model (25 body parts, six levels each, links between parts) and its
  score graphs for bail time, drop, air, speed and rotation, plus its car bonus, read from Skate 3's
  attribute database; the Thrasher panel with Skate 3's HUD rules for when each row shows.
- **A break**: Skate 3's bone sounds, red screen edges and, in single player only, slow motion
  through `Extension/HallOfMeat/hall_of_meat_slow_motion.h` (never in a multiplayer session).
- **The look**: Skate 3's `colour_matrix_hall_of_meat` (half saturation, blue x1.2) applied to the
  game picture in a small DX12 pass before the overlay draws, a spotlight, black corners and grain.
- Each map's best bail, saved with the profile.

## How it works

| Part | Files |
|---|---|
| Crash detection and scoring on Skate 3's tables; no game access | `hom_core.*` |
| The skeleton read each client tick (pose, physics bodies, contacts), the game UI hidden while it shows | `hom_rig.*` |
| The HUD, the X-ray and the per-tick tracker feed | `s3hom_hud.*` |
| The colour matrix pass (overlay render, before the HUD) | `hom_grade.*` |
| The asset pack: finding it in Mods, its images in the overlay atlas, the meshes, the sounds | `hom_art.*`, `hom_bones.*`, `hom_audio.*` |
| `hom` console commands | `hom_commands.cpp` |

Outside this folder: the client tick calls it after the trainer, the overlay draws it and records the
colour pass, the console registers `hom`, and the launcher counts `HallOfMeat/bones.bin` as content so
the pack installs from Thunderstore like any mod.

Reads go through `first_person_read` (SEH-guarded); the contact records use the layout in
`Engine/Game/Build/20260929/skater_body.h` and are only read once its fingerprints match. With the pack
off, nothing of the skater is read.
