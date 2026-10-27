# Animator Studio implementation

This describes the 1.0.10 candidate. User installation and operation are in [README](../README.md); release acceptance is in the [checklist](studio-release-checklist.md).

## Runtime contract

Studio is compiled into ReSkate.dll. ReSkate 2.1.2 and game build 20260929 / Steam 25414733 are the supported integration. Do not infer support for another executable from a successful source build.

Gameplay edits are applied to fresh native body/board poses. The selected trick, pop, stance and takeoff context decide which saved edits are eligible. The native clip matching clock supplies source timing; marked fallback paths are not proof of a decoded native animation-graph state.

Studio and plugin pose/render/board callbacks have separate registrations. Studio's outgoing pose filter uses `set_animation_studio_pose_filter`; it does not replace the plugin's filter. Graphics descriptors are allocated in disjoint ranges through `descriptor_slots.h`.

Base stance comes from the profile's `DefaultStanceIsGoofy`. Runtime leading-foot flags live in `Engine/Game/Build/20260929/skater_lead_foot.h`. Exact stance and Front foot are different authoring/playback choices; check both physical limb ownership and fakie state when changing selection.

`AirStretch` can slow a trick from its catch on a drop independently of `air_corrections`. Its authored pose returns toward the native landing as the predicted touchdown approaches; catch tools/drop preview are visible again. General airborne foot corrections remain disabled. At a rail lock, feet captured relative to the last airborne board slide toward the native grind stance only for that lock's fade. A later fade, a playing saved grind, or a different skater generation invalidates that capture. Ground contact protection and other-pop handling remain separate code paths.

The native library is built from the supported game's data, with versioned build metadata. Inventory includes unsupported controllers and graph/document entries. A converted preview or saved native piece is not automatically a verified live grind/transition override.

## Saved data and workers

Animations use JSON metadata and FAS clip payloads. The reader retains FAS1/FAS2 compatibility and supports FAS3 compressed joint data plus rotation-only ownership masks. Malformed payloads are rejected. Mirror operations carry masks and keyed anatomy to the matching joints.

Saved tricks load on a background worker. Immutable snapshots keep the animation/render workers from reading mutable editor state. Clip/model caches share common native data. A stale snapshot must not be described as evidence that the newest saved file played.

Packs reference saved tricks. Named playlists select packs/tricks; one playlist is on at a time. Fixed selection resolves the winning entry per slot before expanding an Alternate slot. Shuffle and In turns retain their broader candidate groups.

Pack imports validate files before preparing a `PackFileTransaction`. All replacement files are staged and originals backed up before the commit. Reported replacement errors restore old files; failed recovery retains backups. This is rollback for reported file errors, not automatic crash recovery or a claim that concurrent observers see a single atomic multi-file rename.

Recycle Bin failure/cancellation aborts deletion before pack metadata is removed. Remaining files are never permanently deleted as a fallback. A partially completed shell operation can still leave some files in the Recycle Bin.

## Market and diagnostics

`marketplace.*` handles Thunderstore listing, downloads, account checks and publishing. Tokens are DPAPI-protected for the Windows user; authenticated requests do not forward tokens through redirects. Linking an account is not itself evidence that a real pack publish succeeded.

Standard bug reports collect bounded diagnostics, relevant preset files, log tail, settings and version information. They are saved locally and exclude account.key. Automatic runtime crash reporting is a separate ReSkate facility.

## Relevant files

| Area | Source |
|---|---|
| Serialization, animation math, native clock | `Extension/AnimationStudio/anim_core.*` |
| Selection and playback rules | `anim_presets.h`, `anim_runtime_rules.h`, `anim_playback.h` |
| Game/editor integration and workers | `anim_studio.*`, `studio_room.*`, `pop_grades.*` |
| File rollback and cancellation | `pack_file_transaction.h`, `Test/pack_file_tests.cpp` |
| Native inventory/build | `native_library.*`, `native_asset_names.h` |
| UI and key bindings | `Extension/UI/Overlay/animation_studio_overlay.cpp` |
| Market | `marketplace.*`, `marketplace_listing.h` |

See the [build guide](studio-tester-build.md) for regression and library checks. Legacy recording tools and early prototype notes are historical, not current user workflows.
