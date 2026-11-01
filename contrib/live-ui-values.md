# Live UI values for plugin-authored game UI

This is the **generic ReSkate API** half of the Live Controller port, not the
Live Controller mod itself. The author's original combined Live Controller 2.1.2
and Dynamic QuickDrop mods worked in-game; that does **not** validate this
new API/refactor against the current game build.

## API

Client-side plugins declare `"ui.models"` in `reskate-plugin.json`:

```lua
local symbol = reskate.ui.symbol("width")
local ok, reason = reskate.ui.values({left = 100, top = 165, width = 620})
-- Subsequent calls merge: this updates only width.
local updated, why = reskate.ui.values({width = 640})
```

`symbol(name)` is a 32-bit unsigned number, exactly representable in Luau:
start at 5381, multiply by 33, then xor each ASCII byte (modulo 2^32) of
`ReSkate.Plugin.<mod folder>.<name>`. The mod's **installed folder name must
match the name used while compiling assets**; renaming the folder changes the
symbols and silently disconnects the UI listeners. Asset authors use the hash
for a client-only float DataPath's `SourceContext`. Plugins never receive a
native pointer, type or model handle.

`values(table)` returns `true` on success or `false, reason` on UI startup
unavailability or registration failure. Retry when unavailable, but avoid
publishing on every tick when your value has not changed. The native bridge
also short-circuits identical values already registered in the current UI
manager, without acquiring the exclusive model lock or inserting aliases.
A UI manager change forces registration again. Empty tables do nothing.
Omitted fields retain previous values: updates **merge**, not replace.
Publication is not transactional on a native error; earlier entries in the
batch may have succeeded. Retry the complete desired state after failure.

Names must be 1–48 ASCII letters, digits, `_` or `-`. Values are finite floats
within ±100000. Each plugin can reserve at most 32 distinct names over its
lifetime; at most 256 are reserved for the entire process. The pure registry
refuses invalid names, same-batch or cross-plugin symbol collisions, and quota
overflow *before* reserving any part of that batch. Native registration
failures still consume reserved names to prevent unbounded alias growth.
Reservations survive plugin unload/reload because UI assets may retain aliases.

Unloading zeroes **only the handles already published by that plugin into the
current UI manager**, never creating new aliases or models. Consumers should
treat a zero width/visibility value as inactive. The bridge refuses an occupied
stock alias or typed model, including another producer's float field. A
32-bit hash collision with a stock symbol **before** that stock alias loads
cannot be predicted: the stock loader may subsequently overwrite the alias.
These display-only values cannot access stock profile, progression, cosmetic
or entitlement fields. No new hooks are installed.

`ui.models` is **client-only**. Both `server` and `both` plugins running in a
listen host's `@server` copy are refused before any reservation or native
write. Dedicated server Game providers remain unavailable.

## Separate Live Controller mod and assets

The Live Controller layout plugin and its `build_assets.py` adapter are **not
part of this repository**. Distribute them together as a separate mod and keep
local, untracked development sources under
`contrib/plugins/Live_Controller_Layout` when testing. This matches
[STRUCTURE.md](../STRUCTURE.md) and the optional contributed-plugin tests.
The adapter requires the original unpublished 2.1.2 authoring source tree
and ReSkate Studio; it cannot run from this repository alone. No extracted
EA assets, game partitions or compiled replay binaries are distributed here.

To migrate the original mod, rebuild **only** its three layout listeners
(`left`, `top`, `width`) using the hashed symbols for the actual installed mod
folder, and leave the replay-recorded input listeners, skin/toggle state,
Hide UI and export mounts unchanged. Original fixed symbols
`0xec710101`–`0xec710103` are not supported by this API.
The editor uses `ui.screen` for exclusive input and sandboxed files for
layout persistence. It must not substitute live pad input for recorded replay
input while scrubbing or exporting.

## Verification and remaining work

- Prior to this review, the original branch's Windows Release runtime and
  plugin tests built and passed; seven UI payloads were regenerated from the
  author's local mod sources, with 36 bundle occurrences verified. This was
  **before** the review fixes described here, not validation of this revision.
- The self-contained plugin tests now exercise the actual shared reservation
  logic: incremental merge, per-plugin and process quotas, same- and
  cross-plugin hash collisions, atomic rejection, and manager-aware no-ops.
  They also check permission denial and that listen-server plugin copies
  cannot publish.
- The Live Controller integration test follows the existing optional
  `contrib/plugins/Live_Controller_Layout` skip-when-absent convention.
- Rebuild `dingosdk_runtime` and `dingosdk_plugin_tests` on Windows.
  Native alias registration and unload cleanup still require in-game tests.
- Check both skins, recorded input paused/scrubbed/reversed, Hide UI/export,
  layout move/resize/save/cancel, focus loss, disconnect, reload/unload and
  level changes. Successful testing of 2.1.2 does not count as testing this
  refactor.

Dynamic QuickDrop physics/marker changes remain a separate contribution.
