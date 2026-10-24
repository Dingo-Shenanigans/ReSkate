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
  - Throwdowns with other players (Jam, Spot Battle and S.K.A.T.E.), and co-op challenges. Plugins add
    throwdown modes of their own to the Throwdowns page ([contrib/throwdown-kit.md](contrib/throwdown-kit.md)).
- **In-game menu and console.**
  - Map and fast travel.
  - World: time of day, population, district levels, rotating parks.
  - The **Park Editor**: place, move and save objects with freecam, snapping and undo. Save a selection as a
    set and place it again anywhere, on any map.
  - Skater options: first person, movement, boosts, noclip.
  - Hall of Meat, grind exit triggers, Board Break, Road Rash and the FPV drone are plugin mods now,
    installed like any other mod. Board Break and Road Rash ship inside their asset mods.
  - Progression, controls, graphics and multiplayer settings.
- **Mods.**
  - Drop a mod in `Mods/` and it is merged into the game at launch. Mods can add custom maps, loading
    screens, cosmetics and scripts.
  - Browse and install mods from the [Thunderstore community](https://thunderstore.io/c/reskate/) in the
    launcher. A mod that needs others (a mod pack) brings them with it.
  - Save sets of mods as profiles, switch between them, and share one as a code or a file.
  - Most mod changes apply in game without a restart.
- **Launcher.**
  - Checks that you have the supported game build, and can download exactly that build with your Steam
    account.
  - Keeps ReSkate itself up to date.

## Getting started

1. Download the latest `ReSkate-<version>.zip` from
   [Releases](https://github.com/Dingo-Shenanigans/ReSkate/releases).
2. Extract `ReSkateLauncher.exe` and `ReSkate.dll` into **a new, empty folder of their own**, for
   example `C:\Games\ReSkate`. Do **not** put them in Steam's skate. folder: ReSkate keeps its own
   copy of the game there, so Steam updating skate. cannot break it.
3. Run `ReSkateLauncher.exe`.
   - It checks for ReSkate updates, then installs the supported build of the game into that folder
     (about 14 GB of free space).
   - Sign in to Steam when asked, with the account that owns skate. You can scan a QR code with the
     Steam app, or use your username and password with Steam Guard.
   - On later runs it checks the game files and downloads only what is missing.
4. Press **PLAY**.

ReSkate supports one game build at a time (Steam build `25414733`).

### Linux (Proton)

1. Install skate. with Steam, and put `ReSkateLauncher.exe` and `ReSkate.dll` in a new, empty folder
   of their own, for example `~/Games/ReSkate` (not Steam's skate. folder: the launcher refuses to
   run from there).
2. In Steam → skate. → Properties → Launch Options, enter this with your own folder's path:

   ```
   bash -c 'exec "${@/*EAAntiCheat.GameServiceLauncher.exe//home/you/Games/ReSkate/ReSkateLauncher.exe}"' -- %command%
   ```

3. Press **Play** on skate. in your Steam library. The launcher installs its own copy of the game
   into that folder the first time.

Steam starts EA's anti-cheat launcher (`EAAntiCheat.GameServiceLauncher.exe`), not `Skate.exe`, and the
anti-cheat refuses to run under Proton (*E111000B … Wine, Proton, and Steam Deck are not supported*).
The launch option swaps it for ReSkate's launcher, which starts the game itself. Launching from the game's
own Steam entry, rather than adding the launcher as a non-Steam game, keeps skate.'s app ID and its
Proton prefix.

The launcher in a folder of its own is new and has not been tested on Proton yet. With the launcher
beside `Skate.exe`, as it was set up before, ReSkate 1.1.3 was tested with Proton Experimental on
CachyOS: the launcher, the server browser, joining a public server and hosting a lobby. Steam Deck should work the same way but is untested. If you load a
custom map and then join a server on another map, the screen can stay black
([#31](https://github.com/Dingo-Shenanigans/ReSkate/issues/31)): load the server's map first.

### macOS (NotProton / CrossOver)

Steam for Mac with a Wine-based compatibility tool works much the same way; see
[macOS](https://docs.reskate.dev/macos) in the docs for its launch option.

### Multiplayer map downloads

Missing multiplayer custom maps can be downloaded from Thunderstore after confirmation.
The confirmation expires after two minutes. A session waits 30 minutes in all for the map to be
checked, downloaded, installed, applied and joined; dedicated servers and player-hosted lobbies
enforce the same limit on their own monotonic clocks. Ordinary map loading still gets 180
seconds; loading after a live installation still gets 30 seconds.

A download that has begun is not cut short when the session stops waiting: a large map on a slow
connection goes on downloading (up to four hours), installs and is applied, and you are told to
join the server again. Declining, or cancelling the download, stops it and removes the
incomplete file. Installation stops before replacing an existing mod when cancellation is
observed; a filesystem commit or live merge already in progress finishes safely rather than
being forcibly interrupted. Retry after that work has finished, without restarting ReSkate.

### Controls

| Key | Opens |
|---|---|
| **Insert** | the ReSkate menu |
| **~** (grave/tilde) | the command console (`help` lists the commands) |
| **T** | chat, in multiplayer |

The menu and console keys can be changed in the launcher's Settings.

The music player can play Spotify, YouTube Music, your own music files, internet radio and any app
on your PC: type `music setup` in the console, and see
[Music sources](https://docs.reskate.dev/music-sources) in the docs.

In **MAP → ROTATING PARKS** and the controller-friendly **MOD OPTIONS → PARKS** screen,
**Load Random Parks** chooses and loads one random layout for each of
San Vansterdam's three slots. Each supported layout has an equal chance; empty lots are excluded and
a roll can pick a layout already loaded. Choices save with your profile and are shared when you host.
**Randomize on Launch** is off by default. Turn it on to pick fresh layouts once on each subsequent
game launch, when the park controller is ready. Changing maps does not roll again. Guests follow the
host's layouts; a dedicated server admin can use the button to randomize the server's parks for everyone.
Both menus share the same launch preference. It only affects your own parks, including when you host
a lobby. Joining a host cancels any pending startup roll, so leaving the lobby restores your saved
layouts without unexpectedly randomizing them.

The console equivalents are `park random` and `park random-on-launch 0|1`. A dedicated server also
accepts `park random` from its console or an admin.

### Where things are

| What | Where |
|---|---|
| Mods | `Mods\<mod name>\` beside `Skate.exe`, ordered by `Mods\mods.json` |
| Logs | `logs\ReSkate.log` beside `Skate.exe` |
| Profile (skater, progress, parks) | `%LOCALAPPDATA%\ReSkate\profiles\offline\` |
| The game's own settings and saves | `%LOCALAPPDATA%\ReSkate\Game\` (kept apart from the normal game) |
| Launcher settings | `ReSkateLauncher.settings.json` beside the launcher |

### Troubleshooting

**My antivirus says ReSkate is a virus.** It is a false alarm. ReSkate has to load itself into the game as
it starts, and antivirus programs distrust anything that does that, most of all new programs they have not
seen much of yet. Only download ReSkate from this page's
[Releases](https://github.com/Dingo-Shenanigans/ReSkate/releases). If your antivirus removed a file, you can
bring it back: Windows Security → Protection history → find ReSkate → Restore. An error about SHA-256 right
after an update usually means the antivirus took the new file.

**The launcher says it cannot run from Steam's skate. folder.** Move `ReSkateLauncher.exe` and `ReSkate.dll`
into a new, empty folder of their own and run the launcher from there (see [Getting started](#getting-started)).

**The ReSkate menu (Insert) doesn't open.** ReSkate isn't running. On Windows, always start the game with
**PLAY** in the ReSkate launcher: pressing Play on skate. in Steam starts the normal game, without ReSkate.
On Linux and macOS, use the launch option in [Linux (Proton)](#linux-proton) or
[macOS](https://docs.reskate.dev/macos) in the docs.

**The game crashes.** Turn your mods off and try again first: many crashes come from mods. If it still
crashes, [open an issue](https://github.com/Dingo-Shenanigans/ReSkate/issues/new/choose) with your ReSkate
version, when it happened and what you were doing, and attach `logs\ReSkate.log`. With **Send crash reports**
on (it is by default), the crash details have already been sent; see
[Crash reports and privacy](#crash-reports-and-privacy) for what is sent and how to turn it off.

## Mods

- Install mods from the launcher's **MODS** page: browse Thunderstore, or drag a `.zip` or folder onto the
  window.
- **PROFILES** on that page saves which mods are on, and in what order, under a name. Loading a profile
  switches to it and downloads any of its mods you do not have; every mod stays installed once. **COPY CODE**
  and **SAVE FILE** share a profile, and **IMPORT CODE** and **IMPORT FILE** take one in. The codes and
  `.r2z` files are r2modman's, so the two managers read each other's.
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
- A mod that adds Quick Drop objects can name them and group them into families with a
  `reskate-objects.json` in its folder. A family is one tile in the Object Browser, and X / Square on it
  picks between its objects, the way the game's own sizes and colours work:

  ```json
  {
    "schema": 1,
    "categories": [{"id": "category_shoebox", "title": "Shoebox Pads"}],
    "families": [{"id": "qdparent_shoebox_dc", "title": "DC Shoebox", "category": "category_shoebox"}],
    "objects": [
      {"key": "own_bkrails_shoebox_dc_12m", "title": "DC Shoebox 12m", "family": "qdparent_shoebox_dc"},
      {"key": "own_bkrails_shoebox_dc_6m_red", "title": "DC Shoebox 6m, Red", "family": "qdparent_shoebox_dc"}
    ]
  }
  ```

  `key` is the object's owned asset key. A family can also go in one of the game's categories
  (`category_ramp`, `category_rail`, ...), and an object in one of the game's families. Titles, a
  `description` and `priority` (lower first) are optional. The file only adds: ids that are already taken
  and the game's own items are left out and logged, so a mod cannot rename or regroup the game's objects.
  The file is read when the game starts.

Only install mods you trust. Mods change game data. Custom Lua scripts (`scripts/Custom`) are
switched off in this version: nothing in that folder is read or run.

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

Close the game. Copy `ReSkateLauncher.exe` and `ReSkate.dll` into your ReSkate folder (the one the
launcher installed the game into, beside `Skate.exe`), then run the launcher.

Local builds never replace themselves. They only report that an update exists, so a release can't
overwrite the DLL you are testing.

Useful launcher flags:

| Flag | Effect |
|---|---|
| `--no-gui` | start the game straight away, without the launcher window; with no other launch flags it uses the launcher's saved settings |
| `--no-update` | skip the update check |
| `--offline`, `-offline` | play offline, without Steam running |
| `--windowed`, `--width=N`, `--height=N` | windowed mode and its size |
| `--log-level=<trace\|debug\|info\|warning\|error\|critical\|off>` | how much `ReSkate.log` records (`warn` works too) |
| `--log-trace` | the same as `--log-level=trace`; an explicit `--log-level` wins |
| `-wconsole` | also show the log live in a console window while the game runs |
| `--menu-key=0x2D`, `--console-key=0xC0` | menu and console keys (virtual-key codes) |
| `-map <name>` | load that map as soon as the game is up, e.g. `-map "San Vansterdam"`; the name is a map's name, short name or asset path, or the start of one. Over the start map chosen in Settings > GAME |
| `--no-loose-files` | ignore loose Lua and config files beside the game |
| `--gpu-diagnostics` | record extra detail when the graphics driver crashes (DRED) |

### CMake options

| Option | Default | Effect |
|---|---|---|
| `DINGOSDK_VERSION` | `0.0.0` | version stamped into the binaries |
| `DINGOSDK_LAUNCHER_AUTO_UPDATE` | `OFF` | let the launcher replace itself and `ReSkate.dll`, and the server replace itself (release builds) |
| `DINGOSDK_RELEASE_REPO` | `Dingo-Shenanigans/ReSkate` | public GitHub repository whose latest release carries `launcher.json` and the files it pins |
| `DINGOSDK_CRASH_REPORT_URL` | the project's endpoint | Sentry minidump endpoint URL; empty turns crash uploads off |
| `DINGOSDK_TEST_GAME_ROOT` | empty | a Skate folder, for the tests that read real game data |

### Tests

Each module keeps its regression tests in a `Test/` folder. Turn them on with any of
`DINGOSDK_BUILD_MULTIPLAYER_TESTS`, `DINGOSDK_BUILD_LAUNCHER_TESTS`, `DINGOSDK_BUILD_PARK_EDITOR_TESTS`
and `DINGOSDK_BUILD_CRASH_REPORT_TESTS`, then run CTest:

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

### The supported game build

ReSkate supports one `Skate.exe`, build 20260929, and stays on it: it does not follow newer Steam
builds. The launcher downloads that build, pinned in
[supported_build.h](Engine/Game/Build/supported_build.h) by file hashes, the Steam depot and manifest, and
the content cache.

Feature code never contains addresses. Everything tied to the game's code lives in
`Engine/Game/Build/20260929/`. Most hooks and patches check the bytes they expect before touching them,
so a different `Skate.exe` is refused rather than patched.

## Crash reports and privacy

When the launcher or the game crashes, ReSkate uploads a minidump and that session's log to the
project's self-hosted Sentry server, so crashes can be fixed. The log never contains your Steam password or
login name.

With **Send performance timings** ticked in the launcher's Settings (ADVANCED; off unless you turn it on),
ReSkate also sends how long a few steps took: the update check, the Steam download, mod
installs (with the package name), the mod merge, starting the game, each level load, joining a multiplayer
session and fetching the ReSkate lists. Time spent in a level adds its name, average frame rate and count of
long frames. Nothing about your account is included. Turning crash reports off turns this off too.

To turn crash reports off, untick **Send crash reports** in the launcher's Settings (ADVANCED), or set the
environment variable `RESKATE_CRASH_REPORTING=0`.

ReSkate also keeps the game off EA's online services: the game's own crash reports, telemetry and
remote configuration requests are turned off and blocked. Fast-travel artwork still comes from EA's
image CDN (`dingo-dev-assets.akamaized.net`), which only downloads images.

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
