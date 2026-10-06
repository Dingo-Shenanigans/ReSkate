# ReSkate on Apple Silicon

`build-mac` builds a native Mac app that prepares Wine and opens the existing
ReSkate launcher. The launcher can download the supported game through your
Steam account, install mods, and update ReSkate as usual. You need your own copy
of skate. on Steam. Game files, accounts, saves and mods are not bundled.

This is experimental compatibility support, tested with an M1 Max and an M4 on
macOS 27.0.1 using the runtime versions below. It requires Apple Silicon,
macOS 27 or later, and Rosetta. Intel Macs and earlier macOS releases are not
supported by this build. The Windows game and injected DLL still run through
Wine; this does not compile them as native Mac software.

## Build

Install Xcode Command Line Tools (`xcode-select --install`) and Python 3.9 or
newer. Download and mount **Evaluation environment for Windows games 4.0 beta 2**
from [Apple Developer](https://developer.apple.com/download/all/?q=game%20porting%20toolkit).
The build accepts a mounted volume or an extracted copy containing `redist/lib`
and Apple's license and acknowledgement RTF files.

Extract a [ReSkate release](https://github.com/Dingo-Shenanigans/ReSkate/releases)
into a folder, then run from this repository:

```sh
./build-mac \
  --gptk "/Volumes/Evaluation environment for Windows games 4.0 beta 2" \
  --reskate "$HOME/build-inputs/ReSkate" \
  --dmg
```

Use the actual mounted volume name. `--reskate` must contain
`ReSkateLauncher.exe`, `ReSkate.dll`, `LICENSE.txt` and `licenses/`, plus
`launcher.json` when provided by the release. You can also supply the release
output of the Windows/MSVC build described in the main README, with its notices.
The Mac command compiles the Swift wrapper; it does not cross-compile ReSkate.

Output is `build/macos/ReSkate.app`, with an optional `ReSkate.dmg` containing
the app and an Applications shortcut. `--output` selects a different app path;
existing output is refused. `--cache` selects the archive cache. Builds download
two pinned archives from the Sikarugir project's GitHub releases and check their
SHA-256 hashes before extraction, including when reusing the cache:

| Component | Version |
| --- | --- |
| Wine engine | WS12WineSikarugir11.0_1 |
| Native support libraries | Template-1.0.21 |
| Apple graphics | Evaluation environment 4.0 beta 2, supplied by the builder |

The archive URLs and checksums are in `build.py`. The app's
`Contents/Resources/build-sources.json` also records the input ReSkate and Apple
graphics file hashes. Apple's framework signature is checked against Apple's
trust anchor. The builder preserves dependency bytes and signatures; it signs
only the new outer app ad hoc. It does not remove quarantine or change Gatekeeper
settings. Building does not require GameHub, CrossOver, Homebrew or a subscription.

## Use

1. Copy `ReSkate.app` to Applications and open it in Finder.
2. Choose an empty game folder, or an existing complete skate. folder. Quit any
   other copy using that game folder first.
3. Wait for the first-time Wine setup, then use the ReSkate launcher to download
   the game and press **PLAY**. On later runs **Play offline** skips the launcher.

The app offers Apple's Rosetta installer if Rosetta is missing, with an
installation command as a fallback. The game needs
about 14 GB in addition to the app, Wine prefix and mods. Shader preparation can
take several minutes the first time. Use the game menu to quit normally; quitting
the Mac app ends its own Wine session.

An existing ReSkate launcher/DLL pair is preserved, even if it is newer than the
bundled release. Its updater manages future Windows-side releases. Updating this
Mac app changes the bundled Wine/graphics runtime but does not reset the game
folder, mods, settings or profile. Runtime updates require a new Mac build.

| Data | Location |
| --- | --- |
| Selected game folder | `~/Library/Application Support/ReSkateMac/installation.json` |
| Wine prefix, including ReSkate profiles | `~/Library/Application Support/ReSkateMac/prefix` |
| Mac startup diagnostics | `~/Library/Application Support/ReSkateMac/launcher.log` |
| Game log and mods | `logs/` and `Mods/` in the chosen game folder |

Existing profiles in another Wine wrapper are not imported automatically. Back
them up and migrate them separately with both games closed. Keep the data folder
when replacing the app. Deleting the prefix deletes its saves.

## Compatibility fixes

- Keep the Windows launcher at 96 DPI without Retina, while enabling Retina for
  `Skate.exe`. This avoids the enlarged/clipped launcher controls.
- Select Apple's Direct3D libraries and the matching native Wine dependencies
  in a dedicated environment. Inherited Wine/graphics overrides are cleared.
- Disable frame synthesis and dynamic resolution; use resolution scale 1.
  The wrapper does not force an output resolution or change system audio settings.
- Keep the native app and launch locks alive after `ReSkateLauncher.exe` hands
  off to `Skate.exe`. A rejected second instance does not stop the first one.
- Check for the supported build's Steam marker and shader list before direct
  play. Copy the whole game folder when moving it; partial copies can stall at
  loading. The launcher remains available to repair an incomplete download.

Controller, HDMI audio, gameplay and map changes have worked with this runtime
on the tested Macs. Multiplayer and arbitrary mod compatibility are not covered
by those tests. Earlier graphics-settings freezes and bad cosmetic packs are not
claimed to be fixed by this wrapper.

For diagnostics, the executable supports:

```sh
APP="/Applications/ReSkate.app/Contents/MacOS/ReSkate"
"$APP" status
"$APP" launcher
"$APP" play
"$APP" stop
"$APP" wine reg query 'HKCU\Software\Wine\Mac Driver'
```

`--data-root FOLDER` creates an independent configuration/prefix for testing.
Use `setup GAME_FOLDER` first. Launch graphical sessions locally through Finder
or `open -a ReSkate`; direct launches over SSH can fail before rendering starts.

## Tests

The regression checks exercise preservation of existing installs, incomplete
game detection, path handling, inherited environment isolation, timeouts, lock
contention, and the launcher-to-game handoff with a controllable fake Wine server.
They do not require downloading the runtime or the game:

```sh
mkdir -p build/macos
xcrun swiftc -warnings-as-errors contrib/macos/Core.swift \
  contrib/macos/Test/CoreTests.swift -o build/macos/core-tests
build/macos/core-tests
python3 -m unittest discover -s contrib/macos/Test -p 'test_*.py'
```

Before distributing a changed runtime, also build the real app, initialize a
fresh prefix in a path containing spaces, check launcher scaling, enter gameplay,
test controller/audio, and switch maps. Unit checks cannot establish GPU or
game compatibility.

## Distribution

This source-only contribution does not publish a Mac binary or add an automatic
Mac release job. A maintainer can build the app/DMG above for release after
checking the supplied runtime terms and arranging signing/notarization.

Apple's included license sections 2A and 2C describe non-commercial distribution
of the complete D3DMetal framework and redistributables on Apple hardware. Read
the license accompanying the exact input version; it is copied into the app
with its acknowledgements. Apple's components are not relicensed under the
repository's GPL. See also [Apple's toolkit page](https://developer.apple.com/games/game-porting-toolkit/).

Wine and support libraries retain their upstream licenses. Sources/build recipes
are maintained under [Sikarugir-App](https://github.com/Sikarugir-App), including
[Wine](https://github.com/Sikarugir-App/wine). Distributors must provide the
corresponding source and notices required by the versions they ship, including
ReSkate and its dependencies. The build provenance manifest identifies the
binary inputs; it is not a replacement for those obligations.

Ad-hoc signing supports local builds but is not Developer ID signing or
notarization. A polished public download still needs the release maintainer's
signing/notarization process, including its bundled executable dependencies.
