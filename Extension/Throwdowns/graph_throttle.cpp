#include "graph_throttle.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/graph_throttle.h"
#include "Engine/Core/Platform/memory.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;

// The two re-post natives as the loader binds them in a call slot: the functions of their
// {fn, hash} table rows. Each takes one pointer per operand, five here, and writes none back
// (their operands: Engine/Game/Build/20260929/graph_throttle.h).
constexpr std::uint32_t post_seconds_hash = addr::graph_throttle::post_seconds_hash,
                        post_frames_hash = addr::graph_throttle::post_frames_hash;
constexpr Address post_seconds_fast = addr::graph_throttle::post_seconds_fast,
                  post_frames_fast = addr::graph_throttle::post_frames_fast;
using PostSeconds = void (*)(const void*, const void*, const void*, const void*, const float*);
using PostFrames = void (*)(const void*, const void*, const void*, const void*, const std::int32_t*);

// How long a waiting loop now waits before it asks again.
constexpr float wait_seconds = 1.0f;
constexpr std::int32_t wait_frames = 60;
std::atomic<Address> image{}; // Skate.exe's base, set before the first slot is pointed here

void post_seconds_later(const void* self, const void* event, const void* port, const void* payload, const float*) {
    reinterpret_cast<PostSeconds>(image.load(std::memory_order_acquire) + post_seconds_fast)(self, event, port, payload,
                                                                                               &wait_seconds);
}
void post_frames_later(const void* self, const void* payload, const void* port, const void* type, const std::int32_t*) {
    reinterpret_cast<PostFrames>(image.load(std::memory_order_acquire) + post_frames_fast)(self, payload, port, type,
                                                                                            &wait_frames);
}
void skip_post(const void*, const void*, const void*, const void*, const void*) {}

enum class Change : std::uint8_t { stop, wait };
// Each graph's re-post as shipped (Engine/Game/Build/20260929/graph_throttle.h), and what becomes
// of it: stop drops it, wait has it ask again after wait_seconds (wait_frames). The graphs' other
// re-posts stay.
struct Graph : addr::graph_throttle::Repost {
    Change change;
};
constexpr std::array graphs{
    Graph{addr::graph_throttle::coordinator_debug_draw, Change::stop},
    Graph{addr::graph_throttle::input_visualization, Change::stop},
    Graph{addr::graph_throttle::leaderboard_debug_print, Change::stop},
    Graph{addr::graph_throttle::narrative_play_retry, Change::wait},
    Graph{addr::graph_throttle::quick_drop_entitlements, Change::wait},
    Graph{addr::graph_throttle::milestone_init, Change::wait},
    Graph{addr::graph_throttle::quest_delayed_init, Change::wait},
};

// The last copy of each graph that did not match, so it is not checked again every run.
std::array<std::atomic<Address>, graphs.size()> refused{};

template <class T> T read(Address at) {
    T value{};
    if (!memory::peek(at, value)) throw std::runtime_error("memory unavailable");
    return value;
}

Address fast_binding(const Graph& graph) {
    return graph.native == post_frames_hash ? post_frames_fast : post_seconds_fast;
}
void* replacement(const Graph& graph) {
    if (graph.change == Change::stop) return reinterpret_cast<void*>(&skip_post);
    return graph.native == post_frames_hash ? reinterpret_cast<void*>(&post_frames_later)
                                            : reinterpret_cast<void*>(&post_seconds_later);
}

// This copy's re-post slot, if the copy is exactly the shipped graph.
Address slot_of(Address resource, const Graph& graph) {
    if (read<std::uint32_t>(resource + 0x20) != graph.frame || read<std::uint32_t>(resource + 0x24) != graph.constants ||
        read<std::uint32_t>(resource + 0x2c) != graph.code_words)
        throw std::runtime_error("header differs");
    const auto code = resource + graph.code_offset;
    if (read<std::uint32_t>(code) != graph.first_word) throw std::runtime_error("code differs");
    return code + graph.pc + 4;
}

// Points this copy's re-post at the replacement, after checking the call table and the call.
void patch(Address base, Address resource, const Graph& graph, Address slot) {
    const auto code = resource + graph.code_offset;
    unsigned sites{};
    bool found{};
    for (std::uint32_t i = 0; i < graph.table_count; ++i) {
        const auto entry = read<std::array<std::uint32_t, 2>>(resource + graph.table_offset + 8 * std::size_t{i});
        if (entry[1] >= graph.code_words * 4) throw std::runtime_error("call table differs");
        if (entry[0] != graph.native) continue;
        ++sites;
        found = found || entry[1] == graph.pc;
    }
    if (sites != graph.sites || !found) throw std::runtime_error("re-post calls differ");
    // Four operands and the delay: self (S0), ..., the zero delay constant.
    if (read<std::uint32_t>(code + graph.pc) != ((graph.next << 8) | 0x04) ||
        read<std::array<std::uint32_t, 2>>(code + graph.pc + 12) != std::array<std::uint32_t, 2>{3, 0} ||
        read<std::array<std::uint32_t, 2>>(code + graph.pc + 12 + 8 * 4) != std::array<std::uint32_t, 2>{0, graph.delay})
        throw std::runtime_error(std::format("re-post at pc {:#x} differs", graph.pc));
    const auto bound = read<Address>(slot);
    if (bound != base + fast_binding(graph))
        throw std::runtime_error(std::format("re-post at pc {:#x} is bound to {:#x}, expected {:#x}", graph.pc,
                                             bound - base, fast_binding(graph)));
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(slot), &info, sizeof(info)) ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        throw std::runtime_error("graph memory is not writable");
    image.store(base, std::memory_order_release);
    // These graphs run only in the client realm, on the thread running this pump.
    *reinterpret_cast<void**>(slot) = replacement(graph);
}

void settle(Address base, Address resource, std::size_t index) noexcept {
    const auto& graph = graphs[index];
    if (refused[index].load(std::memory_order_relaxed) == resource) return;
    try {
        const auto slot = slot_of(resource, graph);
        if (read<Address>(slot) == reinterpret_cast<Address>(replacement(graph))) return; // this copy is done
        patch(base, resource, graph, slot);
        if (graph.change == Change::stop)
            logging::log(logging::Level::info, logging::Channel::progression,
                         "Graph throttle: {} ({:#x}) stopped; its runs only drew debug output.", graph.name, graph.hash);
        else
            logging::log(logging::Level::info, logging::Channel::progression,
                         "Graph throttle: {} ({:#x}) now waits 1 s between attempts.", graph.name, graph.hash);
    } catch (const std::exception& e) {
        refused[index].store(resource, std::memory_order_relaxed);
        logging::log(logging::Level::warning, logging::Channel::progression,
                     "Graph throttle: {} ({:#x}) left native: {}.", graph.name, graph.hash, e.what());
    } catch (...) {
        refused[index].store(resource, std::memory_order_relaxed);
    }
}
} // namespace

bool throttled_graph(std::uint32_t hash) noexcept {
    for (const auto& graph : graphs)
        if (graph.hash == hash) return true;
    return false;
}

void throttle_graph_ran(Address base, Address vm) noexcept {
    // The pump has just read these for the same run: the expression and its resource are alive.
    if (!vm) return;
    const auto resource = *reinterpret_cast<const Address*>(vm + 0x38);
    if (!resource) return;
    const auto hash = *reinterpret_cast<const std::uint32_t*>(resource + 0x10);
    for (std::size_t i = 0; i < graphs.size(); ++i)
        if (graphs[i].hash == hash) {
            settle(base, resource, i);
            return;
        }
}
} // namespace dingosdk::multiplayer
