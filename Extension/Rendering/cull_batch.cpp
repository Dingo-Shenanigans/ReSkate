#include "cull_batch.h"
#include "cull_batch_core.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Game/Build/20260929/cull_batch.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dingosdk::cull_batch {
namespace {
namespace contract = game::build::v20260929::cull_batch;
using Flat = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
Flat original{};
std::atomic<bool> installed{};
std::atomic<unsigned> wanted{8};
std::atomic<std::uint64_t> items{}, reservations{}, calls{};
std::mutex install_mutex;
std::array<detail::Gate, 2> gates{};
// Validated native arguments; faults disable batching for this call, never the native renderer.
bool inputs(std::uintptr_t context, std::uintptr_t chunk, std::uintptr_t world, detail::Frame& frame) noexcept {
    if (world > 15) return false;
    __try {
        frame.counter = *reinterpret_cast<volatile LONG**>(context + contract::counters_offset + world * 8);
        frame.bound = *reinterpret_cast<const std::uint32_t*>(chunk + contract::count_offset);
        return frame.counter != nullptr && frame.bound <= static_cast<unsigned>(std::numeric_limits<LONG>::max());
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Scope {
    detail::Frame frame;
    detail::Frame* previous = detail::active_frame;
    Scope(std::uintptr_t context, std::uintptr_t chunk, std::uintptr_t world) noexcept {
        frame.batch = wanted.load(std::memory_order_relaxed);
        if (frame.batch > 1 && !inputs(context, chunk, world, frame)) frame.batch = 1;
        detail::active_frame = &frame;
    }
    ~Scope() {
        detail::active_frame = previous;
        if (frame.batch > 1) {
            items.fetch_add(frame.items, std::memory_order_relaxed);
            reservations.fetch_add(frame.reservations, std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
};
void flat(std::uintptr_t context, std::uintptr_t chunk, std::uintptr_t world, std::uintptr_t flags, std::uintptr_t output) {
    Scope scope(context, chunk, world);
    DINGO_PROFILE_ZONE("engine/cull flat");
    original(context, chunk, world, flags, output);
}
}

bool install(std::uintptr_t base) noexcept {
    std::lock_guard lock(install_mutex);
    if (installed.load()) return true;
    std::vector<void*> prepared;
    try {
        prepared.reserve(3);
        if (!base || !IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE)) throw std::runtime_error("AVX is unavailable");
        std::array<unsigned char, 32> head{};
        if (!memory::peek(base + contract::flat.rva, head) || head != contract::flat.bytes)
            throw std::runtime_error("flat culling fingerprint differs");
        for (const auto& site : contract::claims) {
            std::array<unsigned char, 14> bytes{};
            if (!memory::peek_bytes(base + site.rva, bytes.data(), site.size) ||
                !std::equal(bytes.begin(), bytes.begin() + site.size, site.bytes.begin()))
                throw std::runtime_error("culling claim or following comparison differs");
        }
        void* relay{};
        auto* target = reinterpret_cast<void*>(base + contract::flat.rva);
        if (hook_prepare(target, reinterpret_cast<void*>(&flat), &relay) != HookOk)
            throw std::runtime_error("flat culling hook preparation failed");
        prepared.push_back(target);
        original = reinterpret_cast<Flat>(relay);
        for (std::size_t i = 0; i < gates.size(); ++i) {
            const auto at = base + contract::claims[i].rva;
            gates[i] = detail::make_gate(reinterpret_cast<void*>(at + contract::claim_bytes));
            if (hook_prepare(reinterpret_cast<void*>(at), gates[i].code, &relay) != HookOk)
                throw std::runtime_error("culling claim hook preparation failed");
            prepared.push_back(reinterpret_cast<void*>(at));
            *gates[i].stock = relay;
        }
        for (auto* at : prepared) if (hook_queue_enable(at) != HookOk) throw std::runtime_error("could not queue culling hooks");
        if (hook_apply_queued() != HookOk) throw std::runtime_error("could not attach culling hooks");
        installed.store(true);
        try {
            logging::write(logging::Level::info, logging::Channel::runtime,
                "Cull batching: flat-path workers claim up to 8 records at once; `perf cullbatch 1` uses native one-item claims.");
        } catch (...) {}
        return true;
    } catch (const std::exception& error) {
        for (auto* at : prepared) hook_remove(at);
        for (auto& gate : gates) { detail::release_gate(gate); gate = {}; }
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Cull batching unavailable: {}.", error.what());
        return false;
    }
}
bool available() noexcept { return installed.load(); }
unsigned size() noexcept { return wanted.load(std::memory_order_relaxed); }
bool set_size(unsigned value) noexcept {
    if (!available() || (value != 0 && value != 1 && value != 2 && value != 4 && value != 8 && value != 16 && value != 32)) return false;
    wanted.store(value ? value : 1, std::memory_order_relaxed);
    return true;
}
std::string status() {
    if (!available()) return "Cull batching is unavailable for this build.";
    return std::format("Flat culling: {} item(s) per claim; {} items reserved in {} batches over {} calls. "
        "1 uses native claims with instrumentation installed; tree culling is unchanged. Not saved.",
        size(), items.load(), reservations.load(), calls.load());
}
}
