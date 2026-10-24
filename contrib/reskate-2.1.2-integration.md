# Animator Studio on ReSkate 2.1.2

The candidate starts from studio main `9d5e293` (1.0.8), includes PR #9
`99c3368` (the game's DefaultStanceIsGoofy option; recording removal), and merges
the exact official ReSkate `v2.1.2` tag, `97793b7`. Contributor history is retained.
The additional drop/catch and branding work on `studio/ux-round-2` (`540fb9a`)
has been inspected but remains separate from this compatibility build.

ReSkate 2.1.2 now has its own animation and plugin APIs. The studio and plugin
pose/render/board listeners and outgoing pose filters therefore have separate
registrations. Studio filtering runs after plugin filtering; opening the studio
cannot overwrite the plugin callback pointer. Features moved to plugin mods
(Hall of Meat, Road Rash, Board Break, grind exits and Drone) are not restored as
old built-in implementations.

The studio's five graphics descriptors follow the plugin pictures and screen
look ranges in one 28-slot heap. Hardcoded studio slots 4..8 are removed: those
now belong to plugin images. The studio's loading art resolves game data through
the new x-ray/game-image helper instead of the removed Hall of Meat module.

The shared animation core retains studio serialization and math while supporting
2.1.2's rotation-only position ownership masks. FAS1/FAS2 and original FAS3 clips
remain readable; masked FAS3 clips round-trip with or without board data. Mirrored
clips carry the mask to the corresponding limb. Malformed masks are rejected.

The `ci-windows` preset stamps ReSkate 2.1.2 and the modified AnimatorStudio build,
and enables animation plus plugin regression checks. CI also uploads ReSkate.dll
on its own, including when full-library packaging is disabled. Game execution
and visual validation are separate from automated tests.

## Installing the DLL

Update the official installation to ReSkate 2.1.2 first. In the launcher settings,
turn off **Install ReSkate updates** before replacing the DLL, so the official
updater will not restore its own runtime. Close the game, back up ReSkate.dll,
and replace it beside Skate.exe. Keep the current launcher, animation library,
presets and recordings. No repository access is needed for a shared DLL file.

This is a compatibility tester candidate; it has not been visually accepted in
the game. It does not merge the candidate or the still-open PR #9 into main.
