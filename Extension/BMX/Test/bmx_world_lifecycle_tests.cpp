#include "Extension/BMX/bmx_world.h"
#include "Extension/BMX/bmx_world_transform.h"
#include "Extension/Multiplayer/Remote/native_prop.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
struct NativeFixture {
    bool release_allowed{true}, partial_failure{}, incomplete_success{};
    unsigned updates{}, releases{};
    std::uintptr_t last_owner{};
    std::uint64_t generation{1};
} fixture;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
} // namespace

// A portable lifecycle fixture replaces the target engine at its narrow native
// prop boundary. It never calls the game or claims renderer/physics coverage.
namespace dingosdk::multiplayer {
bool release_client_props(std::span<ClientPropHandle> handles) noexcept {
    ++fixture.releases;
    if (!fixture.release_allowed) return false;
    for (auto& handle : handles) handle = {};
    return true;
}
ClientPropBatchResult update_client_props(std::uintptr_t base, std::uintptr_t client,
    std::uintptr_t owner, std::span<ClientPropHandle> handles,
    std::span<const ClientPropPlacement> placements) noexcept {
    ++fixture.updates;
    fixture.last_owner = owner;
    check(base == 100 && client == 200 && handles.size() == bmx::world::part_count && placements.size() == bmx::world::part_count, "native batch shape");
    ClientPropBatchResult result;
    for (unsigned i = 0; i < handles.size(); ++i) {
        check(placements[i].blueprint == bmx::world::blueprints[i], "part blueprint order");
        if (!handles[i].entity) {
            handles[i] = {1000 + i, 300, 400, base, fixture.generation++};
            ++result.created;
        }
        ++result.placed;
        ++result.live;
        if (fixture.partial_failure) {
            result.issue = "Simulated native failure after first part.";
            return result;
        }
        if(fixture.incomplete_success) {result.ready=true;return result;}
    }
    result.ready = true;
    return result;
}
} // namespace dingosdk::multiplayer

int main() {
    using namespace dingosdk::bmx;
    visual::Frame frame;
    world::tick(100, 200, frame);
    check(!world::snapshot().active && world::snapshot().live_parts == 0, "inactive starts without native entities");
    frame.active = true;
    frame.entity = 501;
    const auto first_receipt=world::tick(100,200,frame,11);
    check(first_receipt.complete&&first_receipt.revision==11&&first_receipt.entity==501&&
        first_receipt.live==world::part_count&&first_receipt.placed==world::part_count,
        "receipt binds exact requested revision/entity to all eight completed setter commands");
    auto value = world::snapshot();
    check(value.active && value.ready && value.live_parts == world::part_count, "active bike creates all parts");
    check(value.entities_created == world::part_count && value.entity_placements == world::part_count, "native counters reflect actual fixture calls");
    world::tick(100, 200, frame);
    value = world::snapshot();
    check(value.entities_created == world::part_count && value.entity_placements == 2*world::part_count, "repeated frame reuses entities");
    const auto prior_releases = fixture.releases;
    frame.entity = 502;
    fixture.release_allowed = false;
    world::tick(100, 200, frame);
    check(fixture.updates == 2 && fixture.releases == prior_releases + 1, "owner change waits for proper realm cleanup");
    check(!world::snapshot().ready && world::snapshot().live_parts == world::part_count, "deferred cleanup retains tracked handles");
    fixture.release_allowed = true;
    world::tick(100, 200, frame);
    check(fixture.updates == 3 && fixture.last_owner == 502 && world::snapshot().entities_created == 2*world::part_count, "new owner creates after cleanup");
    frame.yaw = std::numeric_limits<float>::quiet_NaN();
    world::tick(100, 200, frame);
    check(fixture.updates == 3 && world::snapshot().live_parts == 0 && !world::snapshot().ready, "invalid frame cleans up without native placement");
    frame.yaw = 0;
    world::tick(100, 200, frame);
    check(world::snapshot().ready && fixture.updates == 4, "valid frame recreates after invalid frame");
    fixture.release_allowed = false;
    world::tick(100, 200, {});
    check(!world::snapshot().active && !world::snapshot().ready && world::snapshot().live_parts == world::part_count, "dismount on wrong realm retains cleanup only");
    fixture.release_allowed = true;
    world::tick(100, 200, {});
    check(world::snapshot().live_parts == 0, "next proper realm tick completes dismount cleanup");
    fixture.partial_failure = true;
    const auto prior_creates = world::snapshot().entities_created;
    world::tick(100, 200, frame);
    value = world::snapshot();
    check(!value.ready && value.live_parts == 0 && value.entities_created == prior_creates + 1, "partial factory failure removes all parts");
    const auto failed_updates = fixture.updates;
    world::tick(100, 200, frame);
    check(fixture.updates == failed_updates, "missing or failed asset batches throttle retries");
    fixture.partial_failure = false;
    world::tick(100, 200, {});
    world::tick(100, 200, frame);
    check(world::snapshot().ready, "explicit dismount/remount rearms retry without stale failure");
    world::tick(100, 200, {});
    fixture.incomplete_success=true;
    const auto incomplete=world::tick(100,200,frame,99);
    check(!incomplete.complete&&incomplete.revision==99&&incomplete.entity==frame.entity&&incomplete.placed==1&&
        !world::snapshot().ready&&world::snapshot().live_parts==0,
        "ready flag with fewer than eight setters cannot issue a placement receipt; partial model is removed");
    fixture.incomplete_success=false;
    const auto inactive=world::tick(100,200,{},100);
    check(!inactive.complete&&inactive.revision==100&&inactive.entity==0,"inactive cleanup never grants a rider placement receipt");
    std::cout << "BMX native world lifecycle tests passed\n";
}
