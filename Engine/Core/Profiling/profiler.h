#pragma once
#include <atomic>
#include <chrono>
#include <intrin.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// ReSkate's performance profiler (Extension/UI/Overlay/perf_overlay.cpp draws it, `perf` console
// commands drive it):
//  * zones: scoped timers on ReSkate's own work, so its cost on the game's threads is visible;
//  * the client update (the hooked native client tick) and presented frames, timed;
//  * CPU use per thread of the game, which shows a runaway worker at a glance;
//  * an in-process stack sampler for one thread, with a report the overlay lists and a file of
//    collapsed stacks (load it in speedscope.app or flamegraph.pl for a flame graph).
// Everything is off until something asks for it: a zone then costs one relaxed atomic load.
namespace dingosdk::profiler {

// ---- zones ----------------------------------------------------------------------------------
// A place in ReSkate's code whose time is measured. Declared once (DINGO_PROFILE_ZONE makes a
// function-local static), timed on any thread. "a/b" names nest under "a" in the window.
// Zones count CPU timestamp ticks (__rdtsc, a few nanoseconds; invariant on every CPU the game
// runs on) rather than steady_clock (~25 ns through QueryPerformanceCounter): the hook zones run
// ~80,000 times a second, where the clock had cost more than the work it timed.
struct Site {
    explicit Site(const char* name) noexcept;
    const char* name;
    std::atomic<std::uint64_t> ticks{0}, calls{0}, longest{0};
    Site* next{};
    void add(std::uint64_t elapsed) noexcept {
        ticks.fetch_add(elapsed, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
        auto previous = longest.load(std::memory_order_relaxed);
        while (elapsed > previous && !longest.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {}
    }
};
extern std::atomic<bool> zones_enabled;
inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
class Scope {
    Site* site_;
    std::uint64_t start_;
public:
    explicit Scope(Site& site) noexcept
        : site_(zones_enabled.load(std::memory_order_relaxed) ? &site : nullptr), start_(site_ ? __rdtsc() : 0) {}
    ~Scope() { if (site_) site_->add(__rdtsc() - start_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
#define DINGO_PROFILE_CAT2(a, b) a##b
#define DINGO_PROFILE_CAT(a, b) DINGO_PROFILE_CAT2(a, b)
#define DINGO_PROFILE_ZONE(name)                                                                       \
    static ::dingosdk::profiler::Site DINGO_PROFILE_CAT(dingo_profile_site_, __LINE__){name};          \
    const ::dingosdk::profiler::Scope DINGO_PROFILE_CAT(dingo_profile_scope_, __LINE__){                 \
        DINGO_PROFILE_CAT(dingo_profile_site_, __LINE__)}

// ---- frame timing ---------------------------------------------------------------------------
// ReSkate's client tick (once per client update): its own work, timed.
void record_client_update(std::uint64_t own_ns) noexcept;
// The game's whole client frame job, which the tick runs inside: the number that has to stay
// within the sim budget (16.7 ms at 60 Hz) or the game runs in slow motion.
void record_client_frame(std::uint64_t frame_ns) noexcept;
// Each presented frame (the overlay's Present hook).
void record_present() noexcept;
enum class Series { client_interval, client_frame, client_own, present_interval };
// The most recent values in milliseconds, oldest first; returns how many were written.
std::size_t history(Series series, std::span<float> out) noexcept;

// ---- what is on ------------------------------------------------------------------------------
// The HUD and window are the overlay's (both start off each launch); turning either on starts
// the timing and thread monitor.
void set_hud(bool visible) noexcept;
bool hud() noexcept;
void set_window(bool visible) noexcept;
bool window() noexcept;
// Whether anything wants the profiler running (HUD, window or a sample).
bool active() noexcept;

// ---- once-a-second summary ---------------------------------------------------------------------
struct ZoneRow {
    std::string name;
    double calls_per_second{}, milliseconds_per_second{}, average_microseconds{}, longest_milliseconds{};
};
struct ThreadRow {
    std::uint32_t id{};
    std::string label;
    double cpu_percent{};
};
struct Summary {
    double seconds{};                 // the window the rates cover
    double client_updates_per_second{}, client_interval_average_ms{}, client_interval_longest_ms{};
    double client_frame_average_ms{}, client_frame_longest_ms{}; // the game's client frame job
    double client_own_average_ms{}, client_own_longest_ms{};
    double hooks_milliseconds_per_update{}; // zones named "hooks/..." per client update
    double presents_per_second{}, present_interval_average_ms{}, present_interval_longest_ms{};
    double process_cpu_percent{};     // 100 = one whole core
    unsigned cores{};
    std::uint32_t client_thread{}, present_thread{};
    std::vector<ZoneRow> zones;       // busiest first
    std::vector<ThreadRow> threads;   // busiest first
};
std::shared_ptr<const Summary> summary() noexcept;

// ---- the stack sampler -------------------------------------------------------------------------
// `all` samples every thread that was busy (3% of a core or more) when the sample started, and
// reports where the process's CPU work went; time spent waiting in the kernel is left out.
enum class Target { client, present, thread, all };
struct SampleRequest {
    Target target = Target::client;
    std::uint32_t thread_id{};        // for Target::thread
    double seconds = 10;
    unsigned rate = 1000;             // samples per second
    // Above 0: a spike sample. Presented frames that took longer than this many milliseconds are
    // the spikes, and the report only counts samples taken during them (`perf spikes`).
    double spike_ms = 0;
};
struct SampledFunction {
    std::string name;                 // "Skate 0x14xxxxxxx [label]" or "module!symbol"
    std::uint64_t address{};          // function start; Skate.exe ones as Ghidra addresses
    std::uint32_t self{}, total{};    // samples with it on top / anywhere on the stack
};
struct SampleReport {
    bool running{};
    double progress{};                // 0..1 while running
    std::string target;
    std::uint32_t thread_id{};
    std::uint32_t samples{}, rate{};
    double seconds{};
    double busy_percent{};            // samples not waiting in the kernel
    std::vector<SampledFunction> functions; // by self, then total
    std::vector<ThreadRow> threads;   // Target::all: each thread's share of the busy samples
    double spike_ms{};                // a spike sample's threshold, 0 for an ordinary sample
    std::uint32_t spikes{};           // frames over the threshold
    double spike_longest_ms{}, spike_total_ms{};
    std::filesystem::path report, folded;   // written next to ReSkate.log
    std::string error;
};
// Starts a sample on its own thread; false (with the reason) if one is running or the target
// thread is unknown.
bool start_sampling(const SampleRequest& request, std::string& error);
void stop_sampling() noexcept;
std::shared_ptr<const SampleReport> sample_report() noexcept;

// Engine function names for the sampler's report, by RVA (Engine/Game/Build/<build>/profiler.h,
// plus any "<rva or 0x14 address> <name>" lines in ReSkate.labels.tsv next to Skate.exe).
struct Label { std::uintptr_t rva; const char* name; };
void set_engine_labels(std::span<const Label> labels) noexcept;
}
