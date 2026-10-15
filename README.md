# ReSkate Outfit Shuffler

A mod for [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) that shows other players' outfits
and boards as random items, for you only. Turn it on and everyone in your lobby is wearing something
new: a different shirt, other shoes, a deck you have never seen them ride.

Nothing you see is sent anywhere. The players' real outfits are never changed or saved, and they still
see themselves, and you, as you really are. It is a fork of ReSkate with the shuffle built in: no game
or mod files are touched, and everything else in ReSkate works as before.

> This is a fan project. It is not affiliated with or endorsed by Electronic Arts, Full Circle or the
> ReSkate team. You need your own copy of skate. on Steam.

## What it does

One setting, **Shuffle outfits**, with three values:

| Value | What other players look like |
|---|---|
| **Nothing** (default) | As they really dress. An item you do not have installed (from their cosmetics mod) shows the game's usual stand-in. |
| **Everything** | Every item they wear or ride is a random item that fits the same slot. |
| **Not installed** | Only the items you do not have installed are random, instead of a plain stand-in. Everything else shows as they wear it. |

- **Boards too.** Deck graphic, grip, trucks and wheels are shuffled the same way, with picks of
  their own, so a board does not follow its rider's clothes.
- **Empty stays empty.** A slot a player left empty is not filled in, so nobody gets a hat or a tattoo
  they did not wear.
- **Picks stick.** A player keeps the same random outfit until you reroll. Switching between
  Everything and Not installed keeps the picks too.
- **Reroll** deals everyone new picks at any time.
- **Your mods count.** The picks come from every cosmetic installed on your PC, including mod items
  you have.

## Using it

- In game, open the ReSkate menu (**Insert**) and go to the **Online** tab. Under **OUTFITS**, press
  **Shuffle outfits** to step through Nothing, Everything and Not installed. **Reroll shuffle** below
  it deals new picks.
- It is also under **Settings > Interface**.
- From the console (**~**):

  ```
  mp shuffle-outfits off|all|missing|cycle|reroll
  ```

  `off`, `all` and `missing` set Nothing, Everything and Not installed. `cycle` steps to the next
  value in the menu's order: Nothing, Everything, Not installed, then back to Nothing. `reroll` deals
  new picks.

- Your choice is saved with your profile.

## Keeping the game stable

skate. can hold a fixed number of textures. When a map and its mods need one more, the game freezes
(ReSkate warns at 90%). The game does not let go of an item's textures once it has loaded them, so a
shuffle that kept picking new items would slowly fill the game up.

When the game reaches **80%** of its texture limit, the shuffle switches to picking only items already
loaded in the current level, so it brings in no new textures. Chat tells you once when this happens.
Players who are already dressed keep their picks, and modes and rerolls still work, from the smaller
pool.

The console command `textures` shows how many textures are in use.

## Installing

Build the launcher and `ReSkate.dll` from this repository (see [Building from source](#building-from-source)),
then follow ReSkate's own [setup guide](https://github.com/Dingo-Shenanigans/ReSkate#getting-started)
using your built files in place of a release.

Use the launcher from your build. A release launcher keeps ReSkate up to date and would replace your
`ReSkate.dll` with the official one, without the shuffle.

## Status

The shuffle builds and its rules are covered by tests, but it has not been played in game yet. Things
still to check:

- How full-body outfits and costumes look when another item takes their slot under Everything.
- Whether board parts made for each other (a grip cut for one deck shape, say) can end up mismatched.
  The only check is the game's own: an item must fit its slot.
- Whether loading a new level lets go of shuffled items' textures.

How it works, file by file, is in [contrib/outfit-shuffle.md](contrib/outfit-shuffle.md). Bug reports
are welcome as issues on this repository.

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

## License

ReSkate is free software under the [GNU General Public License, version 3](LICENSE), and so is this
fork. Copyright © 2026 the ReSkate contributors.

The license covers the code only. Third-party libraries and fonts in `External/` keep their own
licenses (see [External/README.md](External/README.md)), the chat emotes in `assets/emotes/` were made by
other people, and skate. and everything from it belong to Electronic Arts.

Parts of ReSkate and of this mod were written with the help of AI coding assistants.
