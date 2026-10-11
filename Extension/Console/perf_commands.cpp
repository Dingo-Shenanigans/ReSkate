#include "Extension/Console/commands.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Settings/engine_tweaks.h"
#include "Extension/Settings/job_spin.h"
#include "Extension/UI/ui_pointer_skip.h"
#include "Engine/Core/Hooks/hooks.h"
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <vector>

// The performance profiler's console commands (Engine/Core/Profiling/profiler.h). Everything but
// the saved HUD switch runs on the overlay's thread: the profiler is thread-safe.
namespace dingosdk::console {
namespace {
std::string narrow(const std::filesystem::path& path) {
    std::string result;
    for (const auto c : path.wstring()) result.push_back(c < 128 ? static_cast<char>(c) : '?');
    return result;
}
void print_summary(const Output& out) {
    if (!profiler::active()) {
        out("The profiler is off: `perf hud on` or `perf window on` starts it (figures follow a second later).");
        return;
    }
    const auto s = profiler::summary();
    if (!s || s->seconds <= 0) { out("No figures yet; try again in a second."); return; }
    out(std::format("Client update: {:.1f}/s, every {:.1f} ms (longest {:.1f}); client frame {:.2f} ms (longest {:.1f}); "
        "ReSkate tick {:.2f} ms (longest {:.1f}) + hooks {:.2f} ms per frame",
        s->client_updates_per_second, s->client_interval_average_ms, s->client_interval_longest_ms,
        s->client_frame_average_ms, s->client_frame_longest_ms, s->client_own_average_ms,
        s->client_own_longest_ms, s->hooks_milliseconds_per_update));
    out(std::format("Frames: {:.1f}/s, {:.2f} ms (longest {:.1f}); process CPU {:.0f}% of {} cores",
        s->presents_per_second, s->present_interval_average_ms, s->present_interval_longest_ms,
        s->process_cpu_percent, s->cores));
    out("ReSkate zones (busiest first):");
    std::size_t shown{};
    for (const auto& zone : s->zones) {
        if (shown++ == 12) break;
        out(std::format("  {:<48} {:7.2f} ms/s  {:7.0f}/s  {:8.1f} us avg  {:6.2f} ms longest", zone.name,
            zone.milliseconds_per_second, zone.calls_per_second, zone.average_microseconds, zone.longest_milliseconds));
    }
    out("Threads (100% = one core):");
    shown = 0;
    for (const auto& thread : s->threads) {
        if (shown++ == 10) break;
        out(std::format("  {:>6} {:<26} {:5.1f}%", thread.id, thread.label, thread.cpu_percent));
    }
}
void print_report(const Output& out) {
    const auto r = profiler::sample_report();
    if (!r || (!r->running && !r->samples && r->error.empty())) { out("No sample yet: `perf sample` takes one."); return; }
    if (r->running) { out(std::format("Sampling {}: {:.0f}%, {} samples so far.", r->target, r->progress * 100, r->samples)); return; }
    if (!r->error.empty()) out("error: " + r->error);
    if (!r->samples) return;
    out(std::format("{} (thread {}): {} samples over {:.1f} s, busy {:.1f}%.", r->target, r->thread_id, r->samples,
        r->seconds, r->busy_percent));
    for (const auto& thread : r->threads)
        out(std::format("  {:5.1f}%  thread {} {}", thread.cpu_percent, thread.id, thread.label));
    out("  Self  Total  Function");
    std::size_t shown{};
    for (const auto& f : r->functions) {
        if (shown++ == 20) break;
        out(std::format("{:5.1f}% {:5.1f}%  {}", 100.0 * f.self / r->samples, 100.0 * f.total / r->samples, f.name));
    }
    out("Report: " + narrow(r->report));
    out("Flame graph (open at speedscope.app): " + narrow(r->folded));
}
}

// `perf memory`: what the game's memory is made of, by allocation, and what was added since a
// mark: for finding what a full server's memory goes on. An allocation is one VirtualAlloc
// reservation (the engine's arenas and pools are each one or a few); its size here is what of
// it is committed, which is what counts against the PC's memory.
struct MemoryMap {
    std::map<std::uintptr_t, std::uint64_t> allocations; // private memory: base -> committed bytes
    std::uint64_t committed{}, mapped{}, image{};
};
MemoryMap read_memory_map() {
    MemoryMap map;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto at = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (at < end && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info)) {
        if (info.State == MEM_COMMIT) {
            if (info.Type == MEM_PRIVATE) {
                map.allocations[reinterpret_cast<std::uintptr_t>(info.AllocationBase)] += info.RegionSize;
                map.committed += info.RegionSize;
            } else (info.Type == MEM_IMAGE ? map.image : map.mapped) += info.RegionSize;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return map;
}
std::string megabytes(std::uint64_t bytes) {
    return bytes >= (1ull << 30) ? std::format("{:.2f} GB", static_cast<double>(bytes) / (1ull << 30))
                                 : std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
}
// How much of [base, base + bytes) is in RAM now: the pages the game has written to and
// Windows has not moved out. The rest of a committed block is promised and unused.
std::uint64_t resident_bytes(std::uintptr_t base, std::uint64_t bytes) {
    constexpr std::size_t batch = 16384;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(batch);
    std::uint64_t resident{};
    for (std::uint64_t done = 0; done < bytes;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch, (bytes - done + 4095) / 4096));
        for (std::size_t i = 0; i < count; ++i) pages[i].VirtualAddress = reinterpret_cast<void*>(base + done + i * 4096);
        if (!QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(count * sizeof(pages[0])))) break;
        for (std::size_t i = 0; i < count; ++i) resident += pages[i].VirtualAttributes.Valid ? 4096 : 0;
        done += count * 4096ull;
    }
    return resident;
}
// Allocations by size, the sizes that add up to most first: "126 x 32.0 MB = 3.94 GB".
void print_by_size(const std::vector<std::uint64_t>& sizes, const Output& out, std::size_t most) {
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> groups; // size rounded to 64 KB -> count, bytes
    for (const auto size : sizes) {
        auto& group = groups[(size + 0xffff) & ~0xffffull];
        ++group.first;
        group.second += size;
    }
    std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>> order(groups.begin(), groups.end());
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    for (std::size_t i = 0; i < order.size() && i < most; ++i)
        out(std::format("  {} x {} = {}", order[i].second.first, megabytes(order[i].first), megabytes(order[i].second.second)));
}

// `perf memory trace`: who asks Windows for the middle-sized blocks (2 to 64 MB) while it is on,
// by the code that called for each: where a block of a size `perf memory` showed many of comes
// from. Every commit of memory in the process passes the hook while it is on, so it is a
// research aid and off until asked for.
namespace trace {
constexpr std::size_t callers = 8192, depth = 14;
struct Caller {
    std::uint64_t hash{}, count{}, bytes{}, size{}, largest{}; // size: the smallest such block; largest: the largest
    std::array<void*, depth> frames{};
    unsigned frame_count{};
};
std::mutex mutex;
std::vector<Caller> table(callers); // found by hash: a commit of any size can be traced
std::atomic<bool> on{};
// The sizes traced: 2 to 64 MB from the console; the largest blocks alone from the start of
// the game (RESKATE_MEMORY_TRACE), where nearly everything is being allocated.
std::atomic<std::uint64_t> least{2ull << 20}, most{64ull << 20};
bool prepared{};
using Allocate = LONG (NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
Allocate original{};

// The newer call for the same (VirtualAlloc2, which graphics drivers and the D3D12 runtime use).
using AllocateEx = LONG (NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, void*, ULONG);
AllocateEx original_ex{};
void note(HANDLE process, SIZE_T asked, ULONG type) noexcept;

LONG NTAPI hooked(HANDLE process, PVOID* base, ULONG_PTR zero_bits, PSIZE_T size, ULONG type, ULONG protect) {
    const auto asked = size ? *size : 0;
    const auto status = original(process, base, zero_bits, size, type, protect);
    if (status >= 0) note(process, asked, type);
    return status;
}
LONG NTAPI hooked_ex(HANDLE process, PVOID* base, PSIZE_T size, ULONG type, ULONG protect, void* parameters, ULONG parameter_count) {
    const auto asked = size ? *size : 0;
    const auto status = original_ex(process, base, size, type, protect, parameters, parameter_count);
    if (status >= 0) note(process, asked, type);
    return status;
}
void note(HANDLE process, SIZE_T asked, ULONG type) noexcept {
    if (!on.load(std::memory_order_relaxed) || !(type & (MEM_COMMIT | MEM_RESERVE)) || process != GetCurrentProcess() ||
        asked < least.load(std::memory_order_relaxed) || asked > most.load(std::memory_order_relaxed)) return;
    // (Reservations count too: a block reserved whole and committed a piece at a time is only
    // this large when it is reserved.)
    Caller seen;
    seen.frame_count = CaptureStackBackTrace(2, static_cast<DWORD>(depth), seen.frames.data(), nullptr);
    // By who asked alone: one caller's blocks of every size add up together.
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < seen.frame_count; ++i) hash = hash * 1099511628211ull ^ reinterpret_cast<std::uintptr_t>(seen.frames[i]);
    if (!hash) hash = 1;
    std::lock_guard lock(mutex);
    for (std::size_t probe = 0; probe < 128; ++probe) {
        auto& caller = table[(hash + probe) % callers];
        if (caller.count && caller.hash != hash) continue;
        if (!caller.count) { caller = seen; caller.hash = hash; caller.size = asked; }
        ++caller.count;
        caller.bytes += asked;
        caller.size = std::min<std::uint64_t>(caller.size, asked);
        caller.largest = std::max<std::uint64_t>(caller.largest, asked);
        break;
    }
}
bool start(std::string& error) {
    if (!prepared) {
        const auto target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));
        if (!target || hook_prepare(target, reinterpret_cast<void*>(&hooked), reinterpret_cast<void**>(&original)) != HookOk) {
            error = "the allocation hook could not be prepared";
            return false;
        }
        // (Missing on an older Windows: then only the first is traced.)
        if (const auto ex = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemoryEx"));
            ex && hook_prepare(ex, reinterpret_cast<void*>(&hooked_ex), reinterpret_cast<void**>(&original_ex)) != HookOk)
            original_ex = nullptr;
        prepared = true;
    }
    { std::lock_guard lock(mutex); std::fill(table.begin(), table.end(), Caller{}); }
    const auto target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));
    if (!on.load() && hook_enable(target) != HookOk) { error = "the allocation hook could not be attached"; return false; }
    if (!on.load() && original_ex) hook_enable(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemoryEx")));
    on.store(true);
    return true;
}
void stop() {
    if (!on.exchange(false)) return;
    hook_disable(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory")));
    if (original_ex) hook_disable(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemoryEx")));
}
std::string place(void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module)
        return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(address));
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text += static_cast<char>(*name);
    return std::format("{}+{:#x}", text, reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
}
void report(const Output& out) {
    std::vector<Caller> seen;
    {
        std::lock_guard lock(mutex);
        for (const auto& caller : table) if (caller.count) seen.push_back(caller);
    }
    std::sort(seen.begin(), seen.end(), [](const Caller& a, const Caller& b) { return a.bytes > b.bytes; });
    out(std::format("Memory asked for while tracing (blocks of {} and up), by who asked ({} callers), the most first:",
        megabytes(least.load()), seen.size()));
    for (std::size_t i = 0; i < seen.size() && i < 16; ++i) {
        out(seen[i].size == seen[i].largest
                ? std::format("  {} x {} bytes = {}", seen[i].count, seen[i].size, megabytes(seen[i].bytes))
                : std::format("  {} blocks of {} to {} bytes = {}", seen[i].count, seen[i].size, seen[i].largest, megabytes(seen[i].bytes)));
        std::string line = "     ";
        for (unsigned frame = 0; frame < seen[i].frame_count; ++frame) line += " < " + place(seen[i].frames[frame]);
        out(line);
    }
}
} // namespace trace

// RESKATE_MEMORY_TRACE=<megabytes>: from the start of the game, who asks for each block of at
// least that size (`perf memory` then lists them). A research aid for what a map's load commits.
void start_memory_trace_from_environment() noexcept {
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(L"RESKATE_MEMORY_TRACE", value, 16);
    if (!length || length >= 16) return;
    // (In megabytes; "all" for every block, however small.)
    const bool all = value[0] == L'a';
    const auto megabytes_least = std::wcstoul(value, nullptr, 10);
    if (!megabytes_least && !all) return;
    try {
        trace::least.store(all ? 1 : static_cast<std::uint64_t>(megabytes_least) << 20);
        trace::most.store(UINT64_MAX);
        std::string error;
        (void)trace::start(error);
    } catch (...) {}
}

void register_perf_commands(Commands& registry) {
    // The HUD starts off every launch; these turn it on for the session.
    auto toggle = action("perf", "Show or hide the performance HUD", Group::console);
    toggle.execution = Execution::local;
    toggle.run = [](const Model&, const Values&, const Output& out) {
        const bool on = !profiler::hud();
        profiler::set_hud(on);
        out(on ? "Performance HUD on. `perf window on` opens the profiler; `perf sample` takes a stack sample."
               : "Performance HUD off.");
    };
    registry.add(std::move(toggle));

    auto hud = variable("perf hud", "Performance HUD: client update, frame time, ReSkate's cost, CPU per thread",
        Group::console, argument("on|off", Type::boolean));
    hud.execution = Execution::local;
    hud.inspect = [](const Model&) { return boolean_state(true, profiler::hud()); };
    hud.run = [](const Model&, const Values& args, const Output& out) {
        const bool on = std::get<bool>(args[0]);
        profiler::set_hud(on);
        out(on ? "Performance HUD on." : "Performance HUD off.");
    };
    registry.add(std::move(hud));

    auto window = variable("perf window", "The profiler window (shown while the menu or console is open)",
        Group::console, argument("on|off", Type::boolean));
    window.execution = Execution::local;
    window.inspect = [](const Model&) { return boolean_state(true, profiler::window()); };
    window.run = [](const Model&, const Values& args, const Output& out) {
        profiler::set_window(std::get<bool>(args[0]));
        out(profiler::window() ? "Profiler window on: it shows while the menu or console is open." : "Profiler window off.");
    };
    registry.add(std::move(window));

    auto seconds = argument("seconds", Type::number, true);
    seconds.minimum = 0.5;
    seconds.maximum = 60;
    auto target = argument("client|present|all|<thread id>", Type::text, true);
    target.complete = [](const Model&, auto) { return std::vector<std::string>{"client", "present", "all"}; };
    auto rate = argument("rate", Type::unsigned_integer, true);
    rate.minimum = 50;
    rate.maximum = 4000;
    auto sample = action("perf sample", "Sample stacks: the client update thread (default, 10 s at 1000 Hz), present, all busy threads, or a thread id",
        Group::console, {seconds, target, rate});
    sample.execution = Execution::local;
    sample.run = [](const Model&, const Values& args, const Output& out) {
        profiler::SampleRequest request;
        if (args.size() > 0) request.seconds = std::get<double>(args[0]);
        if (args.size() > 1) {
            const auto& which = std::get<std::string>(args[1]);
            if (equal(which, "client")) request.target = profiler::Target::client;
            else if (equal(which, "present")) request.target = profiler::Target::present;
            else if (equal(which, "all")) { request.target = profiler::Target::all; request.rate = 250; }
            else {
                request.target = profiler::Target::thread;
                try { request.thread_id = static_cast<std::uint32_t>(std::stoul(which, nullptr, 0)); }
                catch (...) { out("error: The target is client, present, all or a thread id."); return; }
            }
        }
        if (args.size() > 2) request.rate = static_cast<unsigned>(std::get<std::uint64_t>(args[2]));
        std::string error;
        if (!profiler::start_sampling(request, error)) { out("error: " + error); return; }
        out(std::format("Sampling for {:.1f} s at {} Hz. `perf report` shows the result; the files go next to ReSkate.log.",
            request.seconds, request.rate));
    };
    registry.add(std::move(sample));

    auto stop = action("perf stop", "Stop the running sample early (it still reports)", Group::console);
    stop.execution = Execution::local;
    stop.run = [](const Model&, const Values&, const Output& out) {
        profiler::stop_sampling();
        out("Stopping the sample.");
    };
    registry.add(std::move(stop));

    auto report = action("perf report", "The last sample's busiest functions and its report files", Group::console);
    report.execution = Execution::local;
    report.run = [](const Model&, const Values&, const Output& out) { print_report(out); };
    registry.add(std::move(report));

    auto microseconds = argument("microseconds", Type::number);
    microseconds.minimum = 0;
    microseconds.maximum = 5000;
    auto spin = variable("perf jobspin", "How long idle engine job threads poll for work before sleeping (ReSkate 50 us, stock 250 us)",
        Group::engine, microseconds);
    spin.execution = Execution::local;
    spin.inspect = [](const Model&) {
        const auto value = job_spin::microseconds();
        return State{value.has_value(), value ? std::optional<std::string>(std::format("{:.0f}", *value)) : std::nullopt,
                     "The job system's spin setting is unavailable for this build.",
                     std::format("ReSkate default {:.0f} us; stock {:.0f} us", job_spin::default_microseconds,
                         job_spin::stock_microseconds()),
                     value && *value != job_spin::default_microseconds};
    };
    spin.run = [](const Model&, const Values& args, const Output& out) {
        const double wanted = std::get<double>(args[0]);
        if (!job_spin::set_microseconds(wanted)) { out("error: The job system's spin setting is unavailable for this build."); return; }
        out(std::format("Job threads now poll for {:.0f} us before sleeping (ReSkate default {:.0f}, stock {:.0f}; not saved).",
            wanted, job_spin::default_microseconds, job_spin::stock_microseconds()));
    };
    spin.reset = [](const Model&, const Output& out) {
        if (!job_spin::set_microseconds(job_spin::default_microseconds)) { out("error: The job system's spin setting is unavailable."); return; }
        out(std::format("Job threads poll for {:.0f} us again.", job_spin::default_microseconds));
    };
    registry.add(std::move(spin));

    auto pointer = variable("perf uipointer", "Skip the UI's per-frame mouse hit-test while the game shows no cursor (on by default)",
        Group::engine, argument("on|off", Type::boolean));
    pointer.execution = Execution::local;
    pointer.inspect = [](const Model&) { return boolean_state(true, ui_pointer::skip(), {}, ui_pointer::status()); };
    pointer.run = [](const Model&, const Values& args, const Output& out) {
        ui_pointer::set_skip(std::get<bool>(args[0]));
        out(ui_pointer::status());
    };
    registry.add(std::move(pointer));

    // Engine tweaks (Extension/Settings/engine_tweaks.h): mainsleep and dirtyskip start on, the
    // others off; nothing is saved. Measure each against stock with `perf sample 8 all`.
    const auto tweak_variable = [&](std::string name, std::string description, engine_tweaks::Tweak tweak, Argument value) {
        auto entry = variable(std::move(name), std::move(description), Group::engine, std::move(value));
        entry.execution = Execution::local;
        const bool numeric = tweak == engine_tweaks::Tweak::gi || tweak == engine_tweaks::Tweak::job_wake;
        entry.inspect = [tweak, numeric](const Model&) {
            const auto now = engine_tweaks::value(tweak);
            const bool changed = now != engine_tweaks::default_value(tweak);
            if (!numeric) {
                auto state = boolean_state(engine_tweaks::available(tweak), now > 0, engine_tweaks::status(tweak), engine_tweaks::status(tweak));
                state.overridden = changed;
                return state;
            }
            return State{engine_tweaks::available(tweak), std::format("{:.0f}", now), engine_tweaks::status(tweak),
                         engine_tweaks::status(tweak), changed};
        };
        entry.run = [tweak, numeric](const Model&, const Values& args, const Output& out) {
            const double wanted = numeric ? std::get<double>(args[0]) : (std::get<bool>(args[0]) ? 1.0 : 0.0);
            std::string error;
            if (!engine_tweaks::set(tweak, wanted, error)) { out("error: " + error); return; }
            out(engine_tweaks::status(tweak));
        };
        entry.reset = [tweak](const Model&, const Output& out) {
            std::string error;
            if (!engine_tweaks::set(tweak, engine_tweaks::default_value(tweak), error)) { out("error: " + error); return; }
            out(engine_tweaks::status(tweak));
        };
        registry.add(std::move(entry));
    };
    auto updates = argument("updates per second", Type::number);
    updates.minimum = 0;
    updates.maximum = 1000;
    tweak_variable("perf gi", "Throttle Enlighten GI's per-frame update to this rate (0 = every frame, stock; try 60)",
        engine_tweaks::Tweak::gi, updates);
    tweak_variable("perf meshtree", "Cull meshes through the octree the game ships off (MeshCullTreeEnabled)",
        engine_tweaks::Tweak::mesh_tree, argument("on|off", Type::boolean));
    auto wait = argument("microseconds", Type::number);
    wait.minimum = 0;
    wait.maximum = 1000;
    tweak_variable("perf jobwake", "Idle job workers wait this long where a push wakes them cheaply, and pushes prefer them (0 = stock; measured to cost more CPU than it saves)",
        engine_tweaks::Tweak::job_wake, wait);
    tweak_variable("perf mainsleep", "Pace the main loop on a high-resolution timer with a 0.25 ms spin instead of a 1 ms one (on by default)",
        engine_tweaks::Tweak::main_sleep, argument("on|off", Type::boolean));
    tweak_variable("perf dirtyskip", "Skip the per-section spinlock that clears dirty bits when none are set (on by default)",
        engine_tweaks::Tweak::dirty_skip, argument("on|off", Type::boolean));
    auto tweaks = action("perf tweaks", "What each engine tweak is doing (perf gi, meshtree, jobwake, mainsleep, dirtyskip)", Group::console);
    tweaks.execution = Execution::local;
    tweaks.run = [](const Model&, const Values&, const Output& out) {
        for (const auto tweak : {engine_tweaks::Tweak::gi, engine_tweaks::Tweak::mesh_tree, engine_tweaks::Tweak::job_wake,
                 engine_tweaks::Tweak::main_sleep, engine_tweaks::Tweak::dirty_skip})
            out(engine_tweaks::status(tweak));
    };
    registry.add(std::move(tweaks));

    auto what = argument("mark", Type::text, true);
    what.complete = [](const Model&, auto) { return std::vector<std::string>{"mark", "trace", "stop", "big"}; };
    auto memory = action("perf memory", "The game's memory by allocation; `perf memory mark` remembers it and the next `perf memory` says what was added since; "
        "`perf memory trace` then also says who asked for the middle-sized blocks, until `perf memory stop`; `perf memory big` lists the largest blocks and how much of each is in use",
        Group::console, {std::move(what)});
    memory.execution = Execution::local;
    memory.run = [](const Model&, const Values& args, const Output& out) {
        static std::mutex mutex;
        static MemoryMap marked;
        static bool has_mark{};
        std::lock_guard lock(mutex);
        const auto now = read_memory_map();
        out(std::format("Memory: {} committed in {} allocations, {} of files mapped in, {} of programs.", megabytes(now.committed),
            now.allocations.size(), megabytes(now.mapped), megabytes(now.image)));
        if (!args.empty() && equal(std::get<std::string>(args[0]), "big")) {
            // The largest blocks one by one: where each is, its exact size, and how much of it is used.
            std::vector<std::pair<std::uint64_t, std::uintptr_t>> order;
            for (const auto& [base, size] : now.allocations) order.emplace_back(size, base);
            std::sort(order.rbegin(), order.rend());
            out("The largest blocks (committed, of which in RAM now):");
            for (std::size_t i = 0; i < order.size() && i < 24; ++i) {
                // (The whole reservation is walked: what is not committed in it is not in RAM either.)
                MEMORY_BASIC_INFORMATION info{};
                std::uint64_t span = order[i].first;
                for (auto at = order[i].second; VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info) &&
                     reinterpret_cast<std::uintptr_t>(info.AllocationBase) == order[i].second;
                     at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
                    span = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize - order[i].second;
                out(std::format("  {:#x}: {} bytes ({}), {} in RAM", order[i].second, order[i].first, megabytes(order[i].first),
                    megabytes(resident_bytes(order[i].second, span))));
                if (i >= 3) continue;
                // What kind of memory the three largest are, and a look at what is in them: how
                // they are protected and held, and the bytes at a few places that are in RAM.
                VirtualQuery(reinterpret_cast<void*>(order[i].second), &info, sizeof(info));
                PSAPI_WORKING_SET_EX_INFORMATION page{reinterpret_cast<void*>(order[i].second)};
                QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page));
                out(std::format("      reserved {:#x} bytes, protection {:#x} (as reserved {:#x}), first region {:#x} bytes; first page: {}{}{}{}",
                    span, info.Protect, info.AllocationProtect, static_cast<std::uint64_t>(info.RegionSize),
                    page.VirtualAttributes.Valid ? "in RAM" : "not in RAM", page.VirtualAttributes.Shared ? ", shared" : "",
                    page.VirtualAttributes.Locked ? ", locked" : "", page.VirtualAttributes.LargePage ? ", large pages" : ""));
                std::size_t looked{}, zero_pages{};
                for (std::uint64_t part = 0; part < 64; ++part) {
                    const auto at = order[i].second + ((span / 64) * part & ~0xfffull);
                    PSAPI_WORKING_SET_EX_INFORMATION here{reinterpret_cast<void*>(at)};
                    if (!QueryWorkingSetEx(GetCurrentProcess(), &here, sizeof(here)) || !here.VirtualAttributes.Valid) continue;
                    std::array<unsigned char, 4096> bytes{};
                    SIZE_T got{};
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), bytes.data(), bytes.size(), &got) || got != bytes.size()) continue;
                    ++looked;
                    const bool zero = std::all_of(bytes.begin(), bytes.end(), [](unsigned char byte) { return byte == 0; });
                    zero_pages += zero;
                    if (zero || looked - zero_pages > 4) continue;
                    std::string hex;
                    for (std::size_t byte = 0; byte < 48; ++byte) hex += std::format("{:02x}{}", bytes[byte], byte % 8 == 7 ? " " : "");
                    out(std::format("      +{:#x}: {}", at - order[i].second, hex));
                }
                out(std::format("      of {} sampled pages in RAM, {} are all zero", looked, zero_pages));
            }
            return;
        }
        const bool tracing = !args.empty() && equal(std::get<std::string>(args[0]), "trace");
        if (!args.empty() && equal(std::get<std::string>(args[0]), "stop")) {
            trace::stop();
            out("Tracing stopped.");
            return;
        }
        if (tracing) {
            std::string error;
            // Every size: a large block is often set aside whole and filled a page at a time.
            trace::least.store(1);
            trace::most.store(UINT64_MAX);
            if (!trace::start(error)) { out("error: " + error); return; }
        }
        if (tracing || (!args.empty() && equal(std::get<std::string>(args[0]), "mark"))) {
            marked = now;
            has_mark = true;
            out(tracing ? "Marked, and tracing who asks for memory. Change what you want to measure, then run `perf memory`; `perf memory stop` ends the tracing."
                        : "Marked. Change what you want to measure, then run `perf memory`.");
            return;
        }
        if (!has_mark) {
            std::vector<std::uint64_t> sizes;
            for (const auto& [base, size] : now.allocations) sizes.push_back(size);
            out("The allocation sizes that add up to most:");
            print_by_size(sizes, out, 20);
            if (trace::on.load()) trace::report(out);
            return;
        }
        std::vector<std::uint64_t> added, grown;
        std::uint64_t added_bytes{}, grown_bytes{}, freed_bytes{};
        for (const auto& [base, size] : now.allocations) {
            const auto before = marked.allocations.find(base);
            if (before == marked.allocations.end()) added.push_back(size), added_bytes += size;
            else if (size > before->second) grown.push_back(size - before->second), grown_bytes += size - before->second;
            else freed_bytes += before->second - size;
        }
        for (const auto& [base, size] : marked.allocations)
            if (!now.allocations.contains(base)) freed_bytes += size;
        out(std::format("Since the mark: {} to {} ({}{}).", megabytes(marked.committed), megabytes(now.committed),
            now.committed >= marked.committed ? "+" : "-",
            megabytes(now.committed >= marked.committed ? now.committed - marked.committed : marked.committed - now.committed)));
        out(std::format("New allocations: {} in {}. By size:", megabytes(added_bytes), added.size()));
        print_by_size(added, out, 20);
        out(std::format("Growth of allocations that were there: {} in {}. By how much each grew:", megabytes(grown_bytes), grown.size()));
        print_by_size(grown, out, 12);
        out(std::format("Given back: {}.", megabytes(freed_bytes)));
        if (trace::on.load()) trace::report(out);
    };
    registry.add(std::move(memory));

    auto status = action("perf status", "Client update and frame timing, ReSkate's zones, CPU per thread", Group::console);
    status.execution = Execution::local;
    status.run = [](const Model&, const Values&, const Output& out) { print_summary(out); };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
