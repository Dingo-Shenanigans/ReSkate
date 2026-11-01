# Applying mods without a level reload: research

Research notes, 2026-10-24, build 20260929 (Ghidra project `ReSkate`, decompile in `C:\Users\Zee\ReSkateDecomp`).
Nothing here is built yet. The aim: after a live apply (`Extension/Assets/live_mods.cpp`), show as many
kinds of mod as possible without loading the level again.

## Where things stand

A live apply already does most of the work without a reload:

- it merges every enabled mod into the patch mounted at launch (`Mods/.reskate`);
- it registers superbundles the game did not know (`register_superbundles`);
- it adds new maps to the root level's on-demand list (`register_on_demand_levels`);
- it swaps the in-memory TOCs of `Win32/globals` and `Win32/items` for the merged ones (`refresh_toc`):
  any bundle loaded from then on is read from the merged data;
- it hands over loading screens (`loading_screen::set_live_screens`).

So **maps, loading screens and plugins already need no reload**. The reload is for everything that is
already *loaded*: the engine reads a bundle when it loads it and keeps it until it unloads it, and a
level load is the one time it unloads and loads them all.

## How the engine loads bundles (found today)

**The resource manager** is the singleton at `DAT_14774dd60` (ReSkate address `0x774dd60`). Bundles load in
*operations*, each into a *compartment*, and stay loaded while the operation's handle is held.

| What | Address | Notes |
|---|---|---|
| begin a load operation | `FUN_1441b48d0(&handle, compartment, names[], count, 0, 0, 0)` | builds the op (`FUN_1441ba810`), queues it with vtable `+0x40` |
| is it done | `FUN_1441b6600(handle, wait_ms)` | vtable `+0x88` |
| did it succeed | `FUN_1441b4f60(handle)` | vtable `+0x90` |
| release / cancel | `FUN_1441b3970(handle)` | vtable `+0xa0` |
| pump while waiting | `FUN_1441b4fc0()` | |
| a synchronous load, as the engine does it | `FUN_144648a00(?, name)` | "Loading bundle %s...": compartment 1, wait, check |

**A level's resources** (`FUN_144649bf0`, profiler name `Level::startLoadLevelResources`) are three
operations whose handles the level object keeps:

| Compartment | Bundles | Handle at |
|---|---|---|
| 1 | one bundle from a global (`DAT_14777cda8`) (`FUN_144649ba0`) | level `+0x660` |
| 4 | the list at level `+0x438` | level `+0x668` |
| 5 | the level description's `Bundles`, less those kept between levels | level `+0x670` |

The root level's description (`levels/game/dingolevel_root/dingolevel_root/description`) lists six
`Bundles`, all with `TryKeepBetweenLevels = false`, so every level load unloads and reloads them:

```
common/textures/bundles/common_assetssharedbundle
customization/GestureVfx
items/bkprops/own_bkbounceable_generic_pad_00001_ecs.517136427_bundle
characters/customization/configs/cas_main_sharedbundle
audio/_systems/bundles/audiocustomizationsharedbundle
Levels/Game/DingoLevel_Root/DingoLevel_Root
```

All six are one operation with one handle: **one of them cannot be unloaded on its own**, and loading
one again only adds a reference to the copy already loaded.

## Cosmetics: most of the mods, and the most to gain

Thunderstore's ReSkate community lists about 5000 packages: Cosmetics 2144, Skateboards 858, Audio 1591,
Maps 330. Cosmetics and decks are the bulk, so they are where a reload-free apply pays off.

### Where a cosmetic lives

- **`win32/characters/customization/configs/cas_main_sharedbundle`** (items superbundle; 17,521 EBX,
  7,526 resources) holds the shared side of every cosmetic: the main bundle-reference table
  `characters/customization/configs/cas_main_bundlereftable`, `items/itemmastercollection`, every season's
  `items/_seasons/<v>/ownables/<v>_itemcollection`, and the item definitions (`items/cust_*/own_*`). It is a
  root level bundle (compartment 5, above).
- **Each preset has a bundle of its own**, `..._ap_cas_main_bundlereftable` (for example
  `vans_hat_beaniewatch_00003`: 51 EBX, 7 resources, 8 chunks): its appearance preset (`_ap`), textures
  (`_c`, `_ny`, `_m`, `_msk`), meshes. These load **on demand** when something wears the item, and
  unload some time after nothing does (`BundleRefLoaderSettings`: `BundleUnloadGraceTime`,
  `StaticBundleUnloadDelay`; the values are not in the bundles, so they are engine defaults or initfs).
- A mod's new preset goes **into a bundle the game already lists** (`Engine/Resource/bundle_ref_table.h`:
  each added preset gets rows pointing at an existing bundle), so new presets ride in existing per-item
  bundles.

### How a worn item finds its bundle

The skater's appearance composite (`FUN_140511f90`) loads each item's bundle by name through
`FUN_140518d70`, which asks every registered **BundleRefTable** whether it knows the name, and queues the
load with the first that does (`FUN_1405201c0`); a name no table knows fails with "Is it in a
BundleRefTable?" and the composite fails.

- The registered tables are a list at `+8` of the registry `DAT_1471ee620` (ReSkate `0x71ee620`), made by
  `FUN_14054b9a0`. The game's customization Lua adds them with a binding of the "TextureCompositor"
  library (`FUN_14052c9f0`; library registered by `FUN_140534600`).
- A lookup (`FUN_1416fe710` → `FUN_1416fe160`) hashes the name with 64-bit FNV-1a and searches a table:
  - the table's data is behind table `+0x48` (pointer, low 2 bits are tags);
  - **first, the child tables listed at `data + 0x20` (a vector, searched newest first)**;
  - then the table's own rows (`FUN_1416fe070`): at `data + 0x18` → header with the sorted 16-byte rows at
    `+8` (64-bit hash, bundle index, packed name reference), row count `+0x48`, bundle count `+0x4c`,
    strings at `+0x10`; binary search;
  - then the parent tables at table `+0x40`;
  - with no data, a sorted {hash, object} array at table `+0x28` (`FUN_1416fe870`).

  The rows are the resource ReSkate already reads and writes (`Engine/Resource/bundle_ref_table.cpp`).

### What the menu lists

ReSkate builds the game's cosmetic inventory itself (`Extension/Customization/local_cosmetic_catalog.*`,
`refresh_cosmetic_catalog`), from the game's **cosmetics manager** (`0x71ee610`, created by
`FUN_14054b8d0`): a map of loaded item definitions at `+0x30` (each node's asset: key `+0x38`, hash `+0x48`,
categories `+0x20`), and its collections at `+0x58` (name `+0x20`, items `+0x28`). An item reaches the
menu once its definition is loaded and in that map; ReSkate's catalogue then follows it (it rebuilds when
the count changes). What fills the map is not found yet (see the open questions).

### What each kind of cosmetic change needs

| Change | Where it is | Without a reload? |
|---|---|---|
| An existing item looks different (recolour, new texture, new mesh) | its per-item bundle | **Probably already works**: the bundle is read from the swapped items TOC whenever it next loads. An item being worn keeps the old copy until its bundle unloads (grace time) and loads again. To test (E1). |
| A new item | rows in the main BundleRefTable, its definition and collection entry in the shared bundle, its preset in a per-item bundle | Needs three things, below. |
| An existing item's definition, categories or collection | the shared bundle | Needs a reload (the same assets are already loaded). |

### New items without a reload: the plan

1. **Rows:** build a small BundleRefTable of only the presets the mods add (ReSkate's writer can) and attach
   it as a **child** of the loaded `cas_main_bundlereftable` (`data + 0x20`). The engine already searches
   children first, so nothing loaded changes. Alternative: point the table's data at a copy built from the
   merged table, as `refresh_toc` does for TOCs.
2. **Definitions:** the merge also writes a **delta bundle** holding only the assets the mods *add* (new
   item definitions, a collection of them), new GUIDs only, and the live apply loads it with
   `FUN_1441b48d0` into compartment 5, holding the handle. It must be released before the level unloads
   (the next level loads the full merged shared bundle, which holds the same assets).
3. **Registration:** add the delta collection's items to the cosmetics manager (the function that fills it
   is the open question), then `refresh_cosmetic_catalog()` makes them owned and listed.

## Other kinds of mod

| Kind | Today | Without a reload |
|---|---|---|
| Maps | no reload | already |
| Loading screens | no reload | already |
| Plugins | no reload (found within 2 s) | already |
| Plugin game data, world objects | reload | as cosmetics: a per-object bundle loads on demand from the swapped TOC; resident ones need a reload |
| Decks (Skateboards) | reload | as cosmetics (the deck mesh is in the items superbundle) |
| Audio | reload | `audiocustomizationsharedbundle` is a root level bundle like the cosmetics one: same limits; not researched |
| Edits to a base map's own bundles | reload | would need that sublevel unloaded and loaded again (SubLevelManager, `DSubLevelSuperBundle`); not researched |
| Game settings, scoring, physics, UI, language | reload or restart | loaded once (globals, settings bundles in compartment 0); individual values could be patched in memory, case by case |

## Experiments for Zee (in game)

- **E1, existing item changed.** Install a recolour of an item you are *not* wearing, apply it with the
  level *not* reloaded (console `mods apply`, without `reload`), then put the item on.
  Does it show the mod? Then the same with an item you *are* wearing: take it off, wait 30 s, put it on.
- **E2, child table.** Attach a one-row table to `cas_main_bundlereftable` for a mod preset, then wear it by
  recipe (the way remote players' outfits go on). Does the composite load it?
- **E3, delta bundle.** Load a test bundle with `FUN_1441b48d0` in compartment 5 mid-level, release it, load
  another level. Does anything complain (log, crash)?

## Risks

- Holding a compartment-5 handle across a level unload, or releasing it while something uses its assets.
- Two copies of the same asset if the delta bundle is still loaded when the full merged one loads.
- The composite queues bundle loads against a table object: a child table must live as long as the parent.
- Grace-time unloading means a changed item someone is wearing (another player too) can show the old
  copy for a while.
- Everything here is main-thread or job-thread state: changes must run under the same locks the engine uses
  (the layout manager lock for TOCs; the BundleRefTable registry and the cosmetics manager have their own,
  not yet identified).

## Open questions (next RE)

- Which function fills the cosmetics manager's item map (`0x71ee610 + 0x30`) and collection list (`+0x58`)
  at level load, and whether it can be called for one more collection.
- How a BundleRefTable object is created from its resource (to build a child table the engine accepts), and
  the type of the child vector at `data + 0x20`.
- The `BundleUnloadGraceTime` value (E1 measures it).
- Whether compartment 5 is cleared by the level unload regardless of handles (`ResourceCompartmentCleared`).
