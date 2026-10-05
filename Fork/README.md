# Maintaining the Steam privacy module

This fork keeps upstream Steam sign-in and depot-download sections behind
`#if !RESKATE_STEAM_DOWNLOADS_DISABLED`. They remain in the source for easier
upstream merges, but are excluded from the compiled build. The header fixes the
guard to `1`; there is no launcher setting that enables depot downloads.

| File | Purpose |
| --- | --- |
| `steam_privacy.h` | Fixed build policy and launcher messages. |
| `steam_privacy.cpp` | Rejects requests to install or run DepotDownloader. Shared by the Windows launcher and server. |
| `launcher_privacy.cpp` | Local missing/unsupported-game notices; redirects the old sign-in action to installed-game checks; rejects download and credential-answer requests. |
| `Test/steam_privacy_tests.cpp` | Checks that downloader entry points reject calls without invoking callbacks. |

The original installed-game validation stays in `Launcher/launch.cpp` and its
platform helpers. Scripts, crash-report preferences and Steam multiplayer are
unchanged by this module. It does not disable other ReSkate network features.

## Update this fork

If an `upstream` remote does not exist yet, add it once:

```powershell
git remote add upstream https://github.com/Dingo-Shenanigans/ReSkate.git
```

With your local work committed, update and merge:

```powershell
git fetch upstream
git merge upstream/main
```

Keep the `Fork/` files and the small integration changes in `Launcher/` and
`cmake/Apps.cmake`. Resolve conflicts while preserving the guards. Review new
Steam sign-in/download paths: a guard around an old function cannot exclude a
new function added somewhere else.

Rebuild the launcher and run `launcher_steam_privacy` with launcher tests enabled.
The module includes rejecting replacements for the upstream download entry
points; compile/link errors after an API change should be fixed before use.

## Fresh upstream checkout

`patches/Apply-SteamPrivacyPatch.ps1` applies the module files, integration guards
and the existing safer troubleshooting advice together. It checks the entire
patch before applying it, skips already-patched source and rejects incompatible
updates without changing files. The patch is based on upstream commit `5bfe4f7`.

Use either a merge into this fork or the patch on a fresh upstream checkout.
Do not apply this patch over the earlier deletion-based privacy patch; update
that checkout from this fork first, or start from clean upstream source.

This design reduces changes to upstream files. It does not guarantee that every
future upstream version will merge or behave correctly without review.
