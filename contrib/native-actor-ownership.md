# Native actor ownership and observations

Trusted runtime services share one actor-ownership arbiter before publishing
native work. The owner, sequence, current actor context and ticket stay inside
the runtime. This foundation exposes no new Luau method or permission and gives
plugins no native addresses or claim tokens.

## Ownership

The native adapter captures the validated local client/player/entity/component/
core/context/rig chain and the current world/store. It repeats the same capture
around admission and use, binds it to the exact SDK owner and sequence, and
checks current focus, gameplay input, queued teleports and protection leases.
The trusted caller supplies its own state eligibility predicate; a missing
predicate is refused.

A reservation excludes new SDK optional motion writers before capture. Publish,
entry, revocation and retirement use the same arbiter. Each native invocation
uses an RAII `Call`; returning or dropping an unrelated handle cannot retire
another service's ticket. Exact owner/sequence/actor checks remain in force
through cleanup. A service must resolve its own native duty before requesting
retirement, unless the adapter independently observed this exact actor's
retirement. The foundation adds no equipment request queue or recovery action.

Existing SDK motion publishers and dispatchers enter an `IdleCall` through this
same arbiter. New optional movement is refused while a service owns the actor.
Neutral cancellation and restoration remain available so a claim cannot trap
old settings or leave a partially restored native constant behind. Refused
velocity, cruise, push and hold requests reach their existing plugin callers as
failures; hold ownership metadata changes only after acceptance.

## Context and destruction observations

The shared destruction hook advances an SDK epoch around every destruction,
including exceptional native exits, and has independent bounded observers.
Captures require the same idle epoch before and after their work. A watched
local entity also has a separate destruction counter.

`native_lifetime` continuity retains the captured global lifetime fence.
`observed_context` continuity instead compares the same complete local actor
and world identity with a context generation and the watched actor's destruction
counter; unrelated global destruction does not become a new actor identity.
Both still bracket their current observations with the real destruction epoch.
Neither mode pins native memory or proves durable child-object incarnation.

A copied observation, equal address or unchanged SDK counter is evidence for
that sampled edge, not a native allocation lease. Unknown, busy, stale or
changed observations refuse publication or continuation.

## Teleports

The actual placement and position teleport queues carry a guarded lifecycle
stamp. Enqueue, dispatch, replacement, cancellation and profile lifecycle changes
advance that stamp. The native adapter takes paired Try-only observations
around owner validation, so a queue that changed and returned to idle is not
mistaken for one that stayed idle. It does not hold the broad profile mutex in
a controller callback or accept a caller-supplied assertion that teleporting
has finished.

## Native-step clock

A service may enable the clock for its exact owner/sequence/actor ticket.
The existing qualified skeleton response supplies real physics-step observations;
Lua timers, controller packet counters and client ticks do not mint steps.
Clock reads repeat actor/context and claim checks and leave no sample on
refusal. The shared key cannot be reused by a new ticket or another service.
Disabling observation grants no cleanup or retirement proof.

## Client publication and failures

The client tick publishes actor readiness after the local owner is available
and before plugin consumers run. Loading, a skipped/unpublished client frame,
an observer failure or an observation exception publishes false readiness.
A stale earlier success cannot silently authorize a later native invocation.
Each consuming service still owns its own deadline, menu policy and cleanup.

## Validation

Tests cover the arbiter, actor clock, context continuity, bounded callback
registry and paired teleport queue. Contended RAII returns check exact callback
drain, barriers and generation turnover. The teleport suite also compiles the
production profile entry points over constructed state: actor qualification
includes placement work accepted before its opening observation, while the
position-dispatch query retains its separate queue contract. These constructed
tests and a clean build
do not qualify native actor lifetime, callback ABI, native destruction ordering
or controller consumption. Each consuming API needs exact-head runtime tests
and bounded native trials in addition to this foundation's checks.
