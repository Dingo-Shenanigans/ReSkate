# AGENTS.md

Notes for AI coding agents working on ReSkate. People may find them useful too.

ReSkate is a launcher (`ReSkateLauncher.exe`) and a runtime (`ReSkate.dll`) loaded into EA's
`Skate.exe`, plus a dedicated server. Internally it is still called DingoSDK: CMake options and targets
are `DINGOSDK_*` and `dingosdk_*`. [STRUCTURE.md](STRUCTURE.md) says where everything lives and
[README.md](README.md) covers using and building it; read STRUCTURE.md before looking for code.

## Build

Windows, Visual Studio 2022 (v143), CMake 3.24+, from a Developer PowerShell. Everything is vendored,
so the build needs no network or package manager.

```powershell
cmake --preset vs2022-x64
cmake --build --preset release --parallel 4
```

Output is in `build/vs2022-x64/Release/`. One target at a time:
`cmake --build build/vs2022-x64 --config Release --target <target>`, where the targets are
`dingosdk_runtime` (ReSkate.dll), `dingosdk_launcher` (ReSkateLauncher.exe) and `dingosdk_server`
(ReSkateServer.exe).

Warnings are errors (`/W4 /WX`). A change is not done until it builds clean.

The Linux dedicated server builds with `cmake --preset linux-x64` and
`cmake --build build/linux --target dingosdk_server`. Only the server builds on Linux.

## Tests

Tests sit in a `Test/` folder beside the code they cover and build as `dingosdk_*_tests`. Each group
is off until its option is on, for example:

```powershell
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_PLUGIN_TESTS=ON -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build --preset release --parallel 4
ctest --test-dir build/vs2022-x64 -C Release
```

The groups are the `DINGOSDK_BUILD_*_TESTS` options, defined in [CMakeLists.txt](CMakeLists.txt),
[cmake/Apps.cmake](cmake/Apps.cmake) (launcher) and [cmake/CrashReport.cmake](cmake/CrashReport.cmake). Tests that read real game data need
`-DDINGOSDK_TEST_GAME_ROOT=<a Skate folder>`. Run the tests for what you changed, and add one when you
fix a bug the tests can reach.

Most of ReSkate only really runs inside the game, which an agent usually cannot drive. When a change
can only be checked in the game, say so, and say what you did and did not verify.

## Rules

- **One game build, for good.** ReSkate supports exactly one `Skate.exe`, build 20260929 (pins in
  [supported_build.h](Engine/Game/Build/supported_build.h)). It is not moving to newer Steam builds. Do not
  add support for another build or code that adapts to one.
- **No addresses in feature code.** Every game address, offset and patch lives in
  `Engine/Game/Build/20260929/<area>.h` and is used as `addr::<area>::<name>`. A function that is hooked
  or patched has a `<name>_prefix` byte array beside it, and the feature checks those bytes before
  touching it, so wrong bytes are refused rather than patched. Follow an existing header.
- **Hooks go through [Engine/Core/Hooks](Engine/Core/Hooks/hooks.h)** (Microsoft Detours): `hook_prepare`,
  then `hook_enable`. Hooks installed while the game is running go inside a `HookBatch`.
- **The SDK offers general APIs, not modes.** No C++ that knows one particular game mode, menu or
  feature that a plugin could provide. Add the general building block to the SDK and build the feature
  on it as a Luau plugin mod (a mod folder with a `reskate-plugin.json`). The goal is that
  nobody has to send a pull request to the SDK to make a mode or mod.
- **Plugins are sandboxed; keep them that way.** A new `reskate.*` API goes in
  `Extension/Plugins/plugin_api*.cpp` behind a permission in `plugin_manifest.cpp`, with tests in
  `Extension/Plugins/Test/plugin_tests.cpp`, including one showing a plugin without the permission
  cannot use it. Plugins never get raw
  game memory, ReSkate's console commands, the microphone or clipboard, or a way to write cosmetics,
  entitlements or progression.
- **Nothing that unlocks what the game sells.** Mods and plugin APIs refuse paid store items on purpose.
  Do not add a way around that.
- **No EA content in the repository.** No decompiled game code, game binaries or extracted game data.
  Addresses, byte checks and our own notes about the game are fine.
- **Third-party code only in `External/`**, pinned, with its hash in `External/manifest.json` and its
  license in [External/README.md](External/README.md). Ask before adding a dependency.

## Style

- Match the code around you: `snake_case`, the `dingosdk` namespace, and comments that say why in
  plain sentences. Comments about the game record what was found and how.
- Commit subjects name the area, then say what now happens in a plain sentence, for example
  `Launcher: a start map in Settings, GAME, instead of always San Vansterdam`.
- Keep a change to what was asked. Leave unrelated cleanups for their own change.
