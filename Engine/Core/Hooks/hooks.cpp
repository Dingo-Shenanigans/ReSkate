#include "hooks.h"
#include <detours.h>
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dingosdk {
namespace {
thread_local HookFailure last_failure;
LONG failure(const char* operation, LONG error, DWORD thread = 0) {
    last_failure = {operation, error, thread}; return error;
}
struct Hook {
    void* target{};
    void* replacement{};
    void* relay{};
    void** original{};
    bool attached{}, queued{}, retired{};
};
struct Registry {
    std::mutex mutex;
    bool initialized{};
    std::map<void*, Hook*> targets;
    std::vector<std::unique_ptr<Hook>> lifetime;
};
Registry& registry() {
    // Native callbacks can retain originals until process teardown. Deliberately
    // retain their tiny relays; freeing them on remove/shutdown would invalidate
    // function pointers that feature code already published.
    static auto* value = new Registry;
    return *value;
}
HookStatus status(LONG error) { return static_cast<HookStatus>(error); }
bool executable(void* address) {
    MEMORY_BASIC_INFORMATION page{};
    if (!address || !VirtualQuery(address, &page, sizeof(page)) || page.State != MEM_COMMIT ||
        (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = page.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}
HookStatus create_relay(Hook& hook) {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    auto* allocation = static_cast<std::byte*>(VirtualAlloc(nullptr, page * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation) return HookOutOfMemory;
    // jmp qword ptr [rip+disp32]. A separate writable page holds the pointer
    // managed by Detours. No register/stack changes, and no RWX code page.
    std::array<std::byte, 6> jump{std::byte{0xff}, std::byte{0x25}};
    const auto displacement = static_cast<std::int32_t>(page - jump.size());
    std::memcpy(jump.data() + 2, &displacement, sizeof(displacement));
    std::memcpy(allocation, jump.data(), jump.size());
    auto** pointer = reinterpret_cast<void**>(allocation + page);
    *pointer = hook.target;
    DWORD old{};
    if (!VirtualProtect(allocation, page, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation, jump.size())) {
        const auto error = GetLastError(); VirtualFree(allocation, 0, MEM_RELEASE); return status(error);
    }
    hook.relay = allocation; hook.original = pointer;
    return HookOk;
}
struct Threads {
    std::vector<HANDLE> handles;
    ~Threads() { for (const auto thread : handles) CloseHandle(thread); }
    LONG collect() {
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return failure("enumerate threads", static_cast<LONG>(GetLastError()));
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        LONG error = NO_ERROR;
        if (!Thread32First(snapshot, &entry)) error = static_cast<LONG>(GetLastError());
        else do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                FALSE, entry.th32ThreadID);
            if (!thread) {
                const auto failure = GetLastError();
                if (failure == ERROR_INVALID_PARAMETER) continue; // Thread exited during enumeration.
                // A terminating thread can reject full access while its object
                // is still in the snapshot. Skip only an independently verified
                // signaled thread; a live inaccessible thread must fail safely.
                const auto check = OpenThread(SYNCHRONIZE, FALSE, entry.th32ThreadID);
                const bool exited = check && WaitForSingleObject(check, 0) == WAIT_OBJECT_0;
                if (check) CloseHandle(check);
                if (exited) continue;
                error = dingosdk::failure("open thread", static_cast<LONG>(failure), entry.th32ThreadID); break;
            }
            const auto owner = GetProcessIdOfThread(thread);
            if (!owner) {
                const auto query_error = GetLastError();
                const bool exited = WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
                CloseHandle(thread);
                if (exited) continue;
                error = failure("query thread owner", static_cast<LONG>(query_error), entry.th32ThreadID); break;
            }
            if (owner != GetCurrentProcessId()) { CloseHandle(thread); continue; }
            try { handles.push_back(thread); }
            catch (...) { CloseHandle(thread); CloseHandle(snapshot); throw; }
        } while (Thread32Next(snapshot, &entry));
        if (error == NO_ERROR && GetLastError() != ERROR_NO_MORE_FILES)
            error = static_cast<LONG>(GetLastError());
        CloseHandle(snapshot);
        if (error != NO_ERROR && !last_failure.error) failure("enumerate threads", error);
        return error;
    }
};
struct Change { Hook* hook; bool attach; };
HookStatus transact(const std::vector<Change>& changes) {
    if (changes.empty()) return HookOk;
    // Enumerate/open handles and allocate all project-owned storage before any
    // thread is suspended. The declared Detours vendor patch keeps its transaction
    // records off the shared heap for the same reason, including commit/abort.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        Threads threads;
        auto error = threads.collect();
        if (error != NO_ERROR) return status(error);
        error = DetourTransactionBegin();
        if (error != NO_ERROR) return status(failure("begin transaction", error));
        for (const auto& change : changes) {
            error = change.attach ? DetourAttach(change.hook->original, change.hook->replacement)
                : DetourDetach(change.hook->original, change.hook->replacement);
            if (error != NO_ERROR) {
                DetourTransactionAbort();
                return status(failure(change.attach ? "attach target" : "detach target", error));
            }
        }
        bool retry = false;
        for (const auto thread : threads.handles) {
            if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) continue;
            error = DetourUpdateThread(thread);
            if (error == NO_ERROR) continue;
            const auto thread_id = GetThreadId(thread);
            const bool exited = WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
            // UpdateThread poisons the transaction on failure. Abort/resume all
            // threads, then recollect; never commit after a failed enlistment.
            DetourTransactionAbort();
            if (!exited || attempt == 3) return status(failure("enlist thread", error, thread_id));
            retry = true;
            break;
        }
        if (retry) continue;
        error = DetourTransactionCommit();
        if (error != NO_ERROR) return status(failure("commit transaction", error));
        for (const auto& change : changes) change.hook->attached = change.attach;
        return HookOk;
    }
    return HookProtectionFailed;
}
template<class F> HookStatus locked(F action) {
    try {
        auto& r = registry(); std::lock_guard lock(r.mutex);
        last_failure = {};
        return action(r);
    } catch (const std::bad_alloc&) { return HookOutOfMemory; }
}
Hook* find(Registry& r, void* target) {
    const auto found = r.targets.find(target);
    return found == r.targets.end() ? nullptr : found->second;
}
}

HookStatus WINAPI hook_initialize() {
    return locked([](Registry& r) {
        if (r.initialized) return HookAlreadyInitialized;
        r.initialized = true; return HookOk;
    });
}
HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original) {
    if (original) *original = nullptr;
    return locked([&](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        if (!executable(target) || !executable(replacement) || target == replacement) return HookNotExecutable;
        if (r.targets.contains(target)) return HookAlreadyPrepared;
        auto hook = std::make_unique<Hook>();
        hook->target = target; hook->replacement = replacement;
        // Reserve containers before allocating the executable relay, so failure
        // cannot strand an unpublished allocation or a half-registered target.
        if (r.lifetime.size() == r.lifetime.capacity())
            r.lifetime.reserve(std::max<std::size_t>(16, r.lifetime.capacity() * 2));
        const auto [entry, inserted] = r.targets.emplace(target, hook.get());
        (void)inserted;
        const auto result = create_relay(*hook);
        if (result != HookOk) { r.targets.erase(entry); return result; }
        if (original) *original = hook->relay;
        r.lifetime.push_back(std::move(hook));
        return HookOk;
    });
}
namespace {
thread_local unsigned batch_depth{}, batch_queued{};
}
HookStatus WINAPI hook_enable(void* target) {
    return locked([&](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        auto* hook = find(r, target); if (!hook) return HookNotFound;
        if (hook->attached) return HookOk;
        if (batch_depth) {
            hook->queued = true;
            ++batch_queued;
            return HookOk;
        }
        return transact({{hook, true}});
    });
}
HookBatch::HookBatch() noexcept { ++batch_depth; }
HookBatch::~HookBatch() {
    if (--batch_depth || !std::exchange(batch_queued, 0)) return;
    (void)hook_apply_queued();
}
HookStatus WINAPI hook_disable(void* target) {
    return locked([&](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        auto* hook = find(r, target); if (!hook) return HookNotFound;
        if (!hook->attached) return HookDisabled;
        return transact({{hook, false}});
    });
}
HookStatus WINAPI hook_remove(void* target) {
    return locked([&](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        auto* hook = find(r, target); if (!hook) return HookNotFound;
        if (hook->attached) {
            const auto result = transact({{hook, false}});
            if (result != HookOk) return result;
        }
        hook->retired = true; hook->queued = false; r.targets.erase(target);
        return HookOk;
    });
}
HookStatus WINAPI hook_queue_enable(void* target) {
    return locked([&](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        auto* hook = find(r, target); if (!hook) return HookNotFound;
        hook->queued = true; return HookOk;
    });
}
HookStatus WINAPI hook_apply_queued() {
    return locked([](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        std::vector<Change> changes;
        for (const auto& [target, hook] : r.targets) {
            (void)target;
            if (hook->queued && !hook->attached) changes.push_back({hook, true});
        }
        const auto result = transact(changes);
        if (result == HookOk) for (const auto& [target, hook] : r.targets) { (void)target; hook->queued = false; }
        return result;
    });
}
HookStatus WINAPI hook_shutdown() {
    return locked([](Registry& r) {
        if (!r.initialized) return HookNotInitialized;
        std::vector<Change> changes;
        for (const auto& [target, hook] : r.targets) { (void)target; if (hook->attached) changes.push_back({hook, false}); }
        const auto result = transact(changes);
        if (result != HookOk) return result;
        for (const auto& [target, hook] : r.targets) { (void)target; hook->retired = true; hook->queued = false; }
        r.targets.clear(); r.initialized = false; return HookOk;
    });
}
HookFailure hook_last_failure() noexcept { return last_failure; }
const char* WINAPI hook_status_string(HookStatus value) {
    switch (value) {
    case HookOk: return "success";
    case HookNotInitialized: return "hook service not initialized";
    case HookAlreadyInitialized: return "hook service already initialized";
    case HookAlreadyPrepared: return "target already registered";
    case HookDisabled: return "hook already detached";
    case HookNotFound: return "target not registered";
    case HookNotExecutable: return "target or replacement is not executable";
    case HookOutOfMemory: return "out of memory";
    case HookProtectionFailed: return "memory protection or thread access denied";
    case HookUnsupportedFunction: return "Detours cannot relocate target";
    default: return "Detours transaction failed (Win32 status)";
    }
}
HookStatistics hook_statistics() {
    auto& r = registry(); std::lock_guard lock(r.mutex);
    HookStatistics result;
    for (const auto& hook : r.lifetime) {
        if (hook->retired) ++result.retired;
        else { ++result.prepared; if (hook->attached) ++result.attached; }
    }
    return result;
}
}
