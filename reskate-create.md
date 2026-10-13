# ReSkate.Create architecture

This note records what the repository currently proves, what the original Skate.Create references establish, and what must remain behind an adapter until runtime evidence exists.

## Product target

ReSkate.Create is a native, local-first graphics editor inspired by Skate 2 and Skate 3's Graphic Creator. Its source documents remain editable and separate from generated previews or game assets. The initial editor targets layered shapes, transforms, visibility, opacity, ordering, undo/redo, deterministic saves, and local thumbnails. Imported images, equipment application, and sharing follow only after their boundaries are validated.

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

`Extension/GraphicsCreator` now owns schema version 1:

- bounded 64-4096 pixel canvas;
- ordered rectangle and ellipse layers;
- stable IDs, display names, visibility, opacity, transforms, dimensions, and RGBA fill;
- strict finite/range validation and duplicate-ID rejection;
- deterministic JSON serialization;
- atomic saves, previous-version backups, corrupt-primary recovery, and bounded listing.

The model has no engine pointers, network identity, remote URLs, scripts, or equipment mutations. This keeps documents testable and prevents editor data from crossing gameplay ownership boundaries.

## Inferred next integrations

1. Rasterize a document into a bounded RGBA preview and cache by source hash. Rendering must not mutate the source document.
4. Add imported image layers only after image decoding, dimensions, file-size caps, alpha handling, and local-copy ownership are tested.
5. Introduce `EquipmentGraphicAdapter` as an optional boundary. Board, shirt, billboard, and sticker targets must each report unavailable until a supported game-build binding is validated.

## Unknown and intentionally blocked

- The supported game build's runtime texture/decal replacement contract.
- Whether an applied graphic can be refreshed without rebuilding a mod package or reloading an equipped item.
- Which customization surfaces synchronize to other players and how ownership is represented.
- Exact color-space and mip requirements for each target material.

No UI control should claim that a graphic is equipped until those questions have executable evidence and a failure-safe rollback path.

## Pull request sequence

1. Document model, validation, deterministic codec, atomic store, and recovery tests. Implemented.
2. Native editor state, canvas, layer controls, undo/redo, and local gallery. Implemented for rectangle and ellipse layers.
3. Off-screen thumbnail rendering and cache invalidation.
4. Bounded local image import and compositing.
5. One build-validated equipment adapter, starting with board bottom art; expand targets independently.
6. Controller navigation, color-theme integration, polish, and optional sharing design.
