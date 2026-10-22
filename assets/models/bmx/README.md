# Private native BMX geometry

"User-supplied" in the prototype credits describes the source model provided for
development. It is not an end-user setup instruction. The running game loads the
converted native pack from `Mods/BMX_Prototype`; it does not load the source FBX
or require a `Downloads/bmx-free` folder.

The current local pack is not yet part of the standard ReSkate release packaging.
The release requirement is to include a distributable bike pack and its attribution
with BMX, then verify installation on a clean machine without the source model or
conversion tools. Players should only need to install the mod.

The current bike uses the supplied `bmx-free` model as eight native Frostbite object
blueprints: frame, fork, rear wheel, front wheel, cranks, handlebars and two pedals.
`Extension/BMX/bmx_world.cpp` places those world
entities from the bike state. The older `bmx_visual.cpp` overlay renderer is no longer
called by the game rendering path. Entity and placement counters establish native
calls; they do not establish visible pixels, material quality or correct occlusion.

The earlier `reference/bike_geometry.bin` contains geometry extracted locally from the user's
`D:\SteamLibrary\steamapps\common\BMX Streets` installation. Its ignored directory
contains the original manifest, OBJ exports, materials, transforms and assembly records.
These assets retain their source ownership and are private local test inputs. Generated
packs and extracted files must not be committed or redistributed without permission.

That earlier extracted modular geometry was supplemented by procedural rims, spokes, hubs, a shaped saddle,
grips and pedals. The fork uses the exported frame/fork anchors; the crank and rider foot
targets share the frame's bottom-bracket location. Grip sleeves and hand contacts share
the exported bar-end positions and swept axes through `Extension/BMX/bmx_grip_anchors.inc`.
Preparation intersects the imported bar triangles at three sections inside each sleeve,
rejecting detached grips. Other added attachments remain prototype authored. The native pack uses an
existing opaque native material program, with UVs sampling uniform dark rubber and neutral
metal areas of its atlas. Compact axis-angle tangent bases are encoded from surface normals
and tangent directions; wheel corners are smoothed within a 50-degree crease. The normal
decoder was independently matched to all 2,386 authored normals in the donor frame by
position and Half2 UV. Independent full-basis tests use the published shader's cross-product
and handedness conventions. This removes the previous coarse lookup of donor directions.
Original shader programs and mask textures are retained; their rendered tire appearance
still needs inspection. An ignored UV-only control retains an earlier tangent stream.
Both face windings are present while the cross-engine geometry contract is tested.
This is an initial native content pack; appearance still requires in-game inspection.

## Rebuild the earlier reference pack

Build the `dingosdk_bmx_native_pack_builder` CMake target, then run from the repository:

```powershell
.\contrib\bmx-lab\Build-BmxNativeAssets.ps1 -GameRoot 'D:\Games\ReSkate'
```

The script verifies the supported game executable, unpacks the bundled FPV template into
a fresh workspace directory, validates its native mesh declaration, prepares the six
parts and writes a new pack under `build/bmx-native-assets/generated-*/pack`. It reads
the supported game and never installs the output. With the isolated game closed, copy
the pack contents into `build/bmx-game/Mods/BMX_Prototype` for development testing.
The normal Steam installation is not a destination.

The template carries its existing native shaders/materials and its original FPV model
credits. The builder retains those credits in `BMX-NATIVE.txt`. The original game and
template are read-only inputs. The pack also includes the donor's unchanged authored LOD
group from the supported city bundle, so the root-domain meshes can resolve their dependency.
Only the populated surface and depth sections belong to the mesh's render categories;
empty material slots must not become ray-tracing geometry. The independent part validator checked stream ranges, UV/tangent
ordering, bounds, indices, depth geometry and unique chunk identities.

Tools in this directory:

- `extract_unity_reference.py`: export selected explicit local UnityFS bundle paths.
- `build_reference_geometry.py`: assemble OBJ references into the private geometry binary.
- `prepare_native_parts.py`: prepare native triangle, mesh resource and geometry files.
- `preview_reference.py`: render an offline geometry preview, not a game screenshot.

## Supplied replacement model

The user supplied `C:/Users/Jeffr/Downloads/bmx-free/source/BMX_sf.fbx` on October 18.
It contains a complete bike and supplied textures, including both wheels, bars,
cranks and pedals; it has no rider or rider animation rig. The source was preserved.
The private conversion in `build/bmx/free-model-inspection/intermediate` contains
OBJ/MTL, all nine unchanged textures, and exact polygon-wedge UV/normal tables.
Independent validation passed 13 geometry/attribute checks over 206,424 triangles.
`build/bmx/free-model-inspection/albedo-preview.png` is an offline preview.

The replacement is installed only in `build/bmx-game/Mods/BMX_Prototype`, with the
previous pack and runtime backed up under
`build/bmx-native-assets/free-model-20261018/rollback-installed`. Preserving its
proportions at a .34 m wheel radius gives a 1.203956 m wheelbase. The shared
`Engine/Game/BMX/bmx_geometry.h` now supplies measured axle, steering, grip, crank,
pedal and carry anchors to physics, rendering and rider posing. Separate pedal
platforms orbit the bottom bracket while staying level with the bike.

The conversion preserves 206,424 triangles, source UVs/normals and source winding.
It uses a 4096 by 2048 albedo atlas with 13 BC1 mip levels, a flat BC5 normal, and
the donor's native shader and mask. The source normal and PBR maps are preserved
as intermediate data but are not yet bound with validated native semantics.
Independent emitted-pack checks verified the CAS payloads, mip offsets, existing
material bindings and all eight mesh declarations. Run 103 passed the isolated
native ride and Superman rehearsal: first-attempt mount, eight parts, 541 accepted
physics steps, 541 rider render writes, a catch before landing and one clean credit
after 66.7 ms of actual wheel support. Its independent review passed all 24 checks.
Visible material and rider quality still require player inspection. The conversion
manifest records the explicit supplied-base-texture fallback used for the source's
missing foreign-folder `Rul` texture reference.

To prepare a fresh private replacement pack from the validated intermediate:

```powershell
.\contrib\bmx-lab\Build-BmxFreeModel.ps1 -Python 'C:\Users\Jeffr\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
```

This reads the supported source installation and writes a new workspace output;
it does not install it. Geometry and texture validation run before assembly.

The reference binary format is `RSBMX001`, little-endian version 1, record stride 40,
triangle count and six float32 bounds. Each record contains a u32 part ID and nine
float32 coordinates. Native preparation rejects unsupported versions, invalid parts,
nonfinite coordinates, malformed lengths and 16-bit index overflow.

## Rider and motion

The rider adapter reads Skate's actual skeleton and bind orientations, solves the owned
local rider's arms/legs toward the grips/pedals, and reapplies the pose at the render
handoff. Hands use the rendered grip midpoints, offset wrists, explicit palm orientation
and the native finger chains curled around the tube. The relaxed stance is higher and
elbows bend down with a small outward clearance. Contact metrics measure a palm proxy
and ankle joints; they cannot establish skinned hand, clothing or shoe appearance.
Pedaling follows the bike crank phase; preload, weight shift and airborne
extension are bounded posture cues. Only the owned rider's primary chain is changed.
The fresh-pose lease expires after 200 ms and native animation resumes after release.

While BMX is active, a guarded callback hides the associated 16-bone native skateboard
through render-bone scales 2–15. Reference/trajectory roots 0/1 remain native. Run 52's
read-only snapshots verified both roots stayed intact and all 16 scales returned to unit
values while skating after release; no cached pose is restored. Board Break, clothing/body proportions and visual transitions
still need in-game inspection.

BMX Streets' authored input map and paired pedaling/preload/hop clips were recovered
separately under `build/bmx-streets-controls`. They inform the controls and posture cues.
Unity humanoid channels have not been fully retargeted to Skate's skeleton, and native
BMX Streets physics parameters have not been recovered. See
[the controls evidence](../../../contrib/bmx-lab/controls-and-reference.md).
