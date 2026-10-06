# ReSkateCOLOURS

This fork keeps the proven wardrobe-colour patch built into ReSkate instead of treating it as a Mods-folder package.

## What stays built in

- `Insert -> SKATER -> COLOURS`
- Top / Bottoms / Shoes / Socks / Hat / Glasses / Outfit / Costume / Deck / Grip tape / Trucks / Wheels
- ORIGINAL / SOLID / GRADIENT / OFF
- Hue-wheel colour picking plus exact HEX input
- Saved through ReSkate's existing local profile style system
- Normal ReSkate identity/tag rules are left intact

The wardrobe source changes are the same changes used by the locally-tested v2.2 build.

## Updating

The GitHub Actions workflow checks upstream `Dingo-Shenanigans/ReSkate` every 6 hours.

When upstream changes, it:

1. merges upstream `main` into this fork,
2. builds a matched `ReSkateLauncher.exe` + `ReSkate.dll`,
3. makes a release containing those binaries,
4. publishes a `launcher.json` that points wardrobe launcher/runtime updates at this fork while leaving game/depot/server data sourced from the official ReSkate release.

If an upstream merge or build breaks, no new release is published, so the previous working release remains available.

## One-time install

1. Close Skate and ReSkateLauncher.
2. Open this repository's **Releases** page.
3. Download the newest `ReSkateCOLOURS-*.zip`.
4. Extract `ReSkateLauncher.exe` and `ReSkate.dll` into your normal `.reskate` folder, replacing those two files.
5. From then on, launch the normal `ReSkateLauncher.exe`.

ReShade files are not part of this fork and are not modified by the wardrobe build.
