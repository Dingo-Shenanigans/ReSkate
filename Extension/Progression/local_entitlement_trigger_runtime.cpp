#include "Engine/Core/Log/logging.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_challenge_runtime.h"
#include "local_entitlement_trigger_runtime.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include <algorithm>

namespace dingosdk::profile_runtime {
// EntitlementTrigger's two retail graphs compute:

//   connected && session_ready && (HasEntitlement(id) == RequireEntitlement).

// Returning ownership alone leaves both the interaction and map branches idle

// offline. Supply *provider readiness* only while those exact graphs execute;

// their existing entitlement lookup still decides ownership from the local profile.

// No connection state, ECS activation bits, or map entries are fabricated.

// See analysis/local-entitlement-trigger-readiness.md for the bytecode contract.

thread_local std::uintptr_t executing_expression{};

thread_local const ExpressionExecutionScope* expression_scope{};

// Both hooks run for every expression the game executes, thousands a frame on several threads:
// no profiler zones here (each was two timestamp reads and shared atomic adds whenever the
// profiler ran, which inflated the hooks in the very samples that measured them, 2026-10-03).
// The sampler attributes their time.
void execute_expression_hook(std::uintptr_t vm, std::uint32_t pc) {
    if(multiplayer::consume_match_marker_expression(vm,pc))return;
    // The setup helper only prepares registers. Native calls actually execute
    // in 1416d0b90 / 1416d1a10, including direct ECS interpreter entry paths.
    // Keep the exact cursor and profiler context when entering either runner.
    ExpressionExecutionScope scope(vm);
    initialize_starter_from_expression(vm, pc);
    multiplayer::execute_throwdown_preview_expression(vm, pc, 0,
        [](std::uintptr_t expression, std::uint32_t cursor, std::uintptr_t) {
            local_runtime().execute_expression(expression, cursor);
        });
    deliver_local_challenge_completion(vm, pc);
    multiplayer::complete_native_throwdown_parameters(vm);
}

void execute_profiled_expression_hook(std::uintptr_t vm, std::uint32_t pc, std::uintptr_t profiler) {
    if(multiplayer::consume_match_marker_expression(vm,pc))return;
    ExpressionExecutionScope scope(vm);
    initialize_starter_from_expression(vm, pc);
    multiplayer::execute_throwdown_preview_expression(vm, pc, profiler, local_runtime().execute_profiled_expression);
    deliver_local_challenge_completion(vm, pc);
    multiplayer::complete_native_throwdown_parameters(vm);
}

unsigned local_entitlement_trigger_graph() {
    if (!executing_expression || !local_runtime().active.load(std::memory_order_acquire)) return 0;
    std::uintptr_t instance{}, resource{}, current_resource{};
    std::uint32_t key{};
    // Asked by connection/session predicates in many scripts: the running graph's key is peeked
    // first (no system call), and the rest read only for one of the two graphs.
    if (!memory::peek(executing_expression + 0x38, current_resource) || !memory::peek(current_resource + 0x10, key) ||
        std::ranges::none_of(entitlement_trigger_graphs, [&](const auto& graph) { return graph.key == key; }) ||
        !read(executing_expression + 0x30, instance) || !read(instance, resource) || resource != current_resource) return 0;
    for (unsigned i = 0; i < entitlement_trigger_graphs.size(); ++i) {
        const auto& graph = entitlement_trigger_graphs[i];
        if (key != graph.key) continue;
        std::array<std::uint32_t, 10> layout{};
        if (read(resource + 0x20, layout) && layout == graph.layout) return i + 1;
    }
    return 0;
}

void append_entitlement_trigger_context(std::ostringstream& event) {
    // Called only for the first saved lookup of each bus-stop ID (bounded by
    // logged_entitlements). Capture the actual executing graph for live QA,
    // including a rejected layout, without recording entity/player data.
    std::uintptr_t resource{}, instance{}, instance_resource{};
    std::uint32_t key{};
    std::array<std::uint32_t, 10> layout{};
    event << ",\"script\":";
    if (!executing_expression || !read(executing_expression + 0x38, resource) ||
        !read(resource + 0x10, key) || !read(resource + 0x20, layout)) {
        event << "null";
        return;
    }
    const bool instance_matches = read(executing_expression + 0x30, instance) &&
        read(instance, instance_resource) && instance_resource == resource;
    event << "{\"key\":" << key << ",\"instance_matches\":" << (instance_matches ? "true" : "false")
          << ",\"recognized\":" << local_entitlement_trigger_graph() << ",\"layout\":[";
    for (std::size_t i = 0; i < layout.size(); ++i) event << (i ? "," : "") << layout[i];
    event << "]}";
}

bool local_trigger_ready(unsigned predicate) {
    // Preserve the native caller's LastError even when safe-read validation
    // rejects the graph and the caller subsequently falls back to the engine.
    PreserveError preserve;
    const auto graph = local_entitlement_trigger_graph();
    if (!graph) return false;
    const unsigned bit = 1u << ((graph - 1) * 2 + predicate);
    if (!(local_runtime().logged_trigger_ready.fetch_or(bit) & bit)) {
        try {
            std::ostringstream event;
            event << "{\"event\":\"local_profile_trigger_ready\",\"graph\":"
                  << std::quoted(graph == 1 ? "EntitlementTrigger.Activate" : "EntitlementTrigger.OnEntitlementsChanged")
                  << ",\"gate\":" << std::quoted(predicate ? "session" : "connection")
                  << ",\"source\":\"saved_profile\"}";
            dingosdk::logging::event(dingosdk::logging::Channel::progression, event.str().c_str());
        } catch (...) {}
    }
    return true;
}

bool trigger_online_hook() {
    return local_trigger_ready(0) || multiplayer::native_throwdown_ready(0) || local_runtime().trigger_online();
}

bool trigger_session_hook() {
    return local_trigger_ready(1) || multiplayer::native_throwdown_ready(1) || local_runtime().trigger_session();
}
}
