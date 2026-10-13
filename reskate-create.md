# ReSkate.Create architecture

This note records what the repository currently proves, what the original Skate.Create references establish, and what must remain behind an adapter until runtime evidence exists.

## Product target

ReSkate.Create is a native, local-first graphics editor inspired by Skate 2 and Skate 3's Graphic Creator. Its source documents remain editable and separate from generated previews or game assets. The initial editor targets layered shapes and imported images, transforms, visibility, opacity, ordering, undo/redo, deterministic saves, and local previews. Equipment application and sharing follow only after their boundaries are validated.

The historical references establish that Graphic Creator output appeared on shirts, skateboards, Skate 3 billboards, stickers, and team branding. The original browser service was retired in 2013. ReSkate.Create therefore reproduces the creative workflow, not the retired service or its network assumptions:

- https://skate.fandom.com/wiki/Skate.Create
- https://skate.fandom.com/wiki/Graphic_Creator

## Confirmed in this repository

- `Extension/UI/NativeMenu/native_menu.cpp` registers a native `RESKATE.CREATE` tab and owns its lifecycle with the other ReSkate pages.
- `Extension/UI/Overlay/hub_page.cpp` renders the current locked construction page. It is the integration point for the editor UI, not a second top-level overlay.
- `Engine/Core/Json` provides bounded parsing and deterministic object-key ordering.
- `Extension/Objects/ParkEditor/park_document.cpp` demonstrates validated, atomic local document replacement.
- `Extension/Boot/user_data_redirect.*` provides ReSkate-owned Local AppData isolation. Runtime integration should store graphics beneath that root.
- `Extension/Customization/developer_board.*` can validate and publish color parameters for supported board materials. It does not prove arbitrary runtime texture replacement.
- `Engine/Vfs/game_textures.*` and the overlay texture helpers can decode existing game images. They do not yet prove a safe writable decal pipeline.

## Implemented foundation

`Extension/GraphicsCreator` now owns schema version 2 and migrates version 1 saves when they are opened:

- bounded 64-4096 pixel canvas;
- ordered rectangle, ellipse, triangle, diamond, star, and local-image layers;
- sticker, board, beanie, hoodie, and shirt design targets with neutral placement templates;
- stable IDs, display names, visibility, opacity, transforms, dimensions, and RGBA fill;
- strict finite/range validation and duplicate-ID rejection;
- deterministic JSON serialization and local-only asset references;
- atomic saves, previous-version backups, corrupt-primary recovery, and bounded listing.

PNG and JPEG imports are capped at 16 MB and 4096 pixels per side, decoded through WIC, copied into `Create/Assets` under a content-derived name, and rendered as a bounded editor preview. Re-importing the same bytes reuses the same local asset. The UI also includes two reusable ReSkate-branded vector marks and duplicate/reorder controls.

The model has no engine pointers, network identity, remote URLs, scripts, or equipment mutations. This keeps documents testable and prevents editor data from crossing gameplay ownership boundaries.

## Inferred next integrations

1. Replace the bounded CPU editor preview with an off-screen GPU raster target cached by document content hash. Rendering must not mutate the source document.
2. Introduce `EquipmentGraphicAdapter` as an optional boundary. Board, shirt, hoodie, beanie, and sticker targets must each report unavailable until a supported game-build binding is validated.

## Unknown and intentionally blocked

- The supported game build's runtime texture/decal replacement contract.
- Whether an applied graphic can be refreshed without rebuilding a mod package or reloading an equipped item.
- Which customization surfaces synchronize to other players and how ownership is represented.
- Exact color-space and mip requirements for each target material.

No UI control should claim that a graphic is equipped until those questions have executable evidence and a failure-safe rollback path.

## Pull request sequence

1. Document model, validation, deterministic codec, atomic store, and recovery tests. Implemented.
2. Native editor state, canvas, layer controls, undo/redo, targets, and local gallery. Implemented.
3. Bounded local PNG/JPEG import and preview compositing. Implemented.
4. Off-screen full-resolution thumbnail rendering and cache invalidation.
5. One build-validated equipment adapter, starting with board bottom art; expand targets independently.
6. Controller navigation, color-theme integration, polish, and optional sharing design.
