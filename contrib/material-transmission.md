# Material transmission (experimental)

This renderer adds screen-space transmission to an authored material: a private replay of its draw supplies surface position, normal, thickness and print coverage. At presentation the runtime refracts and tints the scene behind that surface, preserving foreground objects and the native lighting of printed artwork. The original draw still runs. Materials without declarations keep their existing rendering.

This is a rendering building block, not an SDEC-specific effect or a Luau shader compiler. A mod supplies its own compatible DXIL mask program and a declaration. It needs no per-board DLL. Native material inputs and mod shader coexistence are separate changes.

## Declaration

Place `reskate-transmission.json` in the enabled mod's root. Declarations are read from the launch catalogue once; restart the game after changing them. Saving the launcher selection or reloading a plugin does not refresh these recipes.

```json
{
  "schema": 1,
  "surfaces": [{
    "shader": "<64 lowercase hexadecimal characters identifying your source pixel shader>",
    "mask": "shaders/glass-mask.dxil",
    "absorption": [20, 4, 2],
    "ior": 1.52,
    "maximum_offset": 12,
    "pigment_depth": 0.0006,
    "coat": 1,
    "reflection": 1
  }]
}
```

The shader value above is a placeholder, not a usable fingerprint. `transmission_shader_key` computes SHA-256 over the source DXBC container's `DXIL`, `ISG1`, `OSG1`, `PSV0` and `SFI0` payloads, in that order. Each payload is preceded by its four-byte name and unsigned 32-bit little-endian length. All five chunks must exist. Debug/reflection packaging is excluded; changing executable code or its interface changes the key. This matches a pixel program, not an item ID: every compatible draw using that exact program can be replayed. Use a distinct authored program when only one material should participate. This is not a proof of item ownership and does not grant any cosmetic entitlement.

Higher-priority launch catalogue entries retain duplicate shader keys. A malformed mod declaration is rejected as a unit. The launch-wide count and byte budgets also admit a mod atomically; earlier valid mods remain active. Missing declarations opt out.

| Setting | Default | Allowed | Meaning |
| --- | --- | --- | --- |
| absorption | [20, 4, 2] | Three numbers, each 0–200 | Per-channel attenuation per unit of view-space travel; larger red absorption produces a cooler transmitted image. |
| ior | 1.52 | 1–2.5 | Refraction index and Fresnel reflectance. |
| maximum_offset | 12 | 0–32 | Maximum screen displacement in pixels on each axis. |
| pigment_depth | 0.0006 | 0–0.03 | Effective travel through printed artwork, clamped to local thickness. |
| coat | 1 | 0–1 | Coat contribution over print. |
| reflection | 1 | 0–1 | Screen-space reflection strength. |

## Mask program contract

Compile a pixel shader compatible with the source draw's existing vertex outputs, root signature, descriptors and constants. Replay does not add bindings. The mask writes these four full-channel output registers; no additional color outputs are accepted:

| Output | Type | Values |
| --- | --- | --- |
| SV_Target0 | float4 | XYZ: view-space surface position with positive Z away from the camera. W: zero outside the surface, 1 for perspective or 2 for orthographic projection. |
| SV_Target1 | float4 | XYZ: view-space surface normal. W: thickness in the same view-space units, clamped to 0–0.03 by the compositor. |
| SV_Target2 | float4 | W: coverage of native printed artwork, 0 for transmissive material and 1 for print. RGB is reserved; native scene color supplies the lit print. |
| SV_Target3 | uint4 | Bit-cast floating-point depth and projection calibration: (A, B, projection scale X, projection scale Y). |

For perspective projection, the raw depth must follow `depth = A + B / viewZ`; for orthographic projection it must follow `depth = A * viewZ + B`. Projection scales map view-space X/Y (divided by Z for perspective) to texture coordinates relative to 0.5; include the texture Y-axis sign. Supply calibration from the actual draw camera, not a different gameplay or preview camera. An HLSL writer can use `asuint(float4(A, B, scaleX, scaleY))` for target 3. Missing or invalid position/calibration retains the native pixel.

The mask should reproduce the source surface's coverage, including any opaque body outside a glass window. Surface imperfections belong in its normals and thickness. This renderer does not use stochastic alpha dots to imitate transparency. A flat normal and constant thickness will look correspondingly flat.

## Limits and fallback

- JSON: 64 KiB; 1–16 surfaces per mod. Mask: at most 4 MiB each and 16 MiB total per mod. Launch total: 32 unique recipes and 32 MiB of mask programs.
- Mask paths must use forward slashes, stay inside the declaring mod after canonical resolution, and contain no absolute paths, traversal or alternate data streams.
- Presentation is SDR only, with single-sample R10G10B10A2, RGBA8 or BGRA8 output. HDR retains native rendering. The SDR transfer is a 2.2 gamma approximation.
- Screen-sized compatible depth and a matching viewport/device/submission are required. Stale masks, mismatched cameras, unavailable depth or rejected private pipelines leave native rendering in place.
- The implementation has two mask slots and three depth slots. Private mask targets are limited to 160 MiB per slot, so the effective supported resolution can be lower than the 4096×2160 dimension ceiling. The common four 16-byte targets exceed that budget at 4K.
- Reflections and refraction only use the current screen image; off-screen or hidden radiance is unavailable. Displacement is bounded and avoids sampling foreground objects and print. This is not ray tracing or a complete physical glass volume simulation.
- One optical parameter set is retained per mask frame; different simultaneous surfaces requiring different optical settings are not yet a supported composition contract.

## Depth capture coexistence and validation

The depth probe owns the shared ResourceBarrier detour and dispatches to both capture consumers. Hook failures are logged. Transmission does not install a competing ResourceBarrier detour or replace Fit Check's depth cut-out.

Build with the normal Windows Release preset and `/W4 /WX`. Enable `DINGOSDK_BUILD_CAPTURE_TESTS` for declaration/shared-method tests and `DINGOSDK_BUILD_TRANSMISSION_GPU_TESTS` for synthetic D3D12 compositor checks. The latter require a working D3D12 adapter. Test cases cover tint, actual refraction against a straight-through control, foreground rejection, stale frames, wrong viewport and HDR fallback. They do not replace in-game tests of draw replay, wrapped COM implementations, preview cameras, device lifetime or mod combinations.

Native validation must cover preview and gameplay, camera orbit and lighting changes, resize/minimize/restore, a legacy animated/lenticular deck, another mod's material, Board Break and depth Fit Check. The older private renderer's successful native test matrix must not be presented as validation of this refactored declaration-based implementation.
