#pragma once
#include <Windows.h>
#include <cstddef>

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
HookStatus WINAPI hook_shutdown();
const char* WINAPI hook_status_string(HookStatus status);
// Captured on the calling thread; formatting/logging happens only after Detours
// has resumed enlisted threads. Read before issuing another hook operation.
struct HookFailure { const char* operation{"none"}; LONG error{}; DWORD thread_id{}; };
HookFailure hook_last_failure() noexcept;

struct HookStatistics { std::size_t prepared{}, attached{}, retired{}; };
HookStatistics hook_statistics();
}
