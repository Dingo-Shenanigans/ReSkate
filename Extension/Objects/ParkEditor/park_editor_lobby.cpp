#include "park_editor_runtime.h"
#include "park_editor_internal.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include <atomic>

namespace dingosdk {
namespace {
std::atomic<bool> placement_allowed{true};
std::atomic<unsigned> object_limit{};
}
bool lobby_object_placement_allowed() noexcept { return placement_allowed.load(std::memory_order_acquire); }
void set_lobby_object_limit(unsigned limit) noexcept { object_limit.store(limit, std::memory_order_release); }
unsigned lobby_object_limit() noexcept { return object_limit.load(std::memory_order_acquire); }
using namespace profile_runtime;
using namespace profile_runtime::park_editor_detail;
// Not for callers that hold the native mutex: they have the layout, and count it themselves.
std::size_t lobby_object_count() {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    const auto found = r.document.maps.find(r.map);
    return found == r.document.maps.end() ? 0 : found->second.size();
}
bool lobby_object_limit_reached() {
    const auto limit = lobby_object_limit();
    return limit && lobby_object_count() >= limit;
}
void set_lobby_object_guest(bool guest) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    if (guest == r.personal_document.has_value()) return;
    auto &e = editor_state();
    // Transfer visible copies to the deletion queue; this changes no saved IDs.
    // Preview entities may not have reached rows/the committed layout yet.
    for (const auto &row : r.rows)
        retire_local_network_object(row.entity, row.object);
    if (e.transaction)
        for (const auto &step : e.transaction->steps)
            retire_local_network_object(step.entity, step.object);
    if (e.late_create)
        retire_local_network_object(e.late_create->entity, e.late_create->object);
    r.creation.reset();
    r.creates.clear(); // Reject requests queued in the previous lobby scope.
    reset_park_editor();
    r.tracked.clear();
    r.restore.clear();
    r.late_restores.clear();
    r.inflight.reset();
    r.rows.clear();
    r.teleport_token = 0;
    r.restore_failed = false;
    r.clearing = false;
    r.clear_entities.clear();
    if (guest)
        r.personal_document.emplace(std::exchange(r.document, {}));
    else {
        r.document = std::move(*r.personal_document);
        r.personal_document.reset();
    }
    queue_placement_layout(r);
    r.status = guest ? "Using the host's saved objects. Your session edits are temporary."
                     : "Restoring your saved objects.";
}
bool clear_lobby_guest_objects() {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    if (!r.personal_document) return true; // Only guests hold temporary session layouts.
    if (!local_runtime().active || !placement_session_ready()) return false;
    std::set<std::uint64_t> tokens;
    for (const auto &row : r.rows) tokens.insert(row.token);
    if (!tokens.empty() && !begin_placement_delete(tokens)) return false;
    // Anything not yet spawned has no row; drop it from the session layout too.
    r.document.maps[r.map].clear();
    r.restore.clear();
    r.late_restores.clear();
    r.status = "The host deleted all guest objects.";
    return true;
}
void set_lobby_object_placement_allowed(bool enabled) {
    std::lock_guard lock(local_runtime().native_mutex);
    placement_allowed.store(enabled, std::memory_order_release);
    if (enabled) return;
    auto &e = editor_state();
    e.selection.clear();
    e.probe.reset();
    e.surface = {};
    if (e.transaction && e.transaction->preview && !e.transaction->preview->cancelled)
        cancel_preview(*e.transaction);
}
std::optional<NetworkObjectSnapshot> capture_local_network_objects() {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &r = placements_runtime();
    auto &e = editor_state();
    if (!local_runtime().active || !placement_session_ready() || !r.restore.empty() || r.inflight ||
        r.clearing || r.restore_failed)
        return {};
    auto layout = r.document.maps[r.map];
    if (e.transaction && e.transaction->preview)
        for (const auto &step : e.transaction->steps) {
            if (!step.entity || step.kind == Kind::erase)
                continue;
            auto found = std::find_if(layout.begin(), layout.end(),
                                      [&](const auto &object) { return object.id == step.object.id; });
            if (found == layout.end())
                layout.push_back(step.object);
            else
                *found = step.object;
        }
    NetworkObjectSnapshot result;
    result.map = r.map;
    for (const auto &object : layout)
        result.objects.push_back({object.id, object.item, object.position, object.rotation, object.scale});
    return result;
}
} // namespace dingosdk
