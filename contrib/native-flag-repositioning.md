# Repositioning an owned native flag

This general SDK addition lets any plugin reposition its own waiting host flag. It does not contain a game mode, asset path, HUD, course format, or player teleport. The plugin retains those responsibilities.

Permission: the existing `throwdowns` permission.

```lua
local ok, request = reskate.throwdowns.flag.restore("duel", game, {
    position = vector.create(x, y, z),
    yaw = facing_degrees,
})
if not ok then
    reskate.log(request, "warn") -- false, reason; the current flag is unchanged
end
```

The mode is the caller's short registered id. The game number must match its current owned host registration. Position is finite, less than 100 km on every axis; yaw is finite degrees between -100000 and 100000. Yaw in the completion event is normalized to the native facing. Other native parameters, including the flag's InstanceId, capacity and settings arrays, are retained.

`true, request` means accepted for asynchronous dispatch. The request is an exactly representable integer, unique for this runtime. `flag.state().restoring` is true and its waiting Start action is blocked until the request finishes. The plugin must continue publishing its waiting card or native editor document. Adopted guest flags, another mode/game, active matches and concurrent requests are refused.

Completion events use the existing `throwdown_ui` event and the caller's mode:

| Action | Args |
| --- | --- |
| `flag_restored` | `{game, x, y, z, yaw_degrees, request}` |
| `flag_restore_failed` | `{game, reason, request}` |

Match mode, game and request before committing any staged data. Successful completion means the existing native `UpdateThrowdownParameters` event was dispatched with that pose; it is not a server replication acknowledgment. Native visual propagation still needs an in-game check. A failed or expired dispatch preserves the original stored pose and registration. Leaving, unloading or changing world cancels pending work. A late job cannot target a reused queue id or complete another request.

The implementation never destroys/recreates the registration and never navigates through setup or placement. It rebuilds the complete captured native parameter object, changes only SpawnPoint and TeleportLocation, validates the request again, then uses the existing native event dispatch. No new game hook or build-specific address is introduced.

Player travel remains separate: `reskate.skater.teleport(position, heading_radians)` is already supported, with permission `skater.teleport`. Wait for the player's observed position and `skater.telemetry().teleport_pending == false`; a true return means the teleport was sent, not that it arrived.

## Validation

Enable `DINGOSDK_BUILD_PLUGIN_TESTS` and `DINGOSDK_BUILD_MULTIPLAYER_TESTS`, then build `dingosdk_runtime`, `dingosdk_plugin_tests` and `dingosdk_native_flag_session_tests` in Release. Run CTest filtered to `plugins` and `native_flag_session`.

The API tests include a caller without permission, invalid arguments, namespaced ownership and refusal. Native session tests exercise two unrelated mode ids, Start suppression, no navigation/destruction, duplicate requests, stale callbacks, failure, timeout, cancellation, adopted guests, active matches and reuse of the same native queue id in a new world. The existing runtime build retains warnings as errors.

For live validation, place a flag with each of two unrelated plugin modes. Reposition it with different positions and headings, keeping the native waiting/join/Hold Start flow. Verify the pole, ground marker, map/join position and remote replicas move; setup and placement must stay closed. Start and quit afterward. Repeat with cancellation, a map change and an unavailable native update. No live game result is claimed by the offline tests.
