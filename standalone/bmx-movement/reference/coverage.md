# Movement reference coverage

This file describes the metadata inventory produced by `verify_reference.py` during initial discovery. Its unverified-behavior statements below are historical to that inventory step. Subsequent current native/runtime work is recorded in `../REFERENCE.md`, `../COMPARISON.md`, `observed-profile-v2.json` and the observed-track provenance files; the current runtime catalog now has 58/58 available fires and completed exits plus six verified empty slots.

The standalone C++ movement module is a reimplementation. The metadata inventory
does not establish complete behavior or parity with BMX Streets.

## Pinned build

The installed files changed during this investigation. A stable pair was copied
to `build/bmx-movement-current/source` and both installed files were reread and
compared after copying. The snapshot operation did not execute game code.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| GameAssembly.dll | 112962048 | 22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8 |
| global-metadata.dat | 33108228 | 7e1af5100b90a78c907999239bc6952dfdce2461fc4321626658f24f2815d7ea |

Both files had observed modification times around 2026-10-21 07:31:00 UTC. This
records local files and does not establish Steam publisher provenance.

Cpp2IL 2022.1.0-pre-release.21 with `attributeinjector` and `diffable-cs` generated
fresh declarations and addresses from that pair. It mapped 200703 method
definitions across the whole image. The output is under
`build/bmx-movement-current/cpp2il-addresses/DiffableCs`; these are empty metadata
declarations, not recovered C# source bodies.

## Bounded movement inventory

`evidence.json` records 254 metadata files and 3952 addressed method declarations
inside these exact roots:

- `Assembly-CSharp/MashBox/BMX_Physics_Development`
- `MashBox.Core.Runtime/MashBox/Core/Runtime/Physics/Vehicle`
- `MashBox.Core.Runtime/MashBox/Core/Runtime/GrindPoseData`

Filename-based groups help navigation; they do not prove movement semantics.

| Candidate group | Metadata files |
| --- | ---: |
| Bike physics and state | 154 |
| Input | 13 |
| Landing, collision and recovery | 12 |
| Manuals and grinds | 17 |
| Rider and animation | 38 |
| Tricks | 20 |

Each addressed method includes the generated declaration, metadata line and
digest, preferred RVA, PE file offset, length and exact native-byte SHA-256.
Classes and methods outside the listed roots may also affect movement.

## Historical evidence

The old build `3d304228003aeeb7e96ee7588ad92526d67eadd3c5db530f7bc0d661399d5ef1`
is different from the pinned build. All 31 selected historical rider/input and
ordinary-wheel method ranges differ at their old RVAs. Their uniquely matched
current named methods also differ in length or exact bytes. Relocated references
and compiler changes can produce byte differences without semantic changes; these
comparisons establish that historical native evidence cannot be labeled exact
current-build evidence.

The old `physics-reference.json` and `physics-typed-configs.json` source hashes
also differ for the DLL, metadata, resources.assets, globalgamemanagers and
globalgamemanagers.assets. Previously extracted settings remain historical
references until recovered again from this build. They are not confirmed active
runtime tuning.

## Current authored controls

`current-controls.json` was extracted directly from the pinned current metadata
literal table. The BMX map has 27 actions and 65 bindings; the Tricking map has
9 actions and 17 bindings. Their action and binding definitions match the old
metadata inventory after ignoring IDs.

The eight Tricking modifier actions are `LS RS`, `RS`, `LS`, `LT RT`, `RT`, `LT`,
`LS RT` and `RS LT`; the ninth action is the right `Trick Stick`. Their authored
bindings use the gamepad shoulders/triggers and `OneModifier` composites.
This supports the input interface; it does not establish current native mask
priority, slot choice, trick timing or animation parity. The old 58-slot catalog
remains historical and unverified for the current build.

## Behavior still requiring evidence

- Bike force, suspension, wheel friction and braking branches require current
  native analysis and live calibration.
- Rider IK target interfaces are inventoried; target skeleton mapping and live
  IK weights remain unverified.
- Trick gating, timing, combinations, cancellation and contact effects require
  current native and runtime evidence.
- Manual balance, grind constraints and grind exit transitions remain incomplete.
- Landing, bail and revival identities are present; live thresholds and outcomes
  remain unobserved.

Current focused REA analysis is documented separately by the main investigation.
The verifier's `current_behavior_recovered_by_this_script` count is zero because
this script only parses metadata and hashes bytes. Its callee identities can
support that focused analysis without claiming to recover its behavior.

## Recheck

Run `python tools/verify_reference.py` from the module folder with the original
local build evidence present. It regenerates `reference/evidence.json`, validates
all current RVA/file-offset pairs, compares historical evidence and checks that
the live installed pair still matches the pinned pair. Exit code 2 reports drift.
The script never loads a game DLL or starts the game. The local snapshots and
generated proprietary metadata are analysis inputs outside the standalone C++
library, and are not required to compile or run that library.
