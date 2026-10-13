# Experimental flat-culling work claims

This build starts with a batch size of eight. It modifies the work-claim operation in Skate's
existing flat-culling helper; all native visibility tests and output-flush reservations still run.
The octree/tree path is not modified by this first implementation.

## Console

```
perf cullbatch
perf cullbatch 1
perf cullbatch 8
perf status
```

`1` (or `0`) selects native one-item scheduling. The hooks/instrumentation remain installed, so
this is not a fully unhooked baseline. Valid batch sizes are 1, 2, 4, 8, 16 and 32; the choice is
session-local and resets to eight on launch. The command reports cumulative items/reservations.
The profiler zone `engine/cull flat` measures the native helper plus the wrapper while profiling
is enabled. Compare the same San Van route/settings and allow loading to settle between runs.
For a completely unhooked comparison, close the game and restore the pre-install DLL backup.

## Implementation boundary

- Addresses and byte fingerprints are in `Engine/Game/Build/20260929/cull_batch.h`.
- The full helper gets a scoped thread-local claim cache. Nested calls restore the outer scope.
- Only the two five-byte `XADD.LOCK [R15],EAX` input-claim sites are hooked.
- Detours performs instruction relocation and attachment in one queued transaction.
- A cached item uses an integer-only gateway; no callback or vector save is needed.
- A cache refill preserves general registers and all sixteen YMM registers before invoking the
  bounded range reservation. Native comparisons consume the claimed index afterward.
- Reservations stop at the input bound. A worker still makes one native terminal claim, and
  output reservations are unchanged. Live setting changes apply when a new helper scope begins,
  rather than abandoning work already reserved by a running scope.
- Unsupported fingerprints or AVX availability leave this feature unavailable.

The generated gateways do not carry a complete unwind description; a stack sample interrupted
inside a refill gateway may have incomplete unwinding. This does not establish a game crash or
FPS benefit. No in-game benchmark has yet been collected.

## Checks

```
cmake --preset vs2022-x64 -DDINGOSDK_BUILD_CULL_BATCH_TESTS=ON
cmake --build --preset release --target dingosdk_cull_batch_tests --parallel 4
ctest --test-dir build/vs2022-x64 -C Release -R cull_batch --output-on-failure -V
```

The native executable fixtures test the actual gateway and Detours relocation, rather than only
the reservation helper. Checks cover 144 multithreaded combinations of boundaries/batch sizes,
exactly-once processing, terminal-counter behavior, nested caches, native passthrough, and all
YMM-register/pointer preservation on stock, refill and cached paths.

A single synthetic run of 500,000 claims on this machine measured:

| Workers | Unhooked native | Instrumented, size 1 | Instrumented, size 8 |
|---|---:|---:|---:|
| 1 | 1.02 ms | 1.91 ms | 1.72 ms |
| 8 | 3.70 ms | 4.54 ms | 1.46 ms |

These are scheduling-fixture results under current system load, not statistically established
timings or a game speedup. They expose the important tradeoff: lower contention with more
workers versus additional hook overhead, especially when only one worker is active.
