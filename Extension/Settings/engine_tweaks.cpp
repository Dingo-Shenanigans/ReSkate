#include "engine_tweaks.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine_tweaks.h"
#include "Extension/Settings/job_spin.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>

#pragma intrinsic(_ReturnAddress)

namespace dingosdk::engine_tweaks {
namespace {
namespace et = addr::engine_tweaks;
using Fingerprint = game::build::Fingerprint;
constexpr std::size_t tweak_count = 5;
constexpr double macos_wine_default_job_wake_us = 50.0;
std::uintptr_t base{};
std::atomic<bool> installed{};
std::array<std::atomic<bool>, tweak_count> ready{};
std::mutex toggling;

std::size_t slot(Tweak tweak) noexcept { return static_cast<std::size_t>(tweak); }

// Running under Wine on macOS (CrossOver), where x86_64 runs under Rosetta 2 across hybrid P/E
// cores and worker semaphore wakeups go through wineserver.
bool under_macos_wine() noexcept {
    static const bool macos = []() noexcept {
        using HostVersion = void(__cdecl*)(const char**, const char**);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto host = ntdll ? reinterpret_cast<HostVersion>(GetProcAddress(ntdll, "wine_get_host_version")) : nullptr;
        const char* sysname{};
        if (host) host(&sysname, nullptr);
        return sysname && std::string_view(sysname) == "Darwin";
    }();
    return macos;
}

template<std::size_t ContractSize, std::size_t PatchSize>
bool apply_inline_guard(std::uintptr_t contract_rva, const std::array<unsigned char, ContractSize>& contract,
    std::uintptr_t patch_offset, const std::array<unsigned char, PatchSize>& patch) noexcept {
    static_assert(PatchSize <= ContractSize);
    std::array<unsigned char, ContractSize> current{};
    if (!memory::peek(base + contract_rva, current)) return false;
    auto expected_patched = contract;
    std::memcpy(expected_patched.data() + patch_offset, patch.data(), PatchSize);
    if (current == expected_patched) return true;
    if (current != contract) return false;
    auto* const destination = reinterpret_cast<void*>(base + contract_rva + patch_offset);
    DWORD previous{}, discarded{};
    if (!VirtualProtect(destination, PatchSize, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(destination, patch.data(), PatchSize);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, PatchSize) != FALSE;
    return VirtualProtect(destination, PatchSize, previous, &discarded) != FALSE && flushed;
}

void install_macos_wine_stall_guards() noexcept {
    if (!under_macos_wine()) return;
    const bool runner_ok = apply_inline_guard(et::task_queue_runner_exit_rva, et::task_queue_runner_exit_contract,
        et::task_queue_runner_exit_patch_offset, et::task_queue_runner_exit_patch);
    const bool server_ok = apply_inline_guard(et::server_step_reader_lock_rva, et::server_step_reader_lock_contract,
        et::server_step_reader_lock_patch_offset, et::server_step_reader_lock_patch);
    if (!runner_ok || !server_ok)
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Engine tweaks: macOS Wine simulation stall guards differ for this build (TaskQueue runner {}, Server reader lock {}).",
            runner_ok ? "patched" : "skipped", server_ok ? "patched" : "skipped");
}

// Counting on hot paths: a per-thread tally folded into the shared total every 256 counts, so
// it never touches a shared cache line per call (totals lag by up to 255 a thread).
struct Count {
    std::atomic<std::uint64_t> total{};
    void reset() noexcept { total.store(0, std::memory_order_relaxed); }
    std::uint64_t value() const noexcept { return total.load(std::memory_order_relaxed); }
};
inline void bump(std::uint32_t& local, Count& count) noexcept {
    if (++local == 256) {
        count.total.fetch_add(256, std::memory_order_relaxed);
        local = 0;
    }
}

// The job system's own TSC rate when it is known, else a short measurement against QPC.
double ticks_per_microsecond() noexcept {
    static const double rate = [] {
        if (const auto engine = job_spin::ticks_per_microsecond(); engine && *engine > 0) return *engine;
        LARGE_INTEGER frequency{}, first{}, second{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&first);
        const auto start = __rdtsc();
        Sleep(20);
        QueryPerformanceCounter(&second);
        const auto ticks = __rdtsc() - start;
        const double microseconds = static_cast<double>(second.QuadPart - first.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
        return microseconds > 0 ? static_cast<double>(ticks) / microseconds : 3000.0;
    }();
    return rate;
}

// ---- 1. GI throttle --------------------------------------------------------------------------
using GiUpdate = void (*)(std::uintptr_t manager, std::uintptr_t mode);
GiUpdate original_gi{};
std::atomic<std::uint64_t> gi_period{}, gi_next{}; // TSC ticks; period 0 = every frame
double gi_rate{};
// Plain counts: a few hundred calls a second, from whichever job thread runs the GI frame.
std::atomic<std::uint64_t> gi_ran{}, gi_skipped{}, gi_loop{};

void gi_hook(std::uintptr_t manager, std::uintptr_t mode) {
    const auto period = gi_period.load(std::memory_order_relaxed);
    const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Only the once-a-frame call: the multi-iteration path runs while the game wants GI to
    // converge, and is left alone.
    if (period && from == base + et::gi_update_frame_return) {
        const auto now = __rdtsc();
        const auto next = gi_next.load(std::memory_order_relaxed);
        if (now < next) {
            gi_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Keep the rate's phase (frames land just after each slot), but never catch up in a burst.
        const auto following = next + period;
        gi_next.store(following > now ? following : now + period, std::memory_order_relaxed);
    } else if (from == base + et::gi_update_loop_return) {
        gi_loop.fetch_add(1, std::memory_order_relaxed);
    }
    gi_ran.fetch_add(1, std::memory_order_relaxed);
    original_gi(manager, mode);
}

// ---- 2. Mesh cull tree -----------------------------------------------------------------------
using CullSchedule = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
using SettingsLookup = std::uintptr_t (*)(std::uintptr_t manager, std::uintptr_t type);
CullSchedule original_cull{};
std::atomic<int> tree_wanted{-1}; // -1: the game's own value
std::atomic<int> tree_stock{-1};  // the value before ReSkate changed it, while changed
std::atomic<int> tree_seen{-1};   // what the last frame culled with

// At the start of the scheduler, before it or the jobs it starts read the flag: both see the
// same value for the whole frame.
void latch_tree() noexcept {
    const int wanted = tree_wanted.load(std::memory_order_relaxed);
    const int stock = tree_stock.load(std::memory_order_relaxed);
    if (wanted < 0 && stock < 0) return;
    std::uintptr_t manager{};
    if (!memory::peek(base + et::settings_manager, manager) || !manager) return;
    const auto settings = reinterpret_cast<SettingsLookup>(base + et::settings_lookup)(manager, base + et::world_render_settings_type);
    if (!settings) return;
    auto* flag = reinterpret_cast<volatile std::uint8_t*>(settings + et::mesh_cull_tree_enabled);
    if (wanted >= 0) {
        if (stock < 0) tree_stock.store(*flag, std::memory_order_relaxed);
        if (*flag != wanted) *flag = static_cast<std::uint8_t>(wanted);
    } else {
        *flag = static_cast<std::uint8_t>(stock);
        tree_stock.store(-1, std::memory_order_relaxed);
    }
    tree_seen.store(*flag, std::memory_order_relaxed);
}

std::uintptr_t cull_hook(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d, std::uintptr_t e,
    std::uintptr_t f, std::uintptr_t g, std::uintptr_t h, std::uintptr_t i, std::uintptr_t j, std::uintptr_t k) {
    latch_tree();
    return original_cull(a, b, c, d, e, f, g, h, i, j, k);
}

// ---- 3. Job wake -----------------------------------------------------------------------------
using RegisterIdle = std::uintptr_t (*)();
using Push = std::uintptr_t (*)(std::uintptr_t scheduler, std::uintptr_t job);
using Wake = bool (*)(int index);
using LockfreePush = void (*)(std::uintptr_t queue, std::uintptr_t job, std::uintptr_t link);
using Execute = std::uintptr_t (*)(std::uintptr_t queue, std::uintptr_t job);
RegisterIdle original_register{};
Push original_push{};
std::atomic<std::uint64_t> wake_spin{}; // TSC ticks; 0 = stock
double wake_microseconds{};
Count spins, spins_woken, cheap_wakes, full_wakes;
thread_local std::uint32_t spins_local{}, spins_woken_local{}, cheap_local{}, full_local{};
constexpr LONG running = 3, entering_idle = 4;

volatile LONG* state_of(unsigned index) noexcept {
    return reinterpret_cast<volatile LONG*>(base + et::worker_table + index * et::worker_stride + et::worker_state);
}

// Called by the idle wait with the worker at state 4 (entering idle). When nothing was found,
// stay there a little: a pusher wakes us with one CAS (4 -> 3), and the idle wait, seeing 3,
// goes back to work without sleeping.
std::uintptr_t register_hook() {
    const auto found = original_register();
    const auto ticks = wake_spin.load(std::memory_order_relaxed);
    if (found || !ticks) return found;
    const auto block = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58))
        [*reinterpret_cast<const std::uint32_t*>(base + et::tls_index)];
    const auto worker = block ? *reinterpret_cast<const std::uintptr_t*>(block + et::tls_worker) : 0;
    const auto table = base + et::worker_table;
    if (worker < table || worker >= table + 64 * et::worker_stride || (worker - table) % et::worker_stride) return found;
    auto* state = reinterpret_cast<volatile LONG*>(worker + et::worker_state);
    bump(spins_local, spins);
    const auto deadline = __rdtsc() + ticks;
    while (*state == entering_idle) {
        if (__rdtsc() >= deadline) return found;
        _mm_pause();
    }
    bump(spins_woken_local, spins_woken);
    return found;
}

// The engine's push, with one change: before waking a sleeping worker (a semaphore release and a
// context switch), any worker in the mask that is entering idle or spinning above takes the job
// with one CAS. Otherwise it is the original, in its order.
std::uintptr_t push_hook(std::uintptr_t scheduler, std::uintptr_t job) {
    if (!wake_spin.load(std::memory_order_relaxed)) return original_push(scheduler, job);
    if (*reinterpret_cast<const volatile std::uint8_t*>(scheduler + et::job_scheduler_immediate))
        return reinterpret_cast<Execute>(base + et::job_execute)(scheduler + et::job_scheduler_inline, job);
    reinterpret_cast<LockfreePush>(base + et::job_lockfree_push)(scheduler + et::job_scheduler_queue, job, job + et::job_link);
    const auto mask = *reinterpret_cast<const volatile std::uint32_t*>(scheduler + et::job_scheduler_waiters);
    if (!mask) return 0;
    for (auto bits = mask; bits; bits &= bits - 1) {
        auto* state = state_of(static_cast<unsigned>(std::countr_zero(bits)));
        if (*state == entering_idle && InterlockedCompareExchange(state, running, entering_idle) == entering_idle) {
            bump(cheap_local, cheap_wakes);
            return 0;
        }
    }
    const auto wake = reinterpret_cast<Wake>(base + et::job_wake);
    for (auto bits = mask; bits; bits &= bits - 1)
        if (wake(std::countr_zero(bits))) {
            bump(full_local, full_wakes);
            return 0;
        }
    return 0;
}

// ---- 4. Main loop sleep ----------------------------------------------------------------------
using PreciseSleep = void (*)(std::uintptr_t timer, std::int64_t nanoseconds);
PreciseSleep original_sleep{};
std::atomic<bool> precise{};
constexpr std::int64_t sleep_slack_ns = 250'000;
std::atomic<std::uint64_t> sleeps{}, sleep_late_ns{}, sleep_worst_ns{};

// The engine's loop (abort flag checked every pass, thread priority 15 while waiting), on this
// thread's own high-resolution timer. The game's timer is waited on too: an abort fires it.
void sleep_hook(std::uintptr_t timer, std::int64_t nanoseconds) {
    if (!precise.load(std::memory_order_relaxed) || reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != base + et::main_sleep_return)
        return original_sleep(timer, nanoseconds);
    static thread_local const HANDLE own =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!own) return original_sleep(timer, nanoseconds);
    const auto deadline = static_cast<std::int64_t>(profiler::now_ns()) + nanoseconds;
    auto remaining = nanoseconds;
    if (remaining <= 0) return;
    auto* abort = reinterpret_cast<volatile LONG*>(timer + et::sleep_abort);
    const HANDLE game = *reinterpret_cast<const HANDLE*>(timer + et::sleep_timer_handle);
    const std::array handles{own, game};
    do {
        if (InterlockedExchange(abort, 0)) return;
        if (remaining > sleep_slack_ns) {
            LARGE_INTEGER due{};
            due.QuadPart = -((remaining - sleep_slack_ns) / 100);
            if (!due.QuadPart || !SetWaitableTimer(own, &due, 0, nullptr, nullptr, FALSE)) {
                _mm_pause();
            } else {
                const auto thread = GetCurrentThread();
                const auto priority = GetThreadPriority(thread);
                SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
                const auto result = WaitForMultipleObjects(game ? 2 : 1, handles.data(), FALSE, INFINITE);
                SetThreadPriority(thread, priority);
                if (result == WAIT_FAILED) {
                    const auto left = deadline - static_cast<std::int64_t>(profiler::now_ns());
                    if (left > 0) original_sleep(timer, left);
                    return;
                }
            }
        } else {
            _mm_pause();
            _mm_pause();
        }
        remaining = deadline - static_cast<std::int64_t>(profiler::now_ns());
    } while (remaining > 0);
    const auto late = static_cast<std::uint64_t>(-remaining);
    sleeps.fetch_add(1, std::memory_order_relaxed);
    sleep_late_ns.fetch_add(late, std::memory_order_relaxed);
    if (late > sleep_worst_ns.load(std::memory_order_relaxed)) sleep_worst_ns.store(late, std::memory_order_relaxed);
}

// ---- 5. Section dirty-bit clear --------------------------------------------------------------
using DirtyClear = void (*)(std::uintptr_t section);
DirtyClear original_dirty{};
Count dirty_skipped, dirty_run;
thread_local std::uint32_t dirty_skipped_local{}, dirty_run_local{};

// The original takes the section's spinlock and writes the bits back unchanged when none are
// set; a bit set after this read is cleared next frame, as it would be after the original's.
void dirty_hook(std::uintptr_t section) {
    const bool empty = *reinterpret_cast<const volatile std::uint64_t*>(section + et::section_list_begin) ==
                       *reinterpret_cast<const volatile std::uint64_t*>(section + et::section_list_end);
    const bool dirty = empty
        ? (*reinterpret_cast<const volatile std::uint32_t*>(section + et::section_dirty) & et::section_dirty_mask) != 0
        : (*reinterpret_cast<const volatile std::uint16_t*>(section + et::section_list_dirty) & et::section_list_dirty_mask) != 0;
    if (!dirty) {
        bump(dirty_skipped_local, dirty_skipped);
        return;
    }
    bump(dirty_run_local, dirty_run);
    original_dirty(section);
}
std::atomic<bool> dirty_on{};

// ---- hooks -----------------------------------------------------------------------------------
bool matches(const Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + contract.rva, bytes) && bytes == contract.bytes;
}
void* target(const Fingerprint& contract) noexcept { return reinterpret_cast<void*>(base + contract.rva); }
bool attach(const Fingerprint& contract, std::string& error) {
    const auto result = hook_enable(target(contract));
    if (result == HookOk) return true;
    error = std::format("the hook at {:#x} could not be attached ({})", 0x140000000 + contract.rva, hook_status_string(result));
    return false;
}
void detach(const Fingerprint& contract) {
    const auto result = hook_disable(target(contract));
    if (result != HookOk && result != HookDisabled)
        logging::log(logging::Level::warning, logging::Channel::runtime, "Engine tweaks: the hook at {:#x} could not be detached ({}).",
            0x140000000 + contract.rva, hook_status_string(result));
}
const char* name(Tweak tweak) noexcept {
    switch (tweak) {
    case Tweak::gi: return "GI throttle";
    case Tweak::mesh_tree: return "Mesh cull tree";
    case Tweak::job_wake: return "Job wake";
    case Tweak::main_sleep: return "Main loop sleep";
    case Tweak::dirty_skip: return "Section dirty skip";
    }
    return "?";
}
}

bool install(std::uintptr_t b) noexcept {
    if (installed.load()) return true;
    base = b;
    if (!base) return false;
    install_macos_wine_stall_guards();
    struct Hook { Tweak tweak; const Fingerprint* contract; void* detour; void** original; };
    const std::array hooks{
        Hook{Tweak::gi, &et::gi_update, reinterpret_cast<void*>(&gi_hook), reinterpret_cast<void**>(&original_gi)},
        Hook{Tweak::mesh_tree, &et::mesh_cull_schedule, reinterpret_cast<void*>(&cull_hook), reinterpret_cast<void**>(&original_cull)},
        Hook{Tweak::job_wake, &et::job_push, reinterpret_cast<void*>(&push_hook), reinterpret_cast<void**>(&original_push)},
        Hook{Tweak::job_wake, &et::job_register_idle, reinterpret_cast<void*>(&register_hook), reinterpret_cast<void**>(&original_register)},
        Hook{Tweak::main_sleep, &et::precise_sleep, reinterpret_cast<void*>(&sleep_hook), reinterpret_cast<void**>(&original_sleep)},
        Hook{Tweak::dirty_skip, &et::section_dirty_clear, reinterpret_cast<void*>(&dirty_hook), reinterpret_cast<void**>(&original_dirty)}};
    std::array<bool, tweak_count> ok{};
    ok.fill(true);
    for (const auto& hook : hooks)
        if (!matches(*hook.contract)) ok[slot(hook.tweak)] = false;
    std::string missing;
    for (const auto& hook : hooks) {
        if (!ok[slot(hook.tweak)]) continue;
        // Prepared only: nothing is patched until the tweak is turned on.
        if (hook_prepare(target(*hook.contract), hook.detour, hook.original) != HookOk) ok[slot(hook.tweak)] = false;
    }
    for (std::size_t i = 0; i < tweak_count; ++i) {
        ready[i].store(ok[i]);
        if (!ok[i]) missing += std::string(missing.empty() ? "" : ", ") + name(static_cast<Tweak>(i));
    }
    installed.store(true);
    if (!missing.empty())
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Engine tweaks: this build's functions differ for {}; those stay stock.", missing);
    for (const auto tweak : {Tweak::job_wake, Tweak::main_sleep, Tweak::dirty_skip}) {
        const auto wanted = default_value(tweak);
        if (wanted <= 0) continue;
        std::string error;
        if (available(tweak) && !set(tweak, wanted, error))
            logging::log(logging::Level::warning, logging::Channel::runtime, "Engine tweaks: {} left off: {}", name(tweak), error);
    }
    return missing.empty();
}

double default_value(Tweak tweak) noexcept {
    if (tweak == Tweak::dirty_skip) return 1;
    if (under_macos_wine()) return tweak == Tweak::job_wake ? macos_wine_default_job_wake_us : 0;
    return tweak == Tweak::main_sleep ? 1 : 0;
}

bool available(Tweak tweak) noexcept { return installed.load() && ready[slot(tweak)].load(); }

double value(Tweak tweak) noexcept {
    switch (tweak) {
    case Tweak::gi: return gi_period.load() ? gi_rate : 0;
    case Tweak::mesh_tree: return tree_wanted.load() == 1 ? 1 : 0;
    case Tweak::job_wake: return wake_spin.load() ? wake_microseconds : 0;
    case Tweak::main_sleep: return precise.load() ? 1 : 0;
    case Tweak::dirty_skip: return dirty_on.load() ? 1 : 0;
    }
    return 0;
}

bool set(Tweak tweak, double wanted, std::string& error) {
    std::lock_guard lock(toggling);
    if (!available(tweak)) {
        error = std::string(name(tweak)) + " is not available for this build.";
        return false;
    }
    const bool on = wanted > 0;
    switch (tweak) {
    case Tweak::gi:
        if (!(wanted >= 0 && wanted <= 1000)) { error = "The rate is 0 (every frame) to 1000 updates a second."; return false; }
        if (!on) {
            gi_period.store(0);
            detach(et::gi_update);
            return true;
        }
        gi_ran.store(0); gi_skipped.store(0); gi_loop.store(0);
        gi_next.store(0);
        gi_rate = wanted;
        gi_period.store(static_cast<std::uint64_t>(1e6 / wanted * ticks_per_microsecond()));
        if (!attach(et::gi_update, error)) { gi_period.store(0); return false; }
        return true;
    case Tweak::mesh_tree:
        // The hook stays attached once used, so that off can put the game's value back.
        if (on && !attach(et::mesh_cull_schedule, error)) return false;
        tree_wanted.store(on ? 1 : -1);
        return true;
    case Tweak::job_wake:
        if (!(wanted >= 0 && wanted <= 1000)) { error = "The wait is 0 (stock) to 1000 microseconds."; return false; }
        if (!on) {
            wake_spin.store(0);
            detach(et::job_register_idle);
            detach(et::job_push);
            return true;
        }
        spins.reset(); spins_woken.reset(); cheap_wakes.reset(); full_wakes.reset();
        wake_microseconds = wanted;
        if (!attach(et::job_push, error)) return false;
        if (!attach(et::job_register_idle, error)) { detach(et::job_push); return false; }
        wake_spin.store(static_cast<std::uint64_t>(wanted * ticks_per_microsecond()));
        return true;
    case Tweak::main_sleep:
        if (!on) {
            precise.store(false);
            detach(et::precise_sleep);
            return true;
        }
        sleeps.store(0); sleep_late_ns.store(0); sleep_worst_ns.store(0);
        if (!attach(et::precise_sleep, error)) return false;
        precise.store(true);
        return true;
    case Tweak::dirty_skip:
        if (!on) {
            dirty_on.store(false);
            detach(et::section_dirty_clear);
            return true;
        }
        dirty_skipped.reset(); dirty_run.reset();
        if (!attach(et::section_dirty_clear, error)) return false;
        dirty_on.store(true);
        return true;
    }
    return false;
}

std::string status(Tweak tweak) {
    if (!available(tweak)) return std::string(name(tweak)) + " is not available for this build.";
    switch (tweak) {
    case Tweak::gi:
        if (!gi_period.load()) return "GI throttle off: Enlighten updates every rendered frame (stock).";
        return std::format("GI throttle {:.0f}/s: {} updates run, {} skipped; {} multi-iteration updates left alone.",
            gi_rate, gi_ran.load(), gi_skipped.load(), gi_loop.load());
    case Tweak::mesh_tree: {
        const int seen = tree_seen.load(), stock = tree_stock.load();
        return std::format("Mesh cull tree {}: the last frame culled {}{}.",
            tree_wanted.load() == 1 ? "on" : "off (the game's own setting)",
            seen < 0 ? "before ReSkate looked" : seen ? "with the octree" : "flat (every chunk against every view)",
            stock >= 0 ? std::format("; the game's value was {}", stock) : "");
    }
    case Tweak::job_wake:
        if (!wake_spin.load()) return "Job wake off: idle workers sleep at once and pushes wake in bit order (stock).";
        return std::format("Job wake {:.0f} us: {} idle waits, {} ended by a push without sleeping; "
            "pushes woke {} idle-entering workers by CAS and {} through the engine's wake.",
            wake_microseconds, spins.value(), spins_woken.value(), cheap_wakes.value(), full_wakes.value());
    case Tweak::main_sleep: {
        if (!precise.load()) return "Main loop sleep stock: coarse timer, then a 1 ms spin.";
        const auto count = sleeps.load();
        return std::format("Main loop sleep high-resolution (0.25 ms spin): {} sleeps, {:.0f} us late on average, worst {:.0f} us.",
            count, count ? sleep_late_ns.load() / 1000.0 / static_cast<double>(count) : 0.0, sleep_worst_ns.load() / 1000.0);
    }
    case Tweak::dirty_skip:
        if (!dirty_on.load()) return "Section dirty skip off: every section takes its spinlock every frame (stock).";
        return std::format("Section dirty skip on: {} lock-only calls skipped, {} with dirty bits run.", dirty_skipped.value(), dirty_run.value());
    }
    return {};
}
}
