# Live UI values for plugin-authored game UI

This is the API half of the Live Controller port. The original combined 2.1.2
Live Controller and Dynamic QuickDrop mods work in game, as confirmed by their
author. That confirmation does not yet cover this refactor onto current main.

## API

Client plugins declare `ui.models` in `reskate-plugin.json`:

```lua
local symbol = reskate.ui.symbol("width")
local ok, why = reskate.ui.values({left = 100, top = 165, width = 620})
```

`symbol(name)` is a 32-bit unsigned number, exactly representable in Luau. It is
the native name hash (start 5381, multiply by 33 then xor each ASCII byte, modulo
2^32) of `ReSkate.Plugin.<mod folder>.<name>`. Asset builders use it for the
`SourceContext` of a client-only float DataPath listener. Neither the plugin nor
the asset needs to know an engine address or generation-tagged model handle.
Keep the mod folder name stable: renaming it changes its symbols.

`values(table)` publishes floats synchronously on the client thread under the
native model write lock. It returns `true`, or `false, reason` when the native UI
is not ready or registration fails. Retry on subsequent ticks; do not treat
startup unavailability as a permanent plugin error. Empty tables do nothing.
Omitted names keep their previous values; this is not a replacement operation.
Publication is not transactional: on native failure earlier entries may already
have been published. The caller should retry the complete desired state.

Names are 1–48 ASCII letters, digits, `_` or `-`. Values must be numbers, finite,
and within ±100000. A plugin can reserve 32 names over its lifetime; the process
can reserve 256. Reservations survive unload/reload because UI assets can retain
their aliases. Unload publishes zero to that plugin's values, without destroying
models still referenced by assets. A consumer should treat width/visibility zero
as inactive and supply appropriate asset-side defaults if desired.

The native bridge refuses an occupied alias or typed model unless it created
it for that exact plugin/name. Hash collisions fail closed. No stock UI model,
profile option, cosmetic, entitlement or progression field can be addressed.
The server's Game provider reports `unavailable here`. No new hooks are installed.

## Live Controller plugin

`contrib/examples/live-controller-layout` is source for a client plugin. Copy it
to `Mods/Live_Controller_Layout` and pair it with a replay overlay built to listen
to that folder's `left`, `top` and `width` symbols. The source contains no EA assets
and deliberately does not redistribute the compiled replay UI/data partitions.

The 2.1.2 companion's old symbols (`0xec710101`–`0xec710103`) are **not** accepted
by this API. Regenerate its three client-only float listeners using
`reskate.ui.symbol` (or the equivalent builder hash above). Leave the recorded
controller-input listeners, replay-only mounts, saved skin/toggle and Hide UI /
export mounts unchanged. The plugin reads live input only to edit geometry;
it does not substitute live input for recorded replay input.

The editor uses the existing invisible `ui.screen` for input ownership. F8 starts
or cancels, left stick moves, right stick resizes about the centre, LB/L1 is fine
adjustment, A/Enter saves, B/Escape cancels, X resets. Controller disconnect
disarms the buttons until release/reconnect; focus loss and level unload cancel.
Saving uses the sandboxed files API's temporary-file/rename path. Existing global
`controller-overlay-layout.json` is not read or overwritten: copy its geometry
into this plugin's `layout.json` data file to migrate intentionally.

With the original mod's complete authoring sources and ReSkate Studio installed,
the adapter regenerates the assets locally without modifying those sources:

```powershell
python contrib/examples/live-controller-layout/build_assets.py --builders <ReplayControllerOverlay/tools> --game <game-folder> --output <separate-output-folder>
```

It replaces only the graph's three symbolic constants, invokes the original
builder/compiled-payload validation, and packages the plugin alongside the asset
mod. It also replaces the packaged graph source with the source actually used.
The authoring-source and Studio dependency is intentional: no extracted EA data
or compiled game partitions are submitted to this repository.

## Verification

- Local validation: Release runtime and plugin-test targets built successfully;
  plugin tests passed (15 optional, absent contributed plugins skipped). The
  Live Controller example is included in the tests, not one of those skips.
- The adapter rebuilt seven UI payloads and checked 36 bundle occurrences;
  compiled payload bytes matched the project (game head 625200). Output was
  written only to a separate temporary folder, not installed in the game.
- Build `dingosdk_runtime` and `dingosdk_plugin_tests` with plugin tests enabled.
- Host regression tests cover permissions, namespacing, argument type/range,
  nonfinite numbers, embedded NULs, count limits and unavailable providers.
- Native alias creation/publication and editor behavior after this refactor need
  in-game regression testing; fake-host tests do not establish those behaviors.
- Check both skins, recorded inputs while scrubbing/paused/reversed, Hide UI and
  video export; movement/resize, save/reload/cancel, focus loss/disconnect, mod
  disable/reload and a level change. The original 2.1.2 result remains distinct.

Dynamic QuickDrop physics/marker integration belongs to a separate contribution.
