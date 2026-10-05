# ReSkate

play it offline, host your own lobbies and dedicated servers, and mod it.
the launcher, the runtime that loads into the game, and the dedicated server.

> ReSkate is a fan project. It is not affiliated with or endorsed by Electronic Arts or Full Circle.
> You need your own copy of skate. on Steam.

## Features

- **Offline play.** No EA servers needed. Your skater, outfits, unlocks and progress are saved on your PC,
  and the game runs even with Steam closed.
- **Multiplayer.**
  - Host a Steam lobby for up to 32 players: public, or joined with a code, with an optional password.
  - Join dedicated servers from the in-game server browser.
  - Proximity voice chat, and text chat with emotes and an optional bad-word filter.
  - Parties in lobbies and on dedicated servers: invite, join, leave, promote.
  - Throwdowns with other players (Jam, Spot Battle and S.K.A.T.E.), and co-op challenges.
- **In-game menu and console.**
  - Map and fast travel.
  - World: time of day, population, district levels, rotating parks.
  - The **Park Editor**: place, move and save objects with freecam, snapping and undo.
  - Skater options: first person, movement, boosts, noclip.
  - Progression, controls, graphics and multiplayer settings.
- **Mods.**
  - Drop a mod in `Mods/` and it is merged into the game at launch. Mods can add custom maps, loading
    screens, cosmetics and scripts.
  - Browse and install mods from the [Thunderstore community](https://thunderstore.io/c/reskate/) in the
    launcher.
  - Most mod changes apply in game without a restart.
- **Launcher.**
  - Checks that you have the supported game build, and can download exactly that build with your Steam
    account.
  - Keeps ReSkate itself up to date.

## Getting started

1. Download the latest `ReSkate-<version>.zip` from
   [Releases](https://github.com/Dingo-Shenanigans/ReSkate/releases).
2. Extract `ReSkateLauncher.exe` and `ReSkate.dll` into either folder:
   - **your skate. folder**, beside `Skate.exe` (Steam → skate. → Manage → Browse local files); or
   - **an empty folder**, where the launcher installs the game for you (about 14 GB).
3. Run `ReSkateLauncher.exe`.
   - It checks for ReSkate updates, then checks the game files against the supported build.
   - If the game is missing, or Steam has updated it past the supported build, sign in when asked. You
     can scan a QR code with the Steam app, or use your username and password with Steam Guard. It then
     downloads only the files it needs.
4. Press **PLAY**.

ReSkate supports one game build at a time (Steam build `25414733`).
### Controls

| Key | Opens |
|---|---|
| **Insert** | the ReSkate menu |
| **~** (grave/tilde) | the command console (`help` lists the commands) |
| **T** | chat, in multiplayer |

The menu and console keys can be changed in the launcher's Settings.

### Where things are

| What | Where |
|---|---|
| Mods | `Mods\<mod name>\` beside `Skate.exe`, ordered by `Mods\mods.json` |
| Logs | `logs\ReSkate.log` beside `Skate.exe` |
| Profile (skater, progress, parks) | `%LOCALAPPDATA%\ReSkate\profiles\offline\` |
| The game's own settings and saves | `%LOCALAPPDATA%\ReSkate\Game\` (kept apart from the normal game) |
| Launcher settings | `ReSkateLauncher.settings.json` beside the launcher |

## Mods

- Install mods from the launcher's **MODS** page: browse Thunderstore, or drag a `.zip` or folder onto the
  window.
- In game, the **MODS** tab of the ReSkate menu (**Insert**) turns mods on and off and applies the changes.
- Mods are checked against the game build they were made for. Outdated mods, or mods that can't be merged
  cleanly, are left out with a message naming them, and the rest still load.
- A mod that adds songs can give them their own playlist in the game's music screen with a
  `reskate-music.json` in its folder:

  ```json
  {"schema": 1, "playlists": [{"name": "My Playlist", "songs": ["Artist - Title", "Artist - Other Title"]}]}
  ```

  Each entry is the song's artist and title exactly as the mod registers them, joined by ` - `. Entries that
  match no song are ignored, and a playlist with no matching songs is not shown. A file that does not follow
  this shape is skipped and logged. The songs themselves still have to be added by the mod.

  Optional playlist and track covers are PNG files packaged inside the mod:

  ```json
  {
    "schema": 1,
    "playlists": [{"name": "My Playlist", "artwork": "artwork/playlist.png", "songs": ["Artist - Title"]}],
    "song_artwork": {"Artist - Title": "artwork/track.png"}
  }
  ```

  Artwork paths are relative to the mod folder and use forward slashes. Images must be PNGs, at most
  2048x2048 and 4 MiB each. Missing or invalid covers are ignored without removing the playlist or song.
  ReSkate serves registered image bytes through a process-local HTTP endpoint bound only to `127.0.0.1`
  on an automatically selected port; no internet hosting is needed. The endpoint exposes no filesystem
  routes and retains at most 64 MiB of artwork. Existing content-cache covers keep their priority.

Only install mods you trust. Mods change game data, and custom scripts can run code.

## Dedicated servers

`ReSkateServer.exe` is a headless lobby that needs neither the game nor Steam installed. It is in the
`ReSkateServer-<version>.zip` of each release. See [Server/README.txt](Server/README.txt) for setup,
`ReSkateServer.json`, admin commands, votes and the anti-cheat checks.

### Linux servers

Each release also ships `ReSkateServer-Linux-<version>.zip`: the same headless lobby as a native
x86_64 Linux binary (no Wine, no game install). Setup in short:

```sh
unzip ReSkateServer-Linux-<version>.zip -d reskate-server && cd reskate-server
./setup-linux-server-libs.sh
./ReSkateServer            # writes ReSkateServer.json on first run; edit name/admins, restart
```

Details (Steam `.so` files, `world-layers.json`, `systemd`, ports, building from source with
`cmake --preset linux-x64`) are in [Server/README-linux.md](Server/README-linux.md).

## Building from source

### Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK).
- CMake **3.24** or newer. Visual Studio's *C++ CMake tools for Windows* component provides one.

All third-party libraries are in the repository, so the build needs no package manager or network
access.

### Build

From a *Developer PowerShell for VS 2022* in the repository folder:

```powershell
cmake --preset vs2022-x64
cmake --build --preset release --parallel 4
```

The files are written to `build/vs2022-x64/Release/`:

| File | What it is |
|---|---|
| `ReSkateLauncher.exe` | the launcher (also hosts crash reporting) |
| `ReSkate.dll` | the runtime the launcher loads into the game |
| `ReSkateServer.exe` | the dedicated server |
| `ReSkateEmotePacker.exe` | builds the chat emote pack in `assets/emotes` |

To work in Visual Studio, open `build/vs2022-x64/DingoSDK.sln` after configuring and select
**Release | x64**.

### Running your build

Close the game. Copy `ReSkateLauncher.exe` and `ReSkate.dll` beside `Skate.exe`, then run the launcher.

Local builds never replace themselves. They only report that an update exists, so a release can't
overwrite the DLL you are testing.

Useful launcher flags:

| Flag | Effect |
|---|---|
| `--no-gui` | start the game straight away, without the launcher window |
| `--no-update` | skip the update check |
| `--offline` | play offline, without Steam running |
| `--windowed`, `--width=N`, `--height=N` | windowed mode and its size |
| `--log-level=<trace\|debug\|info\|warning\|error>` | how much `ReSkate.log` records |
| `--menu-key=0x2D`, `--console-key=0xC0` | menu and console keys (virtual-key codes) |
| `--no-loose-files` | ignore loose Lua and config files beside the game |
| `--gpu-diagnostics` | record extra detail when the graphics driver crashes (DRED) |

### CMake options

| Option | Default | Effect |
|---|---|---|
| `DINGOSDK_VERSION` | `0.0.0` | version stamped into the binaries |
| `DINGOSDK_LAUNCHER_AUTO_UPDATE` | `OFF` | let the launcher replace itself and `ReSkate.dll`, and the server replace itself (release builds) |
| `DINGOSDK_RELEASE_REPO` | `Dingo-Shenanigans/ReSkate` | public GitHub repository whose latest release carries `launcher.json` and the files it pins |
| `DINGOSDK_BACKTRACE_URL` | the project's endpoint | Backtrace minidump submission URL; empty turns crash uploads off |
| `DINGOSDK_TEST_GAME_ROOT` | empty | a Skate folder, for the tests that read real game data |

### Tests

Each module keeps its regression tests in a `Test/` folder. Turn them on with any of
`DINGOSDK_BUILD_MULTIPLAYER_TESTS`, `DINGOSDK_BUILD_LAUNCHER_TESTS`, `DINGOSDK_BUILD_PARK_EDITOR_TESTS`
and `DINGOSDK_BUILD_BACKTRACE_TESTS`, then run CTest:

```powershell
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON -DDINGOSDK_BUILD_LAUNCHER_TESTS=ON
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release
```

The project builds with `/W4 /WX`: warnings are errors.

## Project layout

The source tree loosely follows Frostbite's own layout.

| Folder | Contents |
|---|---|
| `Engine/Core/` | logging, console, JSON, storage, platform helpers, Detours hooks, crash reporting (`Debug/`) |
| `Engine/Resource/` | Frostbite data formats: EBX, TOC, CAS, bundles, shader lookup |
| `Engine/Vfs/` | InitFS, the Mods folder, mod merging, the content cache |
| `Engine/Scripting/` | custom Lua scripts |
| `Engine/Game/` | the game's native ABI and data models |
| `Engine/Game/Build/<build>/` | every address, fingerprint and patch tied to one `Skate.exe` build, as `addr::<area>::<name>` |
| `Extension/` | ReSkate's features, the in-game UI and diagnostics |
| `Runtime/` | `ReSkate.dll`: entry point, startup and exports |
| `Launcher/` | `ReSkateLauncher.exe` |
| `Server/` | `ReSkateServer.exe` |
| `EmotePacker/` | `ReSkateEmotePacker.exe` |
| `External/` | third-party libraries, pinned and hashed (see [External/README.md](External/README.md)) |
| `assets/`, `config/` | files built into the binaries: launcher art, emotes, default profile settings |
| `cmake/` | the build, split by area |

### Supporting a new game build

Feature code never contains addresses. Everything tied to one `Skate.exe` lives in
`Engine/Game/Build/<build>/` and in the pins of
[supported_build.h](Engine/Game/Build/supported_build.h): file hashes, the Steam depot and manifest, and
the content cache. A new game build means a new table and new pins. Most hooks and patches check the
bytes they expect before touching them, so code from the wrong build is refused rather than patched.

## Crash reports and privacy

When the launcher or the game crashes, ReSkate uploads a minidump and that session's log to the
project's Backtrace account, so crashes can be fixed. The log never contains your Steam password or
login name.

To turn it off, untick **Send crash reports** in the launcher's Settings (ADVANCED), or set the
environment variable `RESKATE_CRASH_REPORTING=0`.

## Third-party code

ReSkate uses Dear ImGui, Microsoft Detours, RapidJSON, spdlog, SQLite, LZ4, Zstandard, miniz, bcdec and
Valve's networking headers, and the Montserrat and Permanent Marker fonts. Versions, licenses and local
patches are in [External/README.md](External/README.md). Every release ships their license files in
`licenses/`.

## License

Copyright © 2026 the ReSkate contributors.

ReSkate's source code is free software: you can redistribute it and modify it under the terms of the
[GNU General Public License, version 3](LICENSE). If you share a modified ReSkate, share its source
code under the same license.

The license covers ReSkate's own code. It does not cover:

- **Third-party libraries and fonts** in `External/`, which keep their own licenses (see
  [External/README.md](External/README.md)).
- **The chat emotes** in `assets/emotes/`, which were made by other people.
- **Images of the game** (`assets/launcher/background.jpg`, `assets/startup_splash.jpg`) and anything
  else from skate. The game and its content belong to Electronic Arts, and nothing here grants any
  rights to them.

## AI disclosure

Parts of ReSkate, including code, reverse-engineering notes and documentation, were written with the
help of AI coding assistants. The maintainers direct and test that work, but AI-written code can contain
mistakes like any other. If something looks wrong, please open an issue.
