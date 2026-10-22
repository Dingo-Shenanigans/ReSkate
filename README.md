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
  - The **Park Editor**: place, move and save objects with freecam, snapping and undo. Save a selection as a
    set and place it again anywhere, on any map.
  - Skater options: first person, movement, boosts, noclip.
  - **Hall of Meat** (off by default): bail and see the bones you hurt, with a skate. 3 style Meat card.
  - **Grind exit triggers** (off by default): press LT or RT while grinding to pick the side you pop
    out to (toe / heel, or left / right), whatever trick and spin you do
    ([how it works](Extension/Skater/GRIND_EXITS.md)).
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
- **BMX prototype on this branch.** Native bike rendering, rider posing, bike/skateboard switching,
  Streets-style controls and physics experiments. See [BMX development](#bmx-development-bmx-branch)
  below for setup, controls and the current limits.

## BMX development (`BMX` branch)

This branch adds BMX directly to ReSkate's runtime, with a generated native bike content pack and
development tools. It is an **experimental offline prototype**. Handling, ramp riding, landings,
collisions, rider animation and equipment transitions are still being developed. The normal
ReSkate release download does not contain this branch's complete BMX setup.

### What has been added

| Area | Work included on this branch |
|---|---|
| Native bike | Eight articulated world entities: frame, fork, handlebars, front/rear wheels, cranks and separate pedals. Shared geometry anchors connect steering, grips, wheels, pedals and carry placement. The active bike uses the game's world renderer. |
| Rider | Posing on the existing Skate skeleton, grip and pedal targets, crank-synchronized feet, preload, weight-shift and airborne posture cues. Native pose timing, contact alignment and clothing fit remain under investigation. |
| Riding | Independent native chassis control, wheel/surface queries, tire contact and braking, speed-sensitive steering, balance, manuals/nose manuals, pumping, hops and airborne control. Ramp approach, contact compliance and landing response have dedicated experiments and regression fixtures. |
| Tricks | Stick/shoulder/trigger input mapping, barspin and tailwhip articulation, a named trick catalog, rider release/pose cues, catch/landing policies and separate BMX trick/combo tallies. Catalog entries and successful unit tests do not establish that every trick looks or lands correctly in game. |
| Equipment | Bike/skateboard switching, nearby-bike mounting and dismounting, parked-bike state, carrying and grounded set-down. Readiness, speed, ground clearance and input ownership checks guard transitions; remounting and native board handoff remain active work. |
| Falls and recovery | Impact/bail detection, bike/rider lifecycle handling, native fall/GetUp investigations and post-fall remount diagnostics. Complete rider separation, natural ragdoll motion and reliable recovery are unfinished. |
| Physics integration | Adapters for the standalone movement library and an opt-in coupled bike/rider backend. The coupled backend has separate native bodies, force/impulse accounting, render-phase rider binding and a private ten-second trial; normal force adoption remains disabled. |
| Development support | Isolated game/profile launch scripts, input and body-ownership diagnostics, regression tests, ramp/equipment/fall rehearsals, recorder gates, video capture receipts, virtual-controller scenarios and an independent visual-review rubric. |

### Riding and equipment controls

The layout keeps the **left stick for handlebars/steering**, the **right stick for rear-wheel
control and tricks**, and **Skate's D-pad mappings**. Xbox / PlayStation button names are shown
together. These are the in-game prototype bindings; the standalone module has its own host controls.

| Input | Action |
|---|---|
| Left stick | Steer the handlebars/front weight shift; airborne yaw/pitch control |
| Right stick | Rear-wheel control; partial down/up for manual/nose manual; down then up for a grounded hop gesture |
| A / Cross | Pedal |
| B / Circle | Brake |
| R3 | Quick hop |
| LB / L1 + right stick down, airborne | Left barspin request |
| RB / R1 + right stick down, airborne | Right barspin request |
| Both triggers + right stick left/right, airborne | Tailwhip request |
| Return right stick to neutral | Catch/rearm after a trick request |
| LB / L1 + a fresh Y / Triangle press | Contextual bike/skateboard switch; release both afterward |
| Tap Y / Triangle, or F7 | Get on/off a nearby bike |
| Hold Y / Triangle, or F8 | Pick up/set down a nearby bike |
| F6 | Request a bike/skateboard switch |
| D-pad | Existing Skate actions |

Stop on clear ground before switching out of riding, and approach a parked bike before mounting
or carrying it. Center the sticks and release buttons to rearm gestures; menus, lost focus and
controller changes clear pending input. Physical-controller transition behavior still needs testing.
The optional human native-board handoff currently uses XInput.

Use the ReSkate console (**~**) for `bmx controls`, `bmx status`, `bmx diagnose`, `bmx toggle`,
`bmx mount`, `bmx dismount`, `bmx interact` and `bmx carry`. `bmx controls` also lists the no-pad
keyboard bindings. Diagnostic commands and automated rehearsals have additional private-profile
requirements; see the [BMX development tools](contrib/bmx-lab/README.md).

### Running the local prototype

Build this branch using the [Windows source-build instructions](#building-from-source). The local
ride helper expects a supported game copy and the generated `Mods/BMX_Prototype` pack already
prepared under `build/bmx-game/`. It copies the selected launcher/runtime into that isolated game;
it does not download the game or generate the bike pack.

With the game closed, run from the repository root:

```powershell
.\contrib\bmx-lab\Start-BmxGame.ps1 -Ride -NativeChassis -HumanNativeSwitch `
  -ProfileName bmx-user-data-working-profile
```

This uses a separate offline development profile under `build/`. The normal Steam installation
and ordinary saves remain separate. Close menus, focus the game and return controls to neutral
before riding. See [native prototype setup](contrib/bmx-lab/native-prototype.md) for the longer
setup and diagnostic history.

The current local bike pack uses the supplied `bmx-free` model, converted into native parts.
The game loads the prepared pack; it does not read an FBX from a player's Downloads folder.
The conversion tools and attribution notes are in [the bike asset documentation](assets/models/bmx/README.md).
Clean-install packaging of a distributable bike pack is still pending. Locally extracted BMX
Streets geometry is private reference material and is excluded from the committed assets.

### Standalone BMX Streets reverse-engineering work

[`standalone/bmx-movement/`](standalone/bmx-movement/README.md) contains the **BMX Streets
reverse-engineering research and its independent C++20 movement reconstruction**. It includes
recovered control/physics policies, measured runtime observations, rider/bike/IK tracks, a reusable
library, a runnable Windows demo, tests and comparison tooling. The library runs independently of
ReSkate, Unity, Steam and a BMX Streets installation.

| Folder/file | Contents |
|---|---|
| `standalone/bmx-movement/include/bmx/` | Public C++ interfaces for movement, suspension, posture, bike articulation, IK and rail-contact helpers |
| `standalone/bmx-movement/src/` | Physics/controller implementations and observed command/presentation tracks |
| `standalone/bmx-movement/demo/` | Windows reference demo with a simple rider and flat-plane host |
| `standalone/bmx-movement/tests/` | Numerical, input, posture and integration-contract tests |
| `standalone/bmx-movement/tools/` | Observation, extraction, reference verification, controller driving and comparison tools |
| `standalone/bmx-movement/reference/` | Calibration, provenance and documented reconstruction policies |
| `standalone/bmx-movement/recordings/` | Local original-game/demo captures, controller logs, comparisons and audit reports; large generated media is excluded from Git |
| [README](standalone/bmx-movement/README.md), [REFERENCE](standalone/bmx-movement/REFERENCE.md), [COMPARISON](standalone/bmx-movement/COMPARISON.md) | Library usage, recovered evidence, actual comparisons and remaining fidelity gaps |

The standalone library covers **58 observed available trick selections**, coupled bike/rider
motion, steering, braking, manuals, flick/quick hops, air control, posture and bike/IK playback.
Its reference host supplies a flat plane. ReSkate must supply real world contacts, collisions,
body ownership and animation retargeting. Reconstruction coverage and fitted tracks do not prove
matching handling or complete in-game trick support. Full rig collisions, bail/ragdoll, walking
and general engine integration remain outside the standalone demo's validation.

Build/test the library separately from the repository root:

```powershell
cmake -S standalone/bmx-movement -B build/standalone-bmx -DBMX_BUILD_DEMO=ON
cmake --build build/standalone-bmx --config Release
ctest --test-dir build/standalone-bmx -C Release --output-on-failure
```

The ReSkate adapters use `DINGOSDK_BMX_STANDALONE_MOVEMENT`; the private two-body backend also
requires `DINGOSDK_BMX_NATIVE_COUPLED`. Both default to **OFF**, and enabling them at build time
does not automatically activate a native physics trial. `DINGOSDK_BMX_MOVEMENT_SOURCE` can select
a pinned library source cut; by default it uses `standalone/bmx-movement/`. Build it with the same
compiler/toolchain as ReSkate rather than linking a supplied MinGW archive into MSVC.

### Tests, capture and current limits

Enable BMX C++ regression fixtures with `-DDINGOSDK_BUILD_BMX_TESTS=ON`, build, then run CTest as
described below. The [BMX lab](contrib/bmx-lab/README.md) also provides a standalone synthetic
controls viewer, serial native rehearsals and recording helpers. The synthetic viewer is separate
from the movement library's 3D demo and does not establish native riding physics.

[`Bmx-VirtualGamepad.py`](contrib/bmx-lab/Bmx-VirtualGamepad.py) and the
[coupled virtual-controller harness](contrib/bmx-lab/coupled-virtual-test.md) drive bounded input
scenarios against an explicitly identified private test process, with neutral release and
disconnect cleanup. Driver setup is a separate development step. Capture readiness, actual input
consumption, native body/pose receipts and visible gameplay must all be checked separately.
Passing fake-host/unit tests does not qualify real-controller transport or riding quality.

The [visual-review rubric](contrib/bmx-lab/judging/rubric-v1.json) requires current-build footage,
independent judges, at least **9.5/10 in every category**, and human-controller feedback. This is
the acceptance target, not the current score. The remaining work includes stable ramp/landing
physics, upright/collision behavior, natural rider motion, dependable falls/remounting and clean
equipment transitions. BMX multiplayer, native Skate scoring/challenges and replay fidelity are
unvalidated. The [feature plan](contrib/bmx-plan.md) separates the prototype from the intended
release scope; items in that plan are not all implemented.

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
[MACOS-SETUP.md](MACOS-SETUP.md) for its launch option.

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
| `-map <name>` | load that map as soon as the game is up, e.g. `-map "San Vansterdam"`; the name is a map's name, short name or asset path, or the start of one |
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
| `DINGOSDK_BUILD_BMX_TESTS` | `OFF` | build BMX input, physics-policy, rider, equipment and lifecycle regression fixtures |
| `DINGOSDK_BMX_STANDALONE_MOVEMENT` | `OFF` | compile/link the standalone BMX movement adapters |
| `DINGOSDK_BMX_NATIVE_COUPLED` | `OFF` | compile the isolated experimental native bike/rider backend; runtime opt-in is also required |
| `DINGOSDK_BMX_MOVEMENT_SOURCE` | `standalone/bmx-movement` in this checkout | choose the library source used by the BMX adapters, including a pinned development source cut |

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
| `Engine/Game/BMX/`, `Extension/BMX/` | BMX input/physics policies, native bike/rider integration, equipment lifecycle and regression fixtures |
| `standalone/bmx-movement/` | independent C++ movement library and BMX Streets reverse-engineering/reference work |
| `contrib/bmx-lab/`, `contrib/bmx-plan.md` | BMX launch, capture, controller/testing tools, research notes and feature plan |
| `assets/models/bmx/` | native bike conversion/validation tools and asset provenance; generated/extracted reference assets stay local |

### Supporting a new game build

Feature code never contains addresses. Everything tied to one `Skate.exe` lives in
`Engine/Game/Build/<build>/` and in the pins of
[supported_build.h](Engine/Game/Build/supported_build.h): file hashes, the Steam depot and manifest, and
the content cache. A new game build means a new table and new pins. Most hooks and patches check the
bytes they expect before touching them, so code from the wrong build is refused rather than patched.

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
