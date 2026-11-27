#pragma once
#include <Windows.h>

namespace dingosdk {
// Win32 failures from Detours are preserved; negative values describe the
// SDK-owned lifecycle. No vendor status codes escape through feature code.
enum HookStatus : LONG {
    HookOk = NO_ERROR,
    HookNotInitialized = -1,
    HookAlreadyInitialized = -2,
    HookAlreadyPrepared = -3,
    HookDisabled = -4,
    HookNotFound = -5,
    HookNotExecutable = -6,
    // The patch helpers (patches.h): the place does not hold what the caller expected, so nothing
    // was written; a rel32 jump cannot reach its destination.
    HookBytesDiffer = -7,
    HookOutOfReach = -8,
    HookOutOfMemory = ERROR_NOT_ENOUGH_MEMORY,
    HookProtectionFailed = ERROR_ACCESS_DENIED,
    HookUnsupportedFunction = ERROR_INVALID_BLOCK,
};

// A prepared original is a permanent x64 relay. Detours changes its pointer
// transactionally, so originals published into atomics or retained by native
// callbacks stay callable before attach and after detach. Only Detours performs
// instruction relocation and target patching. Preparation never patches target.
HookStatus WINAPI hook_initialize();
// original may be null when the replacement never forwards to the target.
HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original = nullptr);
HookStatus WINAPI hook_enable(void* target);
HookStatus WINAPI hook_disable(void* target);
HookStatus WINAPI hook_remove(void* target);
HookStatus WINAPI hook_queue_enable(void* target);
HookStatus WINAPI hook_apply_queued();
// While one is alive on a thread, hook_enable on that thread only queues the hook (and answers
// HookOk); the outermost one, as it ends, attaches all of them in one transaction. Each
// transaction lists every thread on the system and suspends every one of the game's, a few
// milliseconds each, so installs made while the game runs are grouped: on the client thread,
// a frame paid for each of them (2026-10-15). A hook that then fails to attach stays detached,
// as a failed hook_enable leaves it.
class HookBatch {
public:
    HookBatch() noexcept;
    ~HookBatch();
    HookBatch(const HookBatch&) = delete;
    HookBatch& operator=(const HookBatch&) = delete;
};
const char* WINAPI hook_status_string(HookStatus status);
// Captured on the calling thread; formatting/logging happens only after Detours
// has resumed enlisted threads. Read before issuing another hook operation.
struct HookFailure { const char* operation{"none"}; LONG error{}; DWORD thread_id{}; };
HookFailure hook_last_failure() noexcept;
// What one hook operation met when threads refused suspension under its transaction: how many
// tries started over, how long it waited for such threads in all and at the longest, and the id
// of the last such thread. The service waits and starts over without a word; the runtime writes
// these to the log (bootstrap.cpp), so a log shows when a transaction met one and what that cost.
struct HookRetries { unsigned tries{}; unsigned long long waited_us{}, longest_wait_us{}; DWORD thread_id{}; };
// Called on the calling thread after a hook operation that waited or started over, once every
// thread runs again and the service's lock is released, with the operation's result: HookOk when
// the threads it met were ending and the operation went through, a failure when one refused and
// did not end or the tries ran out. One observer for the process; null takes it away. It must not
// call the hook service.
using HookRetryObserver = void (*)(const HookRetries&, HookStatus result);
void WINAPI hook_observe_retries(HookRetryObserver observer) noexcept;
}
