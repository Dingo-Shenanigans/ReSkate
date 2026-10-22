#include "bmx_world.h"
#include "bmx_world_transform.h"
#ifdef _WIN32
#include "bmx_prop_transform_trace.h"
#endif
#include "Extension/Multiplayer/Remote/native_prop.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

namespace dingosdk::bmx::world {
namespace {
struct State {
    std::array<multiplayer::ClientPropHandle, part_count> handles{}; // client thread only
    std::uintptr_t owner{};
    std::chrono::steady_clock::time_point next_attempt{};
    std::mutex mutex;
    Snapshot report;
};
State& state() { static auto* value = new State; return *value; }
unsigned handles_present(const State& value) {
    return static_cast<unsigned>(std::count_if(value.handles.begin(), value.handles.end(),
        [](const auto& handle) { return handle.entity != 0; }));
}
void report(State& value, bool active, bool ready, const multiplayer::ClientPropBatchResult& result,
    std::string issue) {
    std::lock_guard lock(value.mutex);
    value.report.active = active;
    value.report.ready = ready;
    value.report.live_parts = ready ? result.live : handles_present(value);
    value.report.entities_created += result.created;
    value.report.entity_placements += result.placed;
    value.report.issue = std::move(issue);
}
} // namespace

PlacementReceipt tick(std::uintptr_t base, std::uintptr_t client, const visual::Frame& frame, std::uint64_t revision) noexcept {
    auto& value = state();PlacementReceipt receipt;receipt.revision=revision;receipt.entity=frame.entity;
    try {
        if (!frame.active || !frame.entity || !base || !client) {
#ifdef _WIN32
            prop_transform_trace::poll_export(true);
#endif
            const auto removed = multiplayer::release_client_props(value.handles);
            if (removed) { value.owner = 0; value.next_attempt = {}; }
            report(value, false, false, {}, removed ? "Inactive." : "Waiting for native world cleanup.");
            return receipt;
        }
        if (value.owner && value.owner != frame.entity) {
            if (!multiplayer::release_client_props(value.handles)) {
                report(value, true, false, {}, "Waiting for previous rider model cleanup.");
                return receipt;
            }
            value.owner = 0;
            value.next_attempt = {};
        }
        std::array<Matrix, part_count> matrices{};
        if (!transforms(frame, matrices)) {
            (void)multiplayer::release_client_props(value.handles);
            report(value, true, false, {}, "Invalid bike world transform.");
            return receipt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!handles_present(value) && now < value.next_attempt) return receipt;
        std::array<multiplayer::ClientPropPlacement, part_count> placements{};
        for (unsigned part = 0; part < placements.size(); ++part)
            placements[part] = {blueprints[part], matrices[part]};
#ifdef _WIN32
        auto* measured=prop_transform_trace::begin(base,client,frame,revision);
        if(measured)for(unsigned part=0;part<placements.size();++part)placements[part].readback=&measured->parts_readback[part];
#endif
        const auto result = multiplayer::update_client_props(base, client, frame.entity, value.handles, placements);
#ifdef _WIN32
        prop_transform_trace::finish(measured,result);
#endif
        value.owner = frame.entity;receipt.live=result.live;receipt.placed=result.placed;
        receipt.complete=result.ready&&result.live==part_count&&result.placed==part_count;
        if (!receipt.complete) {
            (void)multiplayer::release_client_props(value.handles);
            value.next_attempt = now + std::chrono::seconds(1);
        }
        report(value, true, receipt.complete, result, receipt.complete ? "Native bike entities placed." :
            result.issue.empty()?"Incomplete native bike placement batch.":result.issue);
    } catch (...) {
        receipt.complete=false;
        (void)multiplayer::release_client_props(value.handles);
        try { report(value, frame.active, false, {}, "Native bike model update failed."); } catch (...) {}
    }
    return receipt;
}

Snapshot snapshot() {
    auto& value = state();
    std::lock_guard lock(value.mutex);
    return value.report;
}
} // namespace dingosdk::bmx::world
