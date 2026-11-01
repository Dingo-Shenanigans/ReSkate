# Preparing native bodies: `reskate.world.physics.prepare`

`reskate.world.physics.prepare()` lets a mod ask for the body producer's one-time setup before its
first real allocation, so that `create` is not the call that discovers the setup is pending. See
[owned native bodies](plugin-owned-bodies.md) for the rest of the API.

The call needs `world.physics` and the client phase. It takes no arguments: no owner, definition,
handle, callback or proof. A VM may call it once per plugin tick; a pending, unavailable or ready
answer all use that call, and a second call in the same tick returns `false, reason` without
reaching the game.

```lua
local ready, why = reskate.world.physics.prepare()
-- ready is a boolean, never a body or a native identity.
```

`true` means the producer is hooked and the call could capture the local client and world chain
afresh with a matching 60 Hz step clock. It is a sampled prerequisite, not a reserved allocation
or a promise of capacity: the slots can still run out, an earlier partial allocation can still
prohibit another, and the state can change before the later `create`.

`false, reason` is one of:

| Reason | Meaning |
| --- | --- |
| `native body producer setup pending; call prepare again explicitly` | The call only asked for the deferred one-time setup. |
| `waiting for a fresh qualified 60 Hz physics step` | The producer is hooked but the local skater, world or clock cannot be captured now. |
| `native body ABI or producer hook unavailable; another producer may own it` | The one-time setup already ran and failed. It is not retried. |
| `owned bodies require the current local client phase outside multiplayer` | The client phase is not ready, is stale, is another thread's, or a session is on. |
| `native body preparation failed closed` | An exception was caught before anything was requested. |

Asking twice in a tick gives `asked to prepare native bodies more than once in one tick`.

A pending call keeps no plugin owner, definition, position, allocation or slot. Only the next
`owned_bodies::tick` may run the setup, outside the plugin VM's time budget and the service lock;
a tick outside the client phase drops the request, and returning to the client phase does not
bring it back. The setup registers no other hook and skips none of the byte checks. A ready call
reads only the capture and the clock: it reconciles no world, retires no slot, changes no mailbox
and calls no native allocator, setter or force.

A one-shot body mod should prepare explicitly, wait for a later tick after a pending answer, call
`prepare` again, and only after `true` make its single `create`. Failed `create` calls are not a
readiness probe; the API retries nothing for the caller.

## Tests

The plugin tests (`Extension/Plugins/Test/body_prepare_host_fixture.inc`) cover the missing
permission, the server side, argument refusal, the per-tick charge and its reset, the pending,
ready and refused answers, independent VMs and reload, all without any body side effect, against a
constructed game. The service test (`Extension/World/Test/body_prepare_source_fixture.inc`) runs
`owned_bodies::prepare` over a constructed client, world, TLS and clock: a cold call only sets the
setup request, the failing setup runs once outside the lock and is not retried, the ready path
needs a fresh capture and clock, and existing slots and the cached world are left as they were.
Neither shows the real setup succeeding in the game.
