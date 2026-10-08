#include "profiler_internal.h"
#include <TlHelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <array>
#include <map>
#include <mutex>
#include <thread>

namespace dingosdk::profiler {
std::atomic<bool> zones_enabled{false};
namespace detail {
std::atomic<std::uint32_t> client_thread{0}, present_thread{0};
std::atomic<bool> sampling{false};
std::atomic<bool> present_log_on{false};
std::array<std::atomic<std::uint64_t>, present_log_size> present_log{};
std::atomic<std::uint32_t> present_log_count{0};
}
namespace {
std::atomic<Site*> sites{nullptr};
std::atomic<bool> hud_visible{false}, window_visible{false}, monitor_running{false};

// Recent values for the HUD's graphs. One writer each (the client update thread, the present
// thread); the overlay reads them racily, which at worst shows one value twice.
constexpr std::size_t history_size = 512;
struct Ring {
    std::array<std::atomic<float>, history_size> values{};
    std::atomic<std::uint64_t> written{0};
    void push(float value) noexcept {
        const auto at = written.load(std::memory_order_relaxed);
        values[at % history_size].store(value, std::memory_order_relaxed);
        written.store(at + 1, std::memory_order_release);
    }
};
std::array<Ring, 4> rings;

// Totals since the monitor's last summary, taken with exchange().
struct Totals {
    std::atomic<std::uint64_t> count{0}, sum{0}, longest{0};
    void add(std::uint64_t value) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(value, std::memory_order_relaxed);
        auto previous = longest.load(std::memory_order_relaxed);
        while (value > previous && !longest.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
    }
    struct Taken { std::uint64_t count, sum, longest; };
    Taken take() noexcept {
        return {count.exchange(0, std::memory_order_relaxed), sum.exchange(0, std::memory_order_relaxed),
                longest.exchange(0, std::memory_order_relaxed)};
    }
};
Totals client_interval, client_frame, client_own, present_interval;

std::mutex summary_mutex;
std::shared_ptr<const Summary> latest_summary = std::make_shared<Summary>();

bool wanted() noexcept {
    return hud_visible.load(std::memory_order_acquire) || window_visible.load(std::memory_order_acquire) ||
           detail::sampling.load(std::memory_order_acquire);
}

std::uint64_t filetime(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

// Per-thread CPU use, from GetThreadTimes deltas. The thread list is refreshed every few
// seconds: a Toolhelp snapshot walks every thread on the system.
class ThreadMonitor {
    struct Thread {
        HANDLE handle{};
        std::uint64_t last{};
        std::string label;
        bool seen{};
    };
    std::map<DWORD, Thread> threads_;
    std::uint64_t last_process_{}, last_wall_{};
    std::chrono::steady_clock::time_point next_refresh_{};

    static std::string module_of(std::uintptr_t address) {
        HMODULE module{};
        if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module)) return {};
        wchar_t name[MAX_PATH]{};
        if (!GetModuleBaseNameW(GetCurrentProcess(), module, name, MAX_PATH)) return {};
        std::string result;
        for (const wchar_t* c = name; *c; ++c) result.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        return result;
    }
    static std::string label_of(HANDLE handle) {
        PWSTR description{};
        std::string result;
        if (SUCCEEDED(GetThreadDescription(handle, &description)) && description) {
            for (const wchar_t* c = description; *c; ++c) result.push_back(*c < 128 ? static_cast<char>(*c) : '?');
            LocalFree(description);
        }
        if (!result.empty()) return result;
        using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
        static const auto query = reinterpret_cast<Query>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        void* start{};
        if (query) query(handle, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
        auto module = module_of(reinterpret_cast<std::uintptr_t>(start));
        std::string lower = module;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Frostbite starts its threads through the CRT, so they all begin in ucrtbase.
        if (lower == "reskate.dll") return "ReSkate worker";
        if (lower == "ucrtbase.dll" || lower == "skate.exe") return "Skate";
        if (lower.starts_with("steam") || lower.starts_with("tier0") || lower.starts_with("gameoverlay")) return "Steam";
        if (lower.starts_with("amd") || lower.starts_with("nv") || lower.starts_with("igd") || lower.starts_with("d3d")) return "Graphics driver";
        return module.empty() ? "Thread" : module;
    }
    void refresh() {
        for (auto& [id, thread] : threads_) thread.seen = false;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        const auto self = GetCurrentProcessId();
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != self) continue;
            auto [it, fresh] = threads_.try_emplace(entry.th32ThreadID);
            it->second.seen = true;
            if (fresh) {
                // Full query rights read the start address (for the label); limited ones still time it.
                it->second.handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!it->second.handle)
                    it->second.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (it->second.handle) {
                    FILETIME created, exited, kernel, user;
                    if (GetThreadTimes(it->second.handle, &created, &exited, &kernel, &user))
                        it->second.last = filetime(kernel) + filetime(user);
                    it->second.label = label_of(it->second.handle);
                }
            }
        }
        CloseHandle(snapshot);
        for (auto it = threads_.begin(); it != threads_.end();) {
            if (it->second.seen) { ++it; continue; }
            if (it->second.handle) CloseHandle(it->second.handle);
            it = threads_.erase(it);
        }
    }
public:
    static std::string label(HANDLE handle) { return label_of(handle); }
    void sample(Summary& summary) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh_) { refresh(); next_refresh_ = now + std::chrono::seconds(3); }
        FILETIME wall_time;
        GetSystemTimePreciseAsFileTime(&wall_time);
        const auto wall = filetime(wall_time);
        const double elapsed = last_wall_ ? static_cast<double>(wall - last_wall_) : 0.0;
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const auto process = filetime(kernel) + filetime(user);
            if (elapsed > 0 && last_process_) summary.process_cpu_percent = 100.0 * static_cast<double>(process - last_process_) / elapsed;
            last_process_ = process;
        }
        const auto client = detail::client_thread.load(std::memory_order_relaxed);
        const auto present = detail::present_thread.load(std::memory_order_relaxed);
        for (auto& [id, thread] : threads_) {
            if (!thread.handle || !GetThreadTimes(thread.handle, &created, &exited, &kernel, &user)) continue;
            const auto total = filetime(kernel) + filetime(user);
            const double percent = elapsed > 0 ? 100.0 * static_cast<double>(total - thread.last) / elapsed : 0.0;
            thread.last = total;
            if (percent < 0.5 && id != client && id != present) continue;
            std::string label = id == client ? "Client update" : id == present ? "Present" : thread.label;
            summary.threads.push_back({static_cast<std::uint32_t>(id), std::move(label), percent});
        }
        last_wall_ = wall;
        std::ranges::sort(summary.threads, [](const ThreadRow& a, const ThreadRow& b) { return a.cpu_percent > b.cpu_percent; });
    }
    ~ThreadMonitor() {
        for (auto& [id, thread] : threads_) if (thread.handle) CloseHandle(thread.handle);
    }
};

void monitor() {
    SetThreadDescription(GetCurrentThread(), L"ReSkate profiler");
    ThreadMonitor threads;
    auto last = std::chrono::steady_clock::now();
    // Zone ticks to nanoseconds: the timestamp counter measured against the steady clock over
    // each summary's second.
    auto last_ticks = __rdtsc();
    while (wanted()) {
        zones_enabled.store(true, std::memory_order_relaxed);
        Sleep(1000);
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::max(1e-3, std::chrono::duration<double>(now - last).count());
        last = now;
        const auto now_ticks = __rdtsc();
        const double ms_per_tick = seconds * 1e3 / static_cast<double>(std::max<std::uint64_t>(1, now_ticks - last_ticks));
        last_ticks = now_ticks;
        auto next = std::make_shared<Summary>();
        auto& s = *next;
        s.seconds = seconds;
        s.cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        s.client_thread = detail::client_thread.load(std::memory_order_relaxed);
        s.present_thread = detail::present_thread.load(std::memory_order_relaxed);
        const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
        const auto interval = client_interval.take(), frame = client_frame.take(), own = client_own.take();
        s.client_updates_per_second = static_cast<double>(own.count) / seconds;
        if (interval.count) {
            s.client_interval_average_ms = ms(interval.sum) / static_cast<double>(interval.count);
            s.client_interval_longest_ms = ms(interval.longest);
        }
        if (frame.count) {
            s.client_frame_average_ms = ms(frame.sum) / static_cast<double>(frame.count);
            s.client_frame_longest_ms = ms(frame.longest);
        }
        if (own.count) {
            s.client_own_average_ms = ms(own.sum) / static_cast<double>(own.count);
            s.client_own_longest_ms = ms(own.longest);
        }
        const auto presents = present_interval.take();
        s.presents_per_second = static_cast<double>(presents.count) / seconds;
        if (presents.count) {
            s.present_interval_average_ms = ms(presents.sum) / static_cast<double>(presents.count);
            s.present_interval_longest_ms = ms(presents.longest);
        }
        double hooks_ms{};
        for (auto* site = sites.load(std::memory_order_acquire); site; site = site->next) {
            const auto calls = site->calls.exchange(0, std::memory_order_relaxed);
            const auto total = static_cast<double>(site->ticks.exchange(0, std::memory_order_relaxed)) * ms_per_tick;
            const auto longest = static_cast<double>(site->longest.exchange(0, std::memory_order_relaxed)) * ms_per_tick;
            if (!calls) continue;
            ZoneRow row{site->name, static_cast<double>(calls) / seconds, total / seconds,
                        total * 1e3 / static_cast<double>(calls), longest};
            if (std::string_view(site->name).starts_with("hooks/")) hooks_ms += total;
            // Sites sharing a name (the same work in two hooks) show as one row.
            const auto same = std::ranges::find(s.zones, row.name, &ZoneRow::name);
            if (same == s.zones.end()) { s.zones.push_back(std::move(row)); continue; }
            const auto calls_total = same->calls_per_second + row.calls_per_second;
            same->average_microseconds = calls_total > 0 ? (same->average_microseconds * same->calls_per_second +
                row.average_microseconds * row.calls_per_second) / calls_total : 0.0;
            same->calls_per_second = calls_total;
            same->milliseconds_per_second += row.milliseconds_per_second;
            same->longest_milliseconds = std::max(same->longest_milliseconds, row.longest_milliseconds);
        }
        if (own.count) s.hooks_milliseconds_per_update = hooks_ms / static_cast<double>(own.count);
        std::ranges::sort(s.zones, [](const ZoneRow& a, const ZoneRow& b) {
            return a.milliseconds_per_second > b.milliseconds_per_second;
        });
        threads.sample(s);
        std::lock_guard lock(summary_mutex);
        latest_summary = std::move(next);
    }
    zones_enabled.store(false, std::memory_order_relaxed);
    monitor_running.store(false, std::memory_order_release);
    // Something may have asked again between the last check and the flag being cleared.
    if (wanted()) detail::wake_monitor();
}
} // namespace

namespace detail {
std::string thread_label(HANDLE thread, std::uint32_t id) {
    if (id == client_thread.load(std::memory_order_relaxed)) return "Client update";
    if (id == present_thread.load(std::memory_order_relaxed)) return "Present";
    return ThreadMonitor::label(thread);
}
void wake_monitor() noexcept {
    if (!wanted() || monitor_running.exchange(true, std::memory_order_acq_rel)) return;
    zones_enabled.store(true, std::memory_order_relaxed);
    try { std::thread(monitor).detach(); }
    catch (...) { monitor_running.store(false, std::memory_order_release); }
}
}

Site::Site(const char* site_name) noexcept : name(site_name) {
    auto* head = sites.load(std::memory_order_relaxed);
    do { next = head; } while (!sites.compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
}

void record_client_update(std::uint64_t own_ns) noexcept {
    // Known even while off, so a sample can target it straight away.
    detail::client_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!zones_enabled.load(std::memory_order_relaxed)) return;
    static std::uint64_t last{};
    const auto now = now_ns();
    if (last && now > last) {
        client_interval.add(now - last);
        rings[static_cast<std::size_t>(Series::client_interval)].push(static_cast<float>(now - last) / 1e6f);
    }
    last = now;
    client_own.add(own_ns);
    rings[static_cast<std::size_t>(Series::client_own)].push(static_cast<float>(own_ns) / 1e6f);
}

void record_client_frame(std::uint64_t frame_ns) noexcept {
    if (!zones_enabled.load(std::memory_order_relaxed)) return;
    client_frame.add(frame_ns);
    rings[static_cast<std::size_t>(Series::client_frame)].push(static_cast<float>(frame_ns) / 1e6f);
}

void record_present() noexcept {
    detail::present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (detail::present_log_on.load(std::memory_order_relaxed)) {
        const auto n = detail::present_log_count.load(std::memory_order_relaxed);
        if (n < detail::present_log_size) {
            detail::present_log[n].store(now_ns(), std::memory_order_relaxed);
            detail::present_log_count.store(n + 1, std::memory_order_release);
        }
    }
    if (!zones_enabled.load(std::memory_order_relaxed)) return;
    static std::uint64_t last{};
    const auto now = now_ns();
    if (last && now > last) {
        present_interval.add(now - last);
        rings[static_cast<std::size_t>(Series::present_interval)].push(static_cast<float>(now - last) / 1e6f);
    }
    last = now;
}

std::size_t history(Series series, std::span<float> out) noexcept {
    auto& ring = rings[static_cast<std::size_t>(series)];
    const auto written = ring.written.load(std::memory_order_acquire);
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>({written, history_size, out.size()}));
    for (std::size_t i = 0; i < count; ++i)
        out[i] = ring.values[(written - count + i) % history_size].load(std::memory_order_relaxed);
    return count;
}

void set_hud(bool visible) noexcept { hud_visible.store(visible, std::memory_order_release); detail::wake_monitor(); }
bool hud() noexcept { return hud_visible.load(std::memory_order_acquire); }
void set_window(bool visible) noexcept { window_visible.store(visible, std::memory_order_release); detail::wake_monitor(); }
bool window() noexcept { return window_visible.load(std::memory_order_acquire); }
bool active() noexcept { return wanted(); }

std::shared_ptr<const Summary> summary() noexcept {
    std::lock_guard lock(summary_mutex);
    return latest_summary;
}
} // namespace dingosdk::profiler
