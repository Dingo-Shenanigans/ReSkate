#include "runtime_internal.h"
#include "Engine/Core/Debug/trace.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/profiler_labels.h"

// The game's client frame job, timed for the profiler (Engine/Core/Profiling/profiler.h). It runs
// the whole client frame, ReSkate's tick included, so its time against the sim budget (16.7 ms at
// 60 Hz) says how close the game is to slow motion. Two clock reads per client frame.
namespace dingosdk::runtime::detail {
namespace {
using ClientFrameJob = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
ClientFrameJob original_client_frame_job{};
std::uintptr_t client_frame_job(std::uintptr_t job, std::uintptr_t timing, std::uintptr_t dependencies, std::uintptr_t extra) {
    const auto start = dingosdk::profiler::now_ns();
    dingosdk::trace::frame(start); // frame pacing for a level session's timing; a few atomic adds
    const auto result = original_client_frame_job(job, timing, dependencies, extra);
    dingosdk::profiler::record_client_frame(dingosdk::profiler::now_ns() - start);
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
