# Animator Studio

This integration brings the Studio 1.0.10 feature set into ReSkate. Install and update ReSkate normally; a separate Studio DLL and disabled launcher updates are not required for an integrated build. See [release acceptance](studio-release-checklist.md) for the fork's earlier validation record.

## Use the studio

Once your skater is in game, press **F8**, hold **L3 + R3**, or choose **Open Animation Studio** under **Mod Options > PLAYER** or the ReSkate menu's **SKATER** page. Close Park Editor first. The first library build reads your own installed game; no game animation data is shipped in this repository.

In **TRICKS**, search the native library or browse its folders, choose a trick/pop, then edit body and board keys on the timeline. Save it under your own name. Preview speed changes watching speed, not gameplay timing. Editor bindings are configurable under **SETTINGS > STUDIO > EDITOR KEYS**.

Saving alone does not activate an animation. In **MY TRICKS**, tick **ACTIVE**, choose a playlist and turn that playlist on. Only one playlist is on at a time. Also enable **Custom tricks in game**. **TEST IN GAME** is temporary until you reopen the studio or restart; playlists retain the persistent selection.

- **Fixed** picks the highest playlist entry per trick/pop/stance/takeoff slot; Alternate variants expand only within that winning entry.
- **In turns** follows playlist order across attempts. **Shuffle** chooses randomly without immediately repeating when alternatives exist.
- **Exact stance** uses the saved leading-foot/fakie combination. **Front foot** can mirror across leading feet. Save separate normal, fakie, switch and switch-fakie versions when each needs its own style.
- **ANY pop** generates low/mid/high versions in memory; **COPY TO LOW, MID AND HIGH** makes separately editable versions. A selected pop can also provide fallbacks where no explicit version covers another height.
- **CATCH**, **CATCH HOLD** and **DROP PREVIEW** tune/watch the catch-based timing on drops. Each trick can override the global crossfade.

Browse/install packs in **MARKET**, then activate them through a playlist. Publishing your own pack requires **MARKET > ACCOUNT / SIGN IN**, your Thunderstore team and its service account token. The token stays in `ReSkate.Tricks/market/account.key`, protected by Windows DPAPI for your Windows user; `account.json` stores the team name. Sign out and relink if that key was deleted or moved to another PC. Never share these account files or zip the whole tricks folder to distribute a pack; use export/share.

Back up `ReSkate.Tricks` before modifying an existing library. Pack update failures keep recovery files under `.pack-transactions` when rollback cannot finish. **SETTINGS > STUDIO > REPORT A BUG** creates a local report ZIP; review it and include the trick, pop, stance, session context and preview/gameplay result when reporting a problem.

Native inventory includes graph states, transitions and parameters as well as clips. An indexed or editable grind/loop does not imply that live replacement of that phase is implemented. General airborne foot correction remains disabled. Proton/macOS Studio operation and live market publishing were not validated in this integration.

## Runtime contract

Studio is compiled into ReSkate.dll for the supported game build 20260929 / Steam 25414733. The fork's 1.0.10 release used ReSkate 2.1.2; this branch reconciles it with official main. Do not infer support for another game executable from a successful source build.

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

Studio work by 22sh, Fourth Studio, Norms and Steez, with testing by THE CREW, built on ReSkate and its contributors. The integration retains the original authors' commit history.
