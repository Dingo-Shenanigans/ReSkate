# ReSkate structure

How the repository is laid out, what each part builds into, and where a feature's code lives.
For building and using ReSkate, see [README.md](README.md).

## What gets built

| Binary | Source | What it is |
|---|---|---|
| `ReSkateLauncher.exe` | `Launcher/` | the launcher: updates, Steam sign-in and game download, mods and profiles, starts the game, crash reporting |
| `ReSkate.dll` | `Runtime/` + `Extension/` + `Engine/` (+ `SpotifyHelper/`) | the runtime the launcher loads into `Skate.exe` |
| `ReSkateServer.exe` / `ReSkateServer` | `Server/` | the headless dedicated server (Windows and native Linux) |
| `ReSkateEmotePacker.exe` | `EmotePacker/` | packs `assets/emotes` into the chat emote pack |
| `dingosdk_builtin_packer` | `BuiltinPacker/` | packs `assets/builtin` into the built-in packs |
| `dingosdk_*_tests` | each module's `Test/` | regression tests, run with CTest |

Internally the project is still called **DingoSDK** (CMake options, target names).

## Layering

```
 Launcher/ ──launches──▶ Skate.exe ◀──injected── ReSkate.dll
                                                   │
                                     Runtime/      entry point, startup, per-frame tick, exports
                                       │
                                     Extension/    ReSkate's features and UI
                                       │
                                     Engine/Game/  native ABI, data models, per-build addresses
                                     Engine/Vfs/   mods, merging, content cache
                                     Engine/Resource/  Frostbite formats
                                     Engine/Core/  logging, console, hooks, crash reports
                                       │
                                     External/     third-party libraries

 Server/ (ReSkateServer) reuses Engine/Core and the multiplayer protocol, not the game.
```

Feature code never holds game addresses: everything tied to one `Skate.exe` build lives in
`Engine/Game/Build/<build>/` (see [Engine/Game](#enginegame)).

## Top level

```
ReSkate/
├── CMakeLists.txt, CMakePresets.json   build entry; presets vs2022-x64, linux-x64, release, linux-release
├── cmake/                 the build, split by area
├── Engine/                engine-level code shared by the DLL, launcher and server
├── Extension/             ReSkate's features (in ReSkate.dll)
├── Runtime/               ReSkate.dll entry point and lifecycle
├── Launcher/              ReSkateLauncher.exe
├── Server/                ReSkateServer (dedicated server)
├── SpotifyHelper/         Rust static library: the game as a Spotify Connect device
├── EmotePacker/           emote pack tool
├── BuiltinPacker/         built-in pack tool
├── External/              vendored third-party code, pinned and hashed
├── assets/                files embedded into the binaries
├── config/defaults/       default profile/settings JSON embedded into ReSkate.dll
├── contrib/               server hosting files, design notes, release script
├── README.md, LICENSE
└── .clangd, .gitattributes, .gitignore
```

## `cmake/`

| File | Covers |
|---|---|
| `ProjectOptions.cmake` | `DINGOSDK_*` options, version stamping (`reskate_version.h`, `version.rc`) |
| `Dependencies.cmake` | the `External/` libraries as static targets |
| `Core.cmake` | `Engine/` libraries: hooks, console core, launcher support, supported build |
| `Features.cmake` | `Extension/` feature libraries |
| `UI.cmake` | the ImGui overlay and PlayStation controller input |
| `Runtime.cmake` | `ReSkate.dll` (`dingosdk_runtime`), including the cargo build of `SpotifyHelper/` |
| `Apps.cmake` | launcher, server, packer tools, and most test executables |
| `CrashReport.cmake` | Sentry crash report client/upload, wired into the DLL, launcher and server |
| `ProfileData.cmake`, `CustomScriptData.cmake` | embed `config/defaults/` and the custom Lua entry |
| `templates/` | generated headers: version, `version.rc`, crash/update config, profile defaults |

## `Engine/`

Loosely follows Frostbite's own layout.

### `Engine/Core/`
| Folder | Contents |
|---|---|
| `Console/` | command registry and console core |
| `Debug/` | crash reports: minidumps, report envelope, trace, upload |
| `Hooks/` | Detours hook helpers |
| `Json/` | JSON helpers over RapidJSON |
| `Log/` | spdlog-based logger, log paths, crash/event logging |
| `Platform/` | process injection, PE helpers, memory and path utilities |
| `Profiling/` | profiler and sampler |
| `Storage/` | SQLite save database |
| `Text/` | chat word filter |

### `Engine/Resource/`
Frostbite data formats: binary bundles, TOC, CAS codec (Oodle/zstd/LZ4), EBX documents (read, write, merge,
fixup repair), mesh sets, skeletons, textures, material grids, shader lookup.

### `Engine/Vfs/`
The virtual file system and modding pipeline:
- `initfs`, `game_archives`, `game_bundles`, `game_textures`, `native_db` — reading the game's data.
- `mod_list`, `mod_catalog`, `mod_item_sources`, `mod_conflicts`, `mod_scoring` — the `Mods/` folder and `mods.json`.
- `mod_merge*` — merging mods into the game's data (overrides, combine, grid, textures, loading screens, CAS store, scan cache, build stamp).
- `content_cache*`, `content_catalogs` — the merged-content cache and its install.
- `thunderstore_package`, `https_download` — Thunderstore packages.
- `mod_music`, `item_thumbnails`, `world_layer_scan`, `builtin_packs`.

### `Engine/Scripting/`
Custom Lua script support (currently switched off for users).

### `Engine/Game/`
The game's native ABI and plain data models, no hooks:

| Folder | Contents |
|---|---|
| `Abi/` | native strings, data layouts, transforms, callback queue |
| `Build/` | `supported_build.h` (hashes, Steam depot/manifest pins), `fingerprint.h`, `image_identity`, `addresses.h` |
| `Build/20260929/` | one header per area with every address, fingerprint and patch for this `Skate.exe` build, as `addr::<area>::<name>` |
| `Input/` | controller bindings, PlayStation reports, voice input |
| `Multiplayer/` | session model and limits, tick/voice/distance settings, role commands, chat rate |
| `Profile/` | progression, player card, object persistence models |
| `Rendering/` | draw packets, graphics controls |
| `Settings/` | named settings, simulation time, multiplayer settings lock |
| `Skater/` | first person, velocity boost, skater body |
| `UI/` | game view, menu entries and scale, loading screen selection |
| `World/` | world model, layers, parks (rotation, randomization, editor), travel, custom level manifest |

## `Extension/`

ReSkate's features. Each folder is one area; most have a `console_commands.cpp` for its console
commands and a `Test/` folder.

| Folder | Feature |
|---|---|
| `Boot/` | offline boot, offline Steam, blocking EA app/services, user-data redirect, startup interventions, Discord presence, fatal dumps |
| `Assets/` | live mods (apply without restart), loose files, mod layers, multiplayer map download, map icons |
| `Console/` | built-in console commands, perf commands, memory ledger |
| `Customization/` | cosmetics catalog, item browser, local customization, player card |
| `Multiplayer/` | Steam lobbies, sessions, parties, voice and text chat (see below) |
| `Music/` | local music, playlists and artwork, external players: Spotify, YouTube Music, Plex |
| `News/` | launcher/game news feed |
| `Objects/` | object placement and the Park Editor (`ParkEditor/`), park object sets, park mods |
| `Plugins/` | the Luau plugin host and its `reskate.*` API (see below) |
| `Profile/` | offline profile storage: skater, progress, missions, objects, preferences |
| `Progression/` | challenges, entitlements, neighborhood unlocks, fast travel unlock, rip score, board wear |
| `Rendering/` | graphics controls and labels, display startup, replay export |
| `ReplayKeyframes/` | replay keyframe curves and settings |
| `Scripting/` | Lua startup, custom script loader, native blocking |
| `Settings/` | gameplay/native/user settings, input bindings, engine tweaks, replay drafts |
| `Skater/` | first person, noclip, free flight, physics tuning, no-bail, AI skaters, x-ray, breakable deck, launches and pad hiding for plugins, trick watch |
| `Throwdowns/` | native throwdowns, custom (plugin) modes, relay and wire format |
| `Trainer/` | trick trainer: waypoints, jumps, landings, presets |
| `Travel/` | location travel |
| `UI/` | the in-game UI (see below) |
| `World/` | level loading, loading screens, time/atmosphere, population, park rotation, world layers, engine guards |
| `AnimationStudio/` | animation core |

### `Extension/Multiplayer/`
| Folder | Contents |
|---|---|
| `Net/` | protocol, wire/delta/pose/sound codecs |
| `Steam/` | lobbies, server browser, transport and lanes, friend join, social |
| `Session/` | session state, send/receive, parties, commands, rooms, passwords |
| `Remote/` | remote skaters: native skater spawn, cosmetics, poses, audio, VFX, collision |
| `Hud/` | nametags, indicators, player UI |
| `Voice/` | voice capture and playback |

### `Extension/UI/`
| Folder | Contents |
|---|---|
| `Overlay/` | the ImGui overlay: the ReSkate menu (Insert) and its pages, console, chat and emotes, Park Editor panels, plugin pages, perf overlay, theme |
| `NativeMenu/` | pages built into the game's own menu (hub, multiplayer, throwdowns) |
| `Startup/` | startup window/splash |

### `Extension/Plugins/`
The Luau plugin host. A plugin is a folder in `Mods/` with a `reskate-plugin.json`.
- `plugin_discovery`, `plugin_manifest` — find and read plugins.
- `plugin_vm`, `plugin_runtime`, `plugin_host` — the sandboxed Luau VM and plugin lifecycle.
- `plugin_api*.cpp` — the `reskate.*` API (game, world, UI, more); `plugin_game*`, `plugin_camera`, `plugin_joystick`.
- `plugin_ui`, `plugin_sound`, `plugin_wave`, `plugin_http`, `plugin_session_feed`, `plugin_wire` — pages, audio, HTTP, multiplayer messages.
- `plugin_commands` — the `plugin` console command.

## `Runtime/`

`ReSkate.dll`'s lifecycle: `exports.cpp` (DLL exports), `bootstrap`, `runtime_init`, `runtime_state`,
`client_context`, `client_tick` (per-frame), `frame_timing`, `level_requests`, `request_scheduler`,
`menu_entry_hook`, `overlay_callbacks`, `console_bridge`.

## `Launcher/`

| Files | Contents |
|---|---|
| `main.cpp`, `launch` | entry point and starting/injecting the game |
| `updater` | ReSkate self-update from GitHub releases |
| `depot_output`, `game_settings` | supported game build download/verification and game settings |
| `mod_manager`, `mod_profiles`, `thunderstore` | mods, profiles (r2modman-compatible codes and `.r2z`), Thunderstore |
| `gui*.cpp` | the window: home, mods (browse, profiles), settings, sign-in, changelog, legal, gamepad |
| `crash_reporter` | crash reporting host |
| `Test/` | launcher tests |

## `Server/`

| Files | Contents |
|---|---|
| `main.cpp`, `server_engine`, `server_host`, `steam_server` | the server loop and Steam game server |
| `server_config` | `ReSkateServer.json` |
| `server_commands`, `server_votes`, `server_party`, `server_parks`, `server_map_update` | admin commands, votes, parties, parks, map updates |
| `roles.h`, `global_bans`, `server_words` | roles/permissions, bans, word filter |
| `board_check.h`, `speed_check.h` | anti-cheat checks |
| `server_plugins` | server-side plugins |
| `server_update`, `server_release` | self-update |
| `GUI/` | optional server window |
| `README.txt`, `README-linux.md`, `README-roles.txt`, `ROLES.md` | server docs |
| `Test/` | server tests and fixtures |

## `SpotifyHelper/`

Rust crate (`reskate-spotify`, librespot-based) built by cargo from `cmake/Runtime.cmake` as a static
library linked into `ReSkate.dll`. It makes the game a Spotify Connect device. Not built in Debug.

## `External/`

Vendored libraries, versions and hashes in `External/manifest.json` and [External/README.md](External/README.md):
Dear ImGui, Microsoft Detours, Luau, RapidJSON, spdlog, SQLite, LZ4, Zstandard, miniz, bcdec,
Valve's Steam networking headers, and fonts (Montserrat, Permanent Marker).

## `assets/` and `config/`

- `assets/launcher/` — launcher background and icons.
- `assets/emotes/` — chat emotes (`emotes.json`, `emotes.png`).
- `assets/builtin/` — built-in packs (`throwdown_menu.zip`).
- `assets/models/fpv/` — FPV drone model.
- `assets/*.jpg|png` — splash, music artwork.
- `config/defaults/` — default `profile.json`, `progress.json`, `settings.json`, `options.json`,
  `challenges.json`, `location-travel.json`, `object-dropper.json`, `news.json`, `manifest.json`.

## `contrib/`

| Path | Contents |
|---|---|
| `pterodactyl/` | Pterodactyl egg for hosting the server |
| `reskate-server.service`, `setup-linux-server-libs.sh` | Linux server setup |
| `package_release_zip.py` | packs release zips |
| `*.md` | design notes for individual features |
| `Test/` | tests for contrib scripts |

Plugin mods (the ports of former built-in features, the test plugins, Fit Check, Animator Studio) are not
in the repository. Maintainers keep them in `contrib/plugins/`, which is ignored, and
`dingosdk_plugin_tests` runs the test plugins from there when they are present.

A plugin mod's folder looks like:

```
<plugin>/
├── reskate-plugin.json   plugin manifest
├── manifest.json         Thunderstore manifest
├── plugin/main.lua       entry point (+ other modules, tests.lua)
└── images/, sounds/, data/   optional assets
```

## Tests

Tests live in a `Test/` folder beside the code they cover (about 115 files) and build as
`dingosdk_*_tests` executables. Turn groups on with `DINGOSDK_BUILD_MULTIPLAYER_TESTS`,
`DINGOSDK_BUILD_LAUNCHER_TESTS`, `DINGOSDK_BUILD_PARK_EDITOR_TESTS`, `DINGOSDK_BUILD_CRASH_REPORT_TESTS`
and `DINGOSDK_BUILD_PLUGIN_TESTS`, then run `ctest --test-dir build/vs2022-x64 -C Release`.
