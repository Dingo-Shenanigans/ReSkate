# Native animation catalog and named presets

The desired workflow is choose a native motion, edit it, name the result, add it to a named
pack, then choose a fixed variant or alternate compatible variants. Recording remains useful
for physics-driven body/board relationships; it should not be the only entry point.

## Implemented locally

- Native asset inventory in an in-game paged category tree. The initial snapshot has 17,258
  distinct EBX assets under animation/dingo. This does not claim the whole game's animation
  inventory: other namespaces, procedural movement and graph-generated blends remain outside it.
- Classification hints for tricks, grind entry/loop/exit, locomotion, small movements, off-board,
  gestures, bails, graph states, layers, transitions and parameters. Hints come from names and
  need type verification; they are not proof of live ownership or editability.
- Converted native trick clips are visible again. The existing extraction script enumerates
  33 trick families x 3 pops; failed exports are skipped and reported, never counted as editable.
- A small native grind preview exporter: no invented takeoff/landing markers, and preview-only
  catalog entries are not silently enabled in gameplay. The initial examples are 50-50 entry,
  loop and frontside pop-out to air.
- Named local packs (e.g. Steezy), multiple variants and fixed/alternating selection, with distinct
  choices for base stance/switch. Name collisions do not replace existing packs.
- Switch transfers authored body edits onto native movement when there is no exact-stance
  recording. The native board remains the base; explicit board keys are applied in the placement
  frame away from rails. This does not solve foot/rail IK or transfer the entire recorded motion.

## Required before all movements can be replaced safely

Each editable item needs a stable native asset identifier and an explicit runtime binding:
state/phase, actor (skater or board), skeleton version, blend layer, additive/absolute semantics,
loop boundaries, stance, hand/foot contact constraints and entry/exit conditions. A transition
is often a graph edge with conditions and an inertial blend, not a standalone animation clip.
For grinds, measure entry/lock-in, loop, pop-out/no-pop exit and landing independently. Integrate
the body and board under native rail/IK constraints before allowing gameplay overrides. Do not
stretch one airborne clip across all these phases.

Native FBX conversion currently reconstructs a clip at 60 Hz and infers trick takeoff/landing
from board height. That heuristic is not a general phase detector and is deliberately not used
for grind previews. Additional native clip families need their own verified markers/bindings.

## Sharing and marketplace design (not implemented)

Keep local editing independent of accounts or network availability. First provide portable
pack export/import and local previews, then add an online catalog as a separate service.
Published items need a stable ID (independent of display name), revision, author attribution,
description/tags, game/skeleton compatibility, required asset IDs, payload checksums and a
preview. A pack references exact variant revisions and declares fixed/alternate rules.

Prefer distributing authored keys/settings and native asset references; clients reconstruct
base clips from their own installed game. Validate missing assets, malformed/nonfinite keys,
duration/phase mismatches, duplicate IDs and archive paths before import. Preview before applying,
keep the previous local revision for rollback, and never execute code supplied by a preset.

The current local pack format (version 1, name-based JSON/clip references) is a prototype.
It needs migration to stable IDs and phase bindings before it can support reliable online
updates, dependency resolution or grind replacement. Authentication, hosting, browsing,
uploads, moderation, ratings and payments are not present in this build.
