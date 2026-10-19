#include "runtime_internal.h"
#include "Engine/Core/Debug/trace.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/profiler_labels.h"
#include "Extension/World/level_loading.h"
#include <format>

// The game's client frame job, timed for the profiler (Engine/Core/Profiling/profiler.h). It runs
// the whole client frame, ReSkate's tick included, so its time against the sim budget (16.7 ms at
// 60 Hz) says how close the game is to slow motion. Two clock reads per client frame.
namespace dingosdk::runtime::detail {
namespace {
using ClientFrameJob = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
ClientFrameJob original_client_frame_job{};

// A freeze: half a second or more from one client frame's start to the next. When this launch
// sends timings, each is sent as a "game.freeze" saying where the time went:
//  - the game's client frame (which ReSkate's tick runs inside), and ReSkate's own tick in it;
//  - the rest, outside the frame: the game waiting on rendering, the GPU or loading;
//  - how many frames the renderer presented meanwhile (none: it stopped as well);
//  - ReSkate's zones (profiler.h) that took the most time in it, on any thread.
// Players reported freezes of about a second while skating (2026-10-15); the level session's
// timing only gave their length.
constexpr std::uint64_t freeze_ns = 500'000'000;
constexpr unsigned freezes_per_launch = 30;
struct Frames {               // the client thread's alone
    std::uint64_t start{}, frame_ns{}, own_ns{}, presents{};
    dingosdk::profiler::ZoneMark mark;
    unsigned sent{};
    bool watching{};
};
Frames& frames() {
    static auto* value = new Frames;
    return *value;
}
void describe_freeze(Frames& f, std::uint64_t start) {
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const auto interval = start - f.start;
    const auto frame = std::min(f.frame_ns, interval);
    const auto own = std::min(f.own_ns, frame);
    const auto presented = dingosdk::profiler::presents() - f.presents;
    const char* where = own * 2 >= interval ? "ReSkate's tick"
        : frame * 2 >= interval ? "the game's frame"
        : "outside the frame (rendering, GPU or loading)";
    dingosdk::trace::Tags tags{{"destination", dingosdk::last_level_destination()},
                               {"where", where},
                               {"renderer", presented ? "kept presenting" : "stopped too"}};
    dingosdk::trace::Measurements measurements{
        {"freeze", ms(interval), "millisecond"}, {"client_frame", ms(frame), "millisecond"},
        {"reskate_tick", ms(own), "millisecond"}, {"outside_frame", ms(interval - frame), "millisecond"},
        {"presents", static_cast<double>(presented)}};
    const auto zones = dingosdk::profiler::zones_since(f.mark, 4);
    for (std::size_t i = 0; i < zones.size(); ++i)
        tags.emplace_back(std::format("zone.{}", i + 1), std::format("{} ({:.0f} ms)", zones[i].name, zones[i].milliseconds));
    dingosdk::trace::record("game.freeze", "app.freeze",
        std::chrono::milliseconds(static_cast<long long>(interval / 1'000'000)), true, std::move(tags), std::move(measurements));
}
// At each client frame's start: the stretch since the last one, described if it froze, and the
// marks the next is measured against.
void watch_frame(std::uint64_t start) noexcept {
    if (!dingosdk::trace::active()) return;
    try {
        auto& f = frames();
        if (!f.watching) {
            f.watching = true;
            dingosdk::profiler::keep_zones(true);
        }
        // (A level loading holds the frame up by design: only freezes in a loaded level are sent.)
        if (f.start && start > f.start && start - f.start >= freeze_ns && f.sent < freezes_per_launch &&
            dingosdk::level_content_active()) {
            ++f.sent;
            describe_freeze(f, start);
        }
        f.start = start;
        f.presents = dingosdk::profiler::presents();
        dingosdk::profiler::mark_zones(f.mark);
    } catch (...) {}
}

std::uintptr_t client_frame_job(std::uintptr_t job, std::uintptr_t timing, std::uintptr_t dependencies, std::uintptr_t extra) {
    const auto start = dingosdk::profiler::now_ns();
    dingosdk::trace::frame(start); // frame pacing for a level session's timing; a few atomic adds
    watch_frame(start);
    const auto result = original_client_frame_job(job, timing, dependencies, extra);
    const auto took = dingosdk::profiler::now_ns() - start;
    dingosdk::profiler::record_client_frame(took);
    if (auto& f = frames(); f.watching) {
        f.frame_ns = took;
        f.own_ns = dingosdk::profiler::last_client_own_ns();
    }
    return result;
}
}
void start_frame_timing() {
    const auto& contract = addr::profiler_labels::client_frame_job;
    auto* target = reinterpret_cast<void*>(runtime().base + contract.rva);
    bool enabled = false;
    if (matches(reinterpret_cast<std::uintptr_t>(target), contract.bytes)) {
        void* original{};
        if (dingosdk::hook_prepare(target, reinterpret_cast<void*>(&client_frame_job), &original) == dingosdk::HookOk) {
            original_client_frame_job = reinterpret_cast<ClientFrameJob>(original);
            enabled = dingosdk::hook_enable(target) == dingosdk::HookOk;
            if (!enabled) (void)dingosdk::hook_remove(target);
        }
    }
    record(std::string("{\"event\":\"frame_timing_initialized\",\"active\":") + (enabled ? "true}" : "false}"));
}
}
