# ReSkate on macOS: setup (Steam for Mac + NotProton / CrossOver)

ReSkate has no native macOS build. It runs through a Wine-based Steam compatibility tool on top of
the native Steam for Mac client. Tested on an M1 Pro, macOS 26, with NotProton on CrossOver 26.3:
the launcher shows *Welcome, <your name>* and the game reaches the Hub.

## Setup

1. Install skate. with the Steam for Mac client (`~/Library/Application Support/Steam`).
2. Install the compatibility tool, then Steam → skate. → Properties → Compatibility → force it.
3. Put `ReSkateLauncher.exe` and `ReSkate.dll` beside `Skate.exe`
   (`steamapps/common/Skate`, as in the README's *Getting started*).
4. Steam → skate. → Properties → Launch Options:
   ```
   /bin/bash -c 'export ROSETTA_ADVERTISE_AVX=1; exec "${@/EAAntiCheat.GameServiceLauncher.exe/ReSkateLauncher.exe}"' -- %command%
   ```
5. Press **Play** in the Steam library. Start from Steam, not by running the launcher yourself:
   the launcher finds your account through the environment Steam gives the compatibility tool.

### Why this launch option

- **`/bin/bash`, not `bash`**: Steam on macOS executes launch options directly, without a shell,
  so `bash` alone failed to spawn here (*Failed to spawn process*, OS error 260). The absolute
  path works.
- **`ROSETTA_ADVERTISE_AVX=1`**: the game refuses to start with a *Below Min Spec* dialog when
  the CPU does not report F16C and OSXSAVE in CPUID, or has 3 or fewer cores. Rosetta 2 runs
  those instructions but hides the CPUID bits by default; this variable makes it report them.
  It must be in the process environment when Wine starts, so it is exported in the launch option
  rather than set from the launcher. Nothing else in that check (GPU, RAM, OS) is involved.
- **The `${@/…/…}` swap** is the same as on Linux: it replaces EA's anti-cheat launcher with
  ReSkate's.

## Steam detection

Under a non-Proton compatibility tool the launcher used to show *Steam is not running*:
the Wine-side `steam.exe` stand-in never sets `ActiveUser`, and `STEAM_COMPAT_CLIENT_INSTALL_PATH`
points into `Steam.AppBundle/.../Contents/MacOS` rather than the Steam data directory. The launcher
now walks up from that path to the directory holding `config/loginusers.vdf`. When it runs under
Wine with Steam's compatibility environment set, it skips the process and handle checks and uses
the `loginusers.vdf` account. Real Windows and Proton are unchanged.

## Building the launcher on macOS

The CMake build is MSVC-only. To cross-build only the launcher with mingw-w64:

```
brew install mingw-w64
contrib/build-launcher-mingw.sh [outdir]
```

It takes about 30 seconds and writes `ReSkateLauncher.exe` and `ReSkateLauncher.stripped.exe` to
`outdir` (default `~/Games/reskate/mingw-build`). This is a mingw build, not the release MSVC one.
`ReSkate.dll` is not built this way.
