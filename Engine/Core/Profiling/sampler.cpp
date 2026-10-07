#include "profiler_internal.h"
#include "Engine/Core/Log/logging.h"
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <functional>

// The stack sampler. A sample suspends the target thread, takes its context and walks its stack,
// then resumes it. While the target is suspended this thread must not take any lock the target
// might hold -- the heap, the loader, DbgHelp -- or both stop for good. So the walk uses only
// memory prepared beforehand: each module's own unwind table (found by binary search, not
// RtlLookupFunctionEntry, which locks) and RtlVirtualUnwind, which only reads. Names are looked
// up after sampling ends.
namespace dingosdk::profiler {
namespace {
constexpr std::uint32_t max_depth = 96;
constexpr std::uintptr_t ghidra_image_base = 0x140000000;

struct Module {
    std::uintptr_t base{}, end{};
    const RUNTIME_FUNCTION* functions{};
    std::uint32_t count{};
    std::wstring path;
    std::string name;
    bool exe{};
};
struct Modules {
    std::vector<Module> list; // sorted by base
    const Module* find(std::uintptr_t pc) const noexcept {
        std::size_t lo = 0, hi = list.size();
        while (lo < hi) {
            const auto mid = (lo + hi) / 2;
            if (list[mid].end <= pc) lo = mid + 1; else hi = mid;
        }
        return lo < list.size() && list[lo].base <= pc && pc < list[lo].end ? &list[lo] : nullptr;
    }
};

std::string narrow(std::wstring_view text) {
    std::string result;
    for (const auto c : text) result.push_back(c < 128 ? static_cast<char>(c) : '?');
    return result;
}

Modules snapshot_modules() {
    Modules result;
    std::vector<HMODULE> handles(1024);
    DWORD needed{};
    if (!EnumProcessModulesEx(GetCurrentProcess(), handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
            &needed, LIST_MODULES_ALL)) return result;
    handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));
    const auto exe = GetModuleHandleW(nullptr);
    for (const auto handle : handles) {
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info))) continue;
        Module module;
        module.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        module.end = module.base + info.SizeOfImage;
        module.exe = handle == exe;
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(handle, path, MAX_PATH)) {
            module.path = path;
            const auto slash = module.path.find_last_of(L"\\/");
            module.name = narrow(slash == std::wstring::npos ? module.path : module.path.substr(slash + 1));
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (directory.VirtualAddress && directory.Size && directory.VirtualAddress + directory.Size <= info.SizeOfImage) {
                module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + directory.VirtualAddress);
                module.count = directory.Size / sizeof(RUNTIME_FUNCTION);
            }
        }
        result.list.push_back(std::move(module));
    }
    std::ranges::sort(result.list, {}, &Module::base);
    return result;
}

const RUNTIME_FUNCTION* lookup(const Module& module, std::uintptr_t pc) noexcept {
    if (!module.functions) return nullptr;
    const auto rva = static_cast<std::uint32_t>(pc - module.base);
    std::uint32_t lo = 0, hi = module.count;
    while (lo < hi) {
        const auto mid = (lo + hi) / 2;
        if (module.functions[mid].EndAddress <= rva) lo = mid + 1; else hi = mid;
    }
    return lo < module.count && module.functions[lo].BeginAddress <= rva ? &module.functions[lo] : nullptr;
}

// Unwinds a suspended thread's context into frames. No allocation, no locks; a fault on a
// corrupt stack just ends the walk.
std::uint32_t walk(const Modules& modules, CONTEXT& context, std::uint64_t* frames) noexcept {
    std::uint32_t depth = 0;
    __try {
        while (depth < max_depth && context.Rip) {
            const auto pc = context.Rip, sp = context.Rsp;
            frames[depth++] = pc;
            const auto* module = modules.find(pc);
            if (!module) break;
            // Past the first frame the PC is a return address, which can sit just after the end of
            // its function: look it up one byte back.
            const auto* entry = lookup(*module, depth > 1 ? pc - 1 : pc);
            if (!entry) {
                // A leaf function: no frame, the return address is on top of the stack.
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
            } else {
                void* handler_data{};
                DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, pc, const_cast<PRUNTIME_FUNCTION>(entry), &context,
                    &handler_data, &establisher, nullptr);
            }
            if (context.Rsp <= sp) break; // a stack only unwinds upwards
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return depth;
}

// The function a frame belongs to: its primary unwind entry (chained entries describe parts of
// the same function) -- the start Ghidra shows too.
std::uintptr_t function_start(const Modules& modules, std::uintptr_t pc, const Module** owner) noexcept {
    const auto* module = modules.find(pc);
    if (owner) *owner = module;
    if (!module) return pc;
    const auto* entry = lookup(*module, pc);
    for (int guard = 0; entry && guard < 32; ++guard) {
        const auto* info = reinterpret_cast<const std::uint8_t*>(module->base + entry->UnwindData);
        if (!(info[0] >> 3 & UNW_FLAG_CHAININFO)) break;
        const auto codes = info[2];
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
    }
    return entry ? module->base + entry->BeginAddress : pc;
}

// ---- names ----
std::mutex labels_mutex;
std::vector<Label> engine_labels;

std::unordered_map<std::uintptr_t, std::string> load_labels(const Modules& modules) {
    std::unordered_map<std::uintptr_t, std::string> result;
    {
        std::lock_guard lock(labels_mutex);
        for (const auto& label : engine_labels) result[label.rva] = label.name;
    }
    // ReSkate.labels.tsv next to Skate.exe: "<RVA or Ghidra address> <name>" per line, so
    // functions named while reading a profile show up by name in the next one.
    for (const auto& module : modules.list) {
        if (!module.exe) continue;
        const auto folder = std::filesystem::path(module.path).parent_path();
        std::ifstream file(folder / L"ReSkate.labels.tsv");
        std::string line;
        while (std::getline(file, line)) {
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            const auto gap = line.find_first_of(" \t", first);
            if (gap == std::string::npos) continue;
            const auto rest = line.find_first_not_of(" \t", gap);
            if (rest == std::string::npos) continue;
            std::uintptr_t address{};
            try { address = std::stoull(line.substr(first, gap - first), nullptr, 16); } catch (...) { continue; }
            if (address >= ghidra_image_base) address -= ghidra_image_base;
            auto name = line.substr(rest);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (!name.empty()) result[address] = name;
        }
    }
    return result;
}

// DbgHelp is single-threaded and the process may use it elsewhere (a crash handler): a private
// session under a handle of our own, used only from the sampler thread, under this lock.
std::mutex symbols_mutex;
const HANDLE symbol_session = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x5245534b)); // "RESK"
bool symbols_ready{};
std::unordered_set<std::uintptr_t> symbols_loaded;

const char waiting_marker{}; // an address inside this module, to tell it from the others
std::string symbol_name(const Module& module, std::uintptr_t address) {
    std::lock_guard lock(symbols_mutex);
    if (!symbols_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
        symbols_ready = SymInitializeW(symbol_session, nullptr, FALSE) != FALSE;
    }
    if (symbols_ready && symbols_loaded.insert(module.base).second)
        SymLoadModuleExW(symbol_session, nullptr, module.path.c_str(), nullptr, module.base,
            static_cast<DWORD>(module.end - module.base), nullptr, 0);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 511;
    DWORD64 displacement{};
    if (symbols_ready && SymFromAddr(symbol_session, address, &displacement, info) && info->Name[0]) {
        // A release ReSkate.dll ships without its symbols, so all this finds in it is the nearest
        // export, megabytes away: every function of ours then reads as one of three exports and
        // a report cannot say which was slow. Unless the address is inside a symbol with a size
        // (the real ones have one), give our own module's address instead: the release's symbols
        // (build/release/ReSkate-<version>-symbols) name it afterwards.
        const auto here = reinterpret_cast<std::uintptr_t>(&waiting_marker);
        const bool own = module.base <= here && here < module.end;
        if (!own || (info->Size && displacement < info->Size)) return module.name + "!" + info->Name;
    }
    return std::format("{}+0x{:x}", module.name, address - module.base);
}

bool waiting(std::string_view name) {
    constexpr std::string_view waits[] = {"WaitFor", "DelayExecution", "RemoveIoCompletion", "YieldExecution",
        "SignalAndWait", "WaitForAlertByThreadId", "WaitForWorkViaWorkerFactory", "ReplyWaitReceivePort"};
    if (!name.starts_with("ntdll.dll!") && !name.starts_with("win32u.dll!")) return false;
    return std::ranges::any_of(waits, [&](std::string_view wait) { return name.find(wait) != std::string_view::npos; });
}

// ---- state ----
std::mutex report_mutex;
std::shared_ptr<const SampleReport> latest_report = std::make_shared<SampleReport>();
std::atomic<bool> stop_requested{false};

void publish(SampleReport report) {
    std::lock_guard lock(report_mutex);
    latest_report = std::make_shared<SampleReport>(std::move(report));
}

std::string target_name(Target target, std::uint32_t id) {
    switch (target) {
    case Target::client: return "client update";
    case Target::present: return "present";
    case Target::all: return "all busy threads";
    default: return std::format("thread {}", id);
    }
}

std::string sanitize(std::string text) {
    for (auto& c : text) if (c == ';' || c == '\n' || c == '\r') c = ' ';
    return text;
}

struct Sampled { HANDLE handle{}; std::uint32_t id{}; std::string label; };
// One sample: where its frames start in the pool, how many, and which thread it came from.
struct Record { std::uint32_t offset{}; std::uint16_t thread{}; std::uint8_t depth{}; };
// The pool holds at most this many frames (64 MiB); a sample that no longer fits ends the run.
constexpr std::size_t pool_limit = 8u << 20;

void run(SampleRequest request, std::vector<Sampled> threads) {
    SetThreadDescription(GetCurrentThread(), L"ReSkate sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const bool all = request.target == Target::all;
    SampleReport report;
    report.running = true;
    report.target = target_name(request.target, threads.front().id);
    report.thread_id = all ? 0 : threads.front().id;
    report.rate = request.rate;
    const auto modules = snapshot_modules();
    const auto wanted = static_cast<std::size_t>(request.seconds * request.rate) * threads.size() + 16;
    const auto capacity = std::min<std::size_t>(wanted, pool_limit / 16);
    std::vector<std::uint64_t> pool;
    std::vector<Record> records;
    const auto close = [&] { for (const auto& t : threads) CloseHandle(t.handle); };
    try {
        pool.resize(std::min(pool_limit, capacity * 48 + max_depth));
        records.reserve(capacity);
    } catch (...) {
        report.running = false;
        report.error = "Not enough memory for the sample buffer.";
        publish(std::move(report));
        close();
        detail::sampling.store(false, std::memory_order_release);
        return;
    }
    publish(report);

    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto period = std::chrono::nanoseconds(1'000'000'000 / request.rate);
    const auto start = std::chrono::steady_clock::now();
    const auto finish = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(request.seconds));
    auto next = start;
    auto next_progress = start;
    std::size_t used = 0;
    std::vector<char> ended(threads.size());
    bool full = false;
    while (!full && !stop_requested.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= finish) break;
        bool any = false;
        for (std::size_t t = 0; t < threads.size(); ++t) {
            if (ended[t]) continue;
            if (records.size() == capacity || pool.size() - used < max_depth) { full = true; break; }
            // Nothing between Suspend and Resume may allocate or lock: the frames go straight
            // into the pool, and the record is added after the thread runs again.
            if (SuspendThread(threads[t].handle) == static_cast<DWORD>(-1)) { ended[t] = 1; continue; }
            any = true;
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            std::uint32_t depth = 0;
            if (GetThreadContext(threads[t].handle, &context)) depth = walk(modules, context, pool.data() + used);
            ResumeThread(threads[t].handle);
            if (!depth) continue;
            records.push_back({static_cast<std::uint32_t>(used), static_cast<std::uint16_t>(t), static_cast<std::uint8_t>(depth)});
            used += depth;
        }
        if (!any && !full) { report.error = "The thread has ended."; break; }
        if (now >= next_progress) {
            next_progress = now + std::chrono::milliseconds(250);
            report.progress = std::chrono::duration<double>(now - start).count() / request.seconds;
            report.samples = static_cast<std::uint32_t>(records.size());
            publish(report);
        }
        next += period;
        const auto wait = next - std::chrono::steady_clock::now();
        if (wait > std::chrono::microseconds(50)) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
            } else {
                Sleep(1);
            }
        } else if (wait < -period * 4) {
            next = std::chrono::steady_clock::now(); // fell behind: don't burst
        }
    }
    if (timer) CloseHandle(timer);
    close();
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report.progress = 1;

    // ---- aggregate: by function, and whole stacks for the flame graph ----
    try {
        struct Counts { std::uint32_t self{}, total{}; const Module* module{}; };
        std::unordered_map<std::uintptr_t, Counts> functions;
        std::vector<std::uintptr_t> stack;
        std::unordered_map<std::string, std::uint32_t> folded;
        const auto labels = load_labels(modules);
        std::unordered_map<std::uintptr_t, std::string> names;
        std::unordered_map<std::uintptr_t, const Module*> owners;
        const auto name_of = [&](std::uintptr_t key, const Module* module) -> const std::string& {
            auto it = names.find(key);
            if (it != names.end()) return it->second;
            std::string name;
            if (!module) name = std::format("0x{:x}", key);
            else if (module->exe) {
                const auto rva = key - module->base;
                const auto label = labels.find(rva);
                name = std::format("Skate 0x{:x}", ghidra_image_base + rva);
                if (label != labels.end()) name += " " + label->second;
            } else name = symbol_name(*module, key);
            return names.emplace(key, std::move(name)).first->second;
        };
        std::vector<std::uint32_t> busy_by_thread(threads.size());
        std::uint32_t idle{}, counted{};
        for (const auto& record : records) {
            const auto* sample = pool.data() + record.offset;
            stack.clear();
            for (std::uint32_t f = 0; f < record.depth; ++f) {
                const Module* module{};
                stack.push_back(function_start(modules, f ? sample[f] - 1 : sample[f], &module));
                owners[stack.back()] = module;
            }
            const bool waits = !stack.empty() && waiting(name_of(stack.front(), owners[stack.front()]));
            if (waits) ++idle; else ++busy_by_thread[record.thread];
            // Across threads, a profile of the work: waiting threads are not where CPU went.
            if (all && waits) continue;
            ++counted;
            for (std::size_t f = 0; f < stack.size(); ++f) {
                auto& counts = functions[stack[f]];
                counts.module = owners[stack[f]];
                if (!f) ++counts.self;
                const auto here = stack.begin() + static_cast<std::ptrdiff_t>(f);
                if (std::find(stack.begin(), here, stack[f]) == here) ++counts.total;
            }
            std::string line = all ? sanitize(std::format("thread {} {}", threads[record.thread].id, threads[record.thread].label))
                                   : std::string{};
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (!line.empty()) line += ';';
                line += sanitize(name_of(*it, owners[*it]));
            }
            ++folded[line];
        }
        const auto total = records.size();
        report.samples = counted;
        report.busy_percent = total ? 100.0 * static_cast<double>(total - idle) / static_cast<double>(total) : 0.0;
        if (all) {
            const auto busy_total = std::max<std::uint32_t>(1, counted);
            for (std::size_t t = 0; t < threads.size(); ++t)
                if (busy_by_thread[t])
                    report.threads.push_back({threads[t].id, threads[t].label, 100.0 * busy_by_thread[t] / busy_total});
            std::ranges::sort(report.threads, [](const ThreadRow& a, const ThreadRow& b) { return a.cpu_percent > b.cpu_percent; });
        }
        for (const auto& [key, counts] : functions)
            report.functions.push_back({name_of(key, counts.module),
                counts.module && counts.module->exe ? ghidra_image_base + (key - counts.module->base) : key,
                counts.self, counts.total});
        std::ranges::sort(report.functions, [](const SampledFunction& a, const SampledFunction& b) {
            return a.self != b.self ? a.self > b.self : a.total > b.total;
        });

        // ---- files next to ReSkate.log ----
        const auto directory = logging::status().directory;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto stem = std::format("profile-{:04}{:02}{:02}-{:02}{:02}{:02}-{}", now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, request.target == Target::client ? "client" :
            request.target == Target::present ? "present" : all ? "all" : std::to_string(threads.front().id));
        report.report = directory / (stem + ".txt");
        report.folded = directory / (stem + ".folded");
        const double per = static_cast<double>(std::max<std::uint32_t>(1, counted));
        {
            std::ofstream out(report.report);
            if (all) {
                out << std::format("ReSkate profile: all busy threads ({}), {:.1f} s, {} samples at {} Hz per thread\n",
                    threads.size(), report.seconds, total, request.rate);
                out << std::format("Busy {:.1f}% of samples; the {} busy ones are profiled below (waiting left out).\n",
                    report.busy_percent, counted);
                out << "\n  Share  Thread\n";
                for (const auto& t : report.threads) out << std::format("{:5.1f}%  {} {}\n", t.cpu_percent, t.id, t.label);
                out << "\n";
            } else {
                out << std::format("ReSkate profile: {} (thread {}), {:.1f} s, {} samples at {} Hz\n", report.target,
                    threads.front().id, report.seconds, total, request.rate);
                out << std::format("Busy {:.1f}% (the rest waiting in the kernel)\n", report.busy_percent);
            }
            out << "Skate.exe functions are Ghidra addresses (image base 0x140000000). Name them in\n"
                   "ReSkate.labels.tsv next to Skate.exe (\"<address> <name>\" per line) for the next report.\n"
                   "Flame graph: open the .folded file at https://www.speedscope.app\n\n";
            out << "  Self  Total  Function\n";
            for (const auto& f : report.functions) {
                if (f.total * 1000.0 < per) continue; // under 0.1%
                out << std::format("{:5.1f}% {:5.1f}%  {}\n", 100.0 * f.self / per, 100.0 * f.total / per, f.name);
            }
        }
        {
            std::ofstream out(report.folded);
            for (const auto& [line, n] : folded) out << line << ' ' << n << '\n';
        }
        if (report.functions.size() > 400) report.functions.resize(400);
        logging::log(logging::Level::info, logging::Channel::diagnostics, "Profiler: sampled {} for {:.1f} s ({} samples); report {}",
            report.target, report.seconds, total, narrow(report.report.wstring()));
    } catch (const std::exception& e) {
        report.error = std::string("Report failed: ") + e.what();
    }
    report.running = false;
    publish(std::move(report));
    detail::sampling.store(false, std::memory_order_release);
}

std::uint64_t cpu_time(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    const auto value = [](const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return value(kernel) + value(user);
}

// The process's threads that used 3% of a core or more over a quarter second (at most 48),
// other than ReSkate's own profiler threads.
std::vector<Sampled> busy_threads() {
    std::vector<Sampled> candidates;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    THREADENTRY32 entry{sizeof(entry)};
    const auto self = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self || entry.th32ThreadID == me) continue;
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
            entry.th32ThreadID);
        if (handle) candidates.push_back({handle, entry.th32ThreadID, {}});
    }
    CloseHandle(snapshot);
    std::vector<std::uint64_t> before(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) before[i] = cpu_time(candidates[i].handle);
    Sleep(250);
    std::vector<std::pair<std::uint64_t, std::size_t>> used;
    for (std::size_t i = 0; i < candidates.size(); ++i) used.push_back({cpu_time(candidates[i].handle) - before[i], i});
    std::ranges::sort(used, std::greater<>{});
    std::vector<Sampled> result;
    for (const auto& [time, i] : used) {
        auto& c = candidates[i];
        PWSTR description{};
        bool ours = false;
        if (SUCCEEDED(GetThreadDescription(c.handle, &description)) && description) {
            ours = std::wstring_view(description).starts_with(L"ReSkate ");
            LocalFree(description);
        }
        // 3% of a core over 250 ms, in 100 ns units.
        if (ours || time < 75'000 || result.size() == 48) { CloseHandle(c.handle); continue; }
        c.label = detail::thread_label(c.handle, c.id);
        result.push_back(std::move(c));
    }
    return result;
}
} // namespace

bool start_sampling(const SampleRequest& request, std::string& error) {
    if (detail::sampling.exchange(true, std::memory_order_acq_rel)) { error = "A sample is already running."; return false; }
    auto bounded = request;
    bounded.seconds = std::clamp(bounded.seconds, 0.5, 60.0);
    bounded.rate = std::clamp(bounded.rate, 50u, 4000u);
    stop_requested.store(false, std::memory_order_release);
    detail::wake_monitor();
    try {
        // Choosing the busy threads takes a quarter second: done on the sampler's own thread.
        std::thread([bounded] {
            std::vector<Sampled> threads;
            std::string failure;
            if (bounded.target == Target::all) {
                threads = busy_threads();
                if (threads.empty()) failure = "No busy threads found.";
            } else {
                const std::uint32_t id = bounded.target == Target::client ? detail::client_thread.load()
                    : bounded.target == Target::present ? detail::present_thread.load() : bounded.thread_id;
                const HANDLE handle = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, id) : nullptr;
                if (!id) failure = bounded.target == Target::thread ? "No thread id given." :
                    "That thread has not run yet; try again once the game is up.";
                else if (!handle) failure = std::format("Cannot open thread {} (error {}).", id, GetLastError());
                else threads.push_back({handle, id, detail::thread_label(handle, id)});
            }
            if (!failure.empty()) {
                SampleReport report;
                report.error = failure;
                publish(std::move(report));
                detail::sampling.store(false, std::memory_order_release);
                return;
            }
            run(bounded, std::move(threads));
        }).detach();
    } catch (...) {
        detail::sampling.store(false, std::memory_order_release);
        error = "Cannot start the sampler thread.";
        return false;
    }
    return true;
}

void stop_sampling() noexcept { stop_requested.store(true, std::memory_order_release); }

std::shared_ptr<const SampleReport> sample_report() noexcept {
    std::lock_guard lock(report_mutex);
    return latest_report;
}

void set_engine_labels(std::span<const Label> labels) noexcept {
    try {
        std::lock_guard lock(labels_mutex);
        engine_labels.assign(labels.begin(), labels.end());
    } catch (...) {}
}
}
