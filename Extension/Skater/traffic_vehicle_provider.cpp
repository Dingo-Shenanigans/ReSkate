#include "traffic_vehicle_provider.h"
#include "traffic_vehicle_surface.h"
#include "traffic_vehicle_surface_math.h"
#include "client_source_spawn_internal.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/World/local_park_rotation.h"
#include "Extension/World/local_world_layers.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/population.h"
#include "Engine/Game/Build/20260929/park_rotation.h"
#include "Engine/Game/Build/20260929/world_layers.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "car_grab/lifecycle_journal.h"
#include "car_grab/vehicle_samples.h"
#include "car_grab/contact_motion.h"
#include <limits>
#include <map>
#include <set>

namespace dingosdk::car_grab_native {
namespace {
using namespace client_source::detail;
constexpr std::uintptr_t removal_rva = 0x329f640;
constexpr std::array<unsigned char, 32> removal_bytes{
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x48,0x8d,0x6c,0x24,0xa9,
    0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x48,0x8b,0x05,0x62,0x4d,0xf2,0x03,0x48,0x33};
constexpr ULONGLONG lease_ms = 100;
struct ClientGate {
    std::uintptr_t base{}, client{}, context{};
    ULONGLONG expires{};
    bool ready{};
};
struct Provider {
    std::mutex mutex; // Only immutable value publication; never held across native calls.
    ClientGate client;
    TrafficSnapshot published;
    // Protected by mutex; invalidated by exact epoch, never used for behavior.
    SurfaceProbeReport last_probe;
    std::uint64_t last_probe_vehicle{}, last_probe_epoch{};
    double last_probe_at{};
    std::atomic<std::uint64_t> epoch{1}, published_epoch{}, root_changes{};
    std::atomic<std::uintptr_t> selected_root{}, selected_population{};
    std::atomic<std::uint64_t> selected_vehicle{};
    std::atomic<unsigned> active_removals{};
    std::atomic<bool> initialized{}, exhausted{};
    void (*remove)(std::uintptr_t, std::uint64_t){};
    car_grab::LifecycleJournal removals;
    // Only the native park phase, already serialized by native_mutex, touches these.
    car_grab::VehicleSampler sampler{[] {
        car_grab::VehicleSamplingConfig c;
        c.max_observations = 1000;
        return c;
    }()};
    TrafficScope previous;
    std::uint64_t sampled_epoch{}, removal_cursor{};
    struct SurfaceTrack {
        std::uint64_t generation{}, context{}, record{}, serial{};
        car_grab::Vec3 local_forward{};
        NativeGripSurface surface;
        car_grab::ContactMotion motion;
        car_grab::VehicleGrip cached;
        double next_probe{};
    };
    std::map<std::uint64_t, SurfaceTrack> surfaces;
    std::uint64_t next_surface{1};
};
Provider& provider() { static auto* p = new Provider; return *p; }
double now_seconds() noexcept {
    LARGE_INTEGER frequency{}, counter{};
    return QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
        QueryPerformanceCounter(&counter) && counter.QuadPart > 0
        ? static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart) : 0.0;
}
std::uint64_t increment(std::atomic<std::uint64_t>& counter) noexcept {
    auto before = counter.load(std::memory_order_acquire);
    while (before != UINT64_MAX) {
        if (counter.compare_exchange_weak(before, before + 1, std::memory_order_acq_rel)) return before + 1;
    }
    provider().exhausted.store(true, std::memory_order_release);
    return 0;
}
bool epoch_current(std::uint64_t epoch) noexcept {
    auto& p = provider();
    return epoch && !p.exhausted.load(std::memory_order_acquire) &&
        p.initialized.load(std::memory_order_acquire) &&
        p.epoch.load(std::memory_order_acquire) == epoch;
}
ClientGate client_gate() {
    auto& p = provider();
    std::lock_guard lock(p.mutex);
    return p.client;
}
bool gate_live(const ClientGate& gate) noexcept {
    return gate.ready && gate.base && source_object(gate.client) && source_object(gate.context) &&
        GetTickCount64() < gate.expires;
}
void remove_hook(std::uintptr_t manager, std::uint64_t id) {
    // The native function is void(manager, full entity value). No allocation or
    // observer mutex surrounds it, and its incoming/returned LastError is preserved.
    const auto incoming = GetLastError();
    auto& p = provider();
    p.active_removals.fetch_add(1, std::memory_order_acq_rel);
    p.removals.append(id);
    SetLastError(incoming);
    p.remove(manager, id);
    const auto returned = GetLastError();
    p.removals.append(id);
    p.active_removals.fetch_sub(1, std::memory_order_acq_rel);
    SetLastError(returned);
}
void read_scope(SourceReader& reader, TrafficScope& s, const ClientGate& gate, std::uintptr_t park) {
    source_require(gate_live(gate), "Car Grab client lease unavailable.");
    s.base = gate.base; s.client = gate.client; s.client_context = gate.context; s.park = park;
    source_require(reader.pointer(s.client) == s.base + addr::engine::client_vtable &&
        reader.pointer(s.client, 8) == s.client_context, "Car Grab client identity changed.");
    const auto state = reader.value<DWORD>(s.client, 0xc4);
    source_require((state == 13 || state == 21) && reader.value<DWORD>(s.client, 0xc0) <= 1,
        "Car Grab client state unavailable.");
    s.server = reader.pointer(s.base + addr::engine::game_server);
    source_require(source_object(s.server) && reader.pointer(s.server) == s.base + addr::engine::server_vtable,
        "Car Grab owned server unavailable.");
    const auto controller = reader.pointer(s.client, 0xd0);
    source_require(source_object(controller) && reader.pointer(controller) == s.base + addr::engine::controller_vtable &&
        reader.pointer(controller, 0x4c0) == s.server, "Car Grab server is not owned by this client.");
    s.server_context = reader.pointer(s.server, 8);
    source_require(source_object(s.server_context) &&
        reader.pointer(s.base + addr::remote_collision::game_context_global) == s.client_context &&
        reader.pointer(s.base + addr::remote_collision::game_context_global + 8) == s.server_context,
        "Car Grab realm contexts unavailable.");
    source_require(source_object(s.park) && reader.pointer(s.base + addr::park_rotation::manager) == s.park &&
        reader.pointer(s.park) == s.base + addr::park_rotation::manager_vtable, "Car Grab park owner changed.");
    s.park_reference = reader.pointer(s.park, 8);
    const auto offset = reader.value<std::uint32_t>(s.base + addr::park_rotation::context_offset);
    source_require(source_object(s.park_reference) && s.park_reference == s.server_context && offset <= 0x1000000 &&
        source_range(s.park_reference, static_cast<std::size_t>(offset) + 0x29), "Car Grab park context rejected.");
    s.park_context = s.park_reference + offset;
    source_require(reader.value<std::uint8_t>(s.park_context, 0x28) == 1, "Car Grab park context is not live.");
    s.traffic = reader.pointer(s.base + addr::population::managers + 8); // Authoritative realm1 only.
    source_require(source_object(s.traffic) && reader.pointer(s.traffic) == s.base + addr::population::manager_vtable &&
        reader.value<int>(s.traffic, 8) == 1 && reader.value<std::uint8_t>(s.traffic, 0x152) == 1 &&
        reader.value<std::uint8_t>(s.traffic, 0x150) == 1, "Car Grab traffic manager unavailable.");
}
std::vector<std::uint64_t> read_table(SourceReader& reader, std::uintptr_t manager) {
    const auto begin = reader.pointer(manager, 0xa8), end = reader.pointer(manager, 0xb0);
    source_require(end >= begin && end - begin <= 8000 && begin % 8 == 0 && end % 8 == 0 &&
        (end == begin || source_range(begin, static_cast<std::size_t>(end - begin))), "Car Grab traffic table rejected.");
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>((end - begin) / 8));
    std::set<std::uint64_t> unique;
    for (auto at = begin; at < end; at += 8) {
        const auto id = reader.value<std::uint64_t>(at);
        source_require((id >> 32) && unique.insert(id).second, "Car Grab traffic table identity rejected.");
        ids.push_back(id);
    }
    return ids;
}
void read_root(SourceReader& reader, const TrafficScope& s) {
    source_require(source_object(s.root) && reader.pointer(s.root) == s.base + addr::world_layers::subworld_reference_vtable &&
        reader.pointer(s.root, 0x38) == s.root_data && reader.pointer(s.root, 0x30) == s.root_owner &&
        reader.pointer(s.root_owner, 0x20) == s.park_reference && reader.pointer(s.root, 0x80) != 0 &&
        (reader.value<std::uint8_t>(s.root, 0x148) & 1) != 0, "Car Grab world root unavailable.");
}
// POD-only SEH boundary: native queries are unchecked inside the retail engine.
// This does not replace owner-phase/lifecycle validation or call from physics.
bool query_pose(std::uint64_t id, VehicleIdentity& identity, std::array<float, 12>& pose) noexcept {
    auto& r = profile_runtime::placements_runtime();
    alignas(16) std::uint64_t reference[2]{id, 0}, query[3]{}, after[3]{};
    alignas(16) float transform[12]{};
    std::uint32_t record_before{}, record_after{};
    __try {
        if (!r.valid(reference) || r.query(query, id) != query || query[0] != id ||
            !source_object(query[1]) || !source_object(query[2]) ||
            !memory::peek(query[2], record_before) || record_before != static_cast<std::uint32_t>(id) ||
            r.pose(transform, query) != transform || !r.valid(reference) || r.query(after, id) != after ||
            std::memcmp(query, after, sizeof(query)) != 0 ||
            !memory::peek(after[2], record_after) || record_after != record_before) return false;
        identity = {id, query[1], query[2]};
        std::memcpy(pose.data(), transform, sizeof(transform));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool valid_pose(const std::array<float, 12>& pose) noexcept {
    for (const unsigned lane : {0u,1u,2u,4u,5u,6u,7u,8u,9u,10u})
        if (!std::isfinite(pose[lane])) return false; // Padding3/11 are native markers, not numbers.
    for (unsigned i = 0; i < 3; ++i)
        if (pose[i] <= 0 || pose[i] > 100 || std::abs(pose[8+i]) > 100000) return false;
    double norm{};
    for (unsigned i = 4; i < 8; ++i) norm += static_cast<double>(pose[i]) * pose[i];
    if (norm < .98 || norm > 1.02) return false;
    // pose() returns this default when a valid entity has no transform component.
    return !(pose[0] == 1 && pose[1] == 1 && pose[2] == 1 && pose[4] == 0 && pose[5] == 0 &&
        pose[6] == 0 && pose[7] == 1 && pose[8] == 0 && pose[9] == 0 && pose[10] == 0);
}
car_grab::Vec3 pose_forward(const std::array<float,12>& pose) noexcept {
    surface_math::Transform transform;
    if (!valid_pose(pose) || !surface_math::transform(pose,transform)) return {};
    // Exact-image traffic spawn aligns local +Z with its road path. This is
    // an orientation policy, not a bumper extent or collision-owner inference.
    // A vertical/invalid projection leaves observed motion as the fallback.
    const auto axis = surface_math::rotate({0,0,1},transform.rotation);
    return surface_math::unit({axis.x,0,axis.z});
}
void failed(TrafficStatus status, const char* reason = nullptr) noexcept {
    auto& p = provider();
    invalidate();
    p.sampler.reset();
    // The park hook owns this phase. Record its actual gate failure so user
    // reports can distinguish missing traffic from unsafe world ownership.
    static ULONGLONG last_report{};
    if (reason && GetTickCount64() - last_report >= 2000) {
        logging::printf(logging::Level::info, logging::Channel::skater, "Car Grab traffic gated: %s", reason);
        last_report = GetTickCount64();
    }
    try {
        std::lock_guard lock(p.mutex);
        p.published = {}; p.published.status = status;
    } catch (...) {}
}
}

bool world_current(std::uint64_t epoch) noexcept { return epoch_current(epoch); }
bool retained_identity_current(const TrafficSnapshot& sample, std::uint64_t selected,
                               bool allow_inflight_removal) noexcept {
    // Only retain a private, bounded suspended lock. There are no native reads,
    // and deadline-independent value checks never grant write or hand authority.
    auto& p = provider();
    try {
        const auto active_removals = p.active_removals.load(std::memory_order_acquire);
        if (!selected || sample.status != TrafficStatus::ready || !epoch_current(sample.epoch) ||
            p.published_epoch.load(std::memory_order_acquire) != sample.epoch ||
            (!allow_inflight_removal && active_removals)) return false;
        const auto old_vehicle = std::find_if(sample.vehicles.begin(), sample.vehicles.end(),
            [&](const auto& value) { return value.id == selected; });
        const auto old_identity = std::find_if(sample.identities.begin(), sample.identities.end(),
            [&](const auto& value) { return value.value == selected; });
        if (old_vehicle == sample.vehicles.end() || !old_vehicle->generation ||
            old_identity == sample.identities.end() || old_identity->generation != old_vehicle->generation ||
            !source_object(old_identity->context) || !source_object(old_identity->record)) return false;
        {
            std::lock_guard lock(p.mutex);
            const auto latest = std::find_if(p.published.identities.begin(), p.published.identities.end(),
                [&](const auto& value) { return value.value == selected; });
            if (!p.client.ready || p.client.base != sample.scope.base || p.client.client != sample.scope.client ||
                p.client.context != sample.scope.client_context ||
                (p.published.status != TrafficStatus::ready && p.published.status != TrafficStatus::warming) ||
                p.published.epoch != sample.epoch || p.published.scope != sample.scope ||
                latest == p.published.identities.end() || latest->generation != old_vehicle->generation ||
                latest->context != old_identity->context || latest->record != old_identity->record) return false;
        }
        const auto events = p.removals.read_since(sample.removals);
        return events.status == car_grab::LifecycleReadStatus::complete &&
            std::find(events.ids.begin(), events.ids.end(), selected) == events.ids.end() &&
            epoch_current(sample.epoch) && p.published_epoch.load(std::memory_order_acquire) == sample.epoch &&
            p.active_removals.load(std::memory_order_acquire) == active_removals &&
            p.removals.current_sequence() == events.through;
    } catch (...) { return false; }
}
bool grip_current(std::uint64_t epoch, std::uint64_t vehicle, std::uint64_t surface) noexcept {
    auto& p = provider();
    try {
        if (!epoch_current(epoch) || !vehicle || !surface || p.active_removals.load(std::memory_order_acquire)) return false;
        std::unique_lock lock(p.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        const auto& current = p.published;
        const auto time = now_seconds();
        const auto age = time - current.sampled_at;
        if (current.status != TrafficStatus::ready || current.epoch != epoch || age < 0 || age > .1 ||
            p.published_epoch.load(std::memory_order_acquire) != epoch) return false;
        // An unrelated completed destruction is not this contact's lifetime.
        // Selected events, pending/overflowed history and in-flight destruction
        // remain denied; the final sequence check closes the observation race.
        const auto events = p.removals.read_since(current.removals);
        if (events.status != car_grab::LifecycleReadStatus::complete ||
            std::find(events.ids.begin(), events.ids.end(), vehicle) != events.ids.end()) return false;
        for (const auto& candidate : current.vehicles) if (candidate.id == vehicle)
            return candidate.grip.valid && candidate.grip.surface_id == surface &&
                time >= candidate.grip.sampled_at && time - candidate.grip.sampled_at <= .15 && epoch_current(epoch) &&
                p.published_epoch.load(std::memory_order_acquire) == epoch &&
                p.active_removals.load(std::memory_order_acquire) == 0 &&
                p.removals.current_sequence() == events.through;
        return false;
    } catch (...) { return false; }
}
void invalidate() noexcept {
    auto& p = provider();
    p.published_epoch.store(0, std::memory_order_release);
    increment(p.epoch);
}
void invalidate_root(std::uintptr_t entity) noexcept {
    auto& p = provider();
    increment(p.root_changes);
    if (!p.selected_root.load(std::memory_order_acquire) || p.selected_root.load(std::memory_order_acquire) == entity)
        invalidate();
}
void invalidate_population(std::uintptr_t manager) noexcept {
    auto& p = provider();
    if (!p.selected_population.load(std::memory_order_acquire) || p.selected_population.load(std::memory_order_acquire) == manager)
        invalidate();
}
void start(std::uintptr_t base) noexcept {
    SourceLastError preserve;
    auto& p = provider();
    try {
        auto& r = profile_runtime::placements_runtime();
        for (const auto* contract : {&addr::profile::placement_valid_contract, &addr::profile::placement_query_contract,
                &addr::profile::placement_pose_contract})
            source_require(profile_runtime::matches(base, *contract), "Car Grab API fingerprint mismatch.");
        source_require(r.valid && r.query && r.pose, "Car Grab native API is not initialized.");
        surface_start(base);
        std::array<unsigned char, 32> bytes{};
        source_require(memory::read(base + removal_rva, bytes) && bytes == removal_bytes,
            "Car Grab traffic removal fingerprint mismatch.");
        void* target = reinterpret_cast<void*>(base + removal_rva);
        source_require(hook_prepare(target, reinterpret_cast<void*>(&remove_hook), reinterpret_cast<void**>(&p.remove)) == HookOk,
            "Car Grab removal observer preparation failed.");
        if (hook_enable(target) != HookOk) {
            hook_remove(target);
            source_require(false, "Car Grab removal observer attachment failed.");
        }
        p.initialized.store(true, std::memory_order_release);
        logging::write(logging::Level::info, logging::Channel::skater,
            "Car Grab native traffic adapter enabled; waiting for an owned local world and fresh traffic poses.");
    } catch (...) {
        p.initialized.store(false, std::memory_order_release);
        invalidate();
        logging::write(logging::Level::warning, logging::Channel::skater, "Car Grab vehicle adapter unavailable; no towing writes enabled.");
    }
}
void publish_client(std::uintptr_t base, std::uintptr_t client, std::uintptr_t context, bool ready) noexcept {
    SourceLastError preserve;
    auto& p = provider();
    try {
        std::lock_guard lock(p.mutex);
        const auto& old = p.client;
        // A deadline denies writes; it is not evidence of a different world.
        // Actual scope/readiness transitions still invalidate every incarnation.
        if (old.base != base || old.client != client || old.context != context || old.ready != ready) invalidate();
        p.client = {base, client, context, ready ? GetTickCount64() + lease_ms : 0, ready};
    } catch (...) { invalidate(); }
}
void capture(std::uintptr_t base, std::uintptr_t park) noexcept {
    SourceLastError preserve;
    auto& p = provider();
    if (!p.initialized.load(std::memory_order_acquire)) return;
    try {
        const auto gate = client_gate();
        if (!gate.ready || gate.base != base || !source_object(gate.client) || !source_object(gate.context)) {
            failed(TrafficStatus::unavailable); return;
        }
        if (!gate_live(gate)) return; // No publication or identity reset on an expired authorization.
        const auto now = now_seconds(); // Includes the entire capture/validation cost in freshness.
        auto& layers = profile_runtime::world_layers_runtime();
        source_require(profile_runtime::local_runtime().active.load(std::memory_order_acquire) &&
            profile_runtime::park_runtime().active.load(std::memory_order_acquire) &&
            layers.active.load(std::memory_order_acquire), "Car Grab native owner phase unavailable.");
        const auto roots_before = p.root_changes.load(std::memory_order_acquire);
        std::lock_guard lifetime(layers.lifetime_mutex);
        source_require(layers.model.ready && layers.model.map != WorldMap::none && layers.root_generation,
            "Car Grab world is not ready.");
        SourceReader reader;
        TrafficScope scope;
        read_scope(reader, scope, gate, park);
        source_require(layers.context == scope.park_reference, "Car Grab world owner changed.");
        for (const auto& [entity, node] : layers.nodes) {
            if (node.context != layers.context || node.generation != layers.root_generation) continue;
            source_require(!scope.root, "Car Grab world root is ambiguous.");
            scope.root = entity; scope.root_data = node.data;
            scope.root_owner = reader.pointer(entity, 0x30);
        }
        scope.root_generation = layers.root_generation; scope.map = static_cast<int>(layers.model.map);
        read_root(reader, scope);
        p.selected_root.store(scope.root, std::memory_order_release);
        p.selected_population.store(scope.traffic, std::memory_order_release);
        if (p.previous != scope) { invalidate(); p.previous = scope; }
        const auto epoch = p.epoch.load(std::memory_order_acquire);
        if (p.sampled_epoch != epoch) { p.sampler.reset(); p.surfaces.clear(); p.sampled_epoch = epoch; }
        const auto events = p.removals.read_since(p.removal_cursor);
        if (events.status == car_grab::LifecycleReadStatus::overflow) {
            p.removal_cursor = events.through;
            failed(TrafficStatus::incomplete, "Removal journal overflow; warming a new traffic snapshot."); return;
        }
        source_require(events.status == car_grab::LifecycleReadStatus::complete &&
            p.active_removals.load(std::memory_order_acquire) == 0, "Car Grab removal observation incomplete.");
        for (auto id : events.ids) { p.sampler.forget(id); p.surfaces.erase(id); }
        p.removal_cursor = events.through;
        // The pinned park callback and exact owned server/park/realm/root checks
        // establish this phase. Raw query uses the global namespace registry;
        // it does not need the asserting TLS-context helper or a TLS switch.
        const auto ids = read_table(reader, scope.traffic);
        std::vector<car_grab::VehicleObservation> observations;
        std::vector<VehicleIdentity> identities;
        std::map<std::uint64_t, std::array<float, 12>> poses;
        observations.reserve(ids.size()); identities.reserve(ids.size());
        for (auto id : ids) {
            VehicleIdentity identity;
            std::array<float, 12> pose{};
            if (!query_pose(id, identity, pose) || !valid_pose(pose)) continue;
            source_require(reader.value<std::uint32_t>(identity.context, 8) == static_cast<std::uint32_t>(id >> 32) &&
                reader.value<std::uint32_t>(identity.record) == static_cast<std::uint32_t>(id),
                "Car Grab query incarnation changed during capture.");
            observations.push_back({id, identity.context, identity.record, {pose[8], pose[9], pose[10]}, pose_forward(pose)});
            identities.push_back(identity);
            poses.emplace(id, pose);
        }
        source_require(read_table(reader, scope.traffic) == ids, "Car Grab traffic table changed during capture.");
        reader.verify();
        const auto current_gate = client_gate();
        source_require(epoch_current(epoch) && current_gate.ready && current_gate.base == gate.base &&
            current_gate.client == gate.client && current_gate.context == gate.context &&
            p.root_changes.load(std::memory_order_acquire) == roots_before &&
            p.removals.current_sequence() == events.through && p.active_removals.load(std::memory_order_acquire) == 0,
            "Car Grab native lifetime changed during capture.");
        const auto copied_at = now_seconds();
        source_require(std::isfinite(now) && now > 0 && std::isfinite(copied_at) && copied_at >= now,
            "Car Grab capture clock is invalid.");
        if (!gate_live(current_gate) || copied_at - now > .1) return;
        auto motion = p.sampler.sample({epoch, now, observations, true});
        if (motion.status == car_grab::VehicleSampleStatus::invalid ||
            motion.status == car_grab::VehicleSampleStatus::incomplete) {
            failed(TrafficStatus::incomplete, "Motion sampling rejected an incomplete or invalid capture."); return;
        }
        for (auto& identity : identities) {
            const auto incarnation = std::find_if(motion.incarnations.begin(), motion.incarnations.end(),
                [&](const auto& value) { return value.id == identity.value; });
            source_require(incarnation != motion.incarnations.end() && incarnation->generation &&
                incarnation->context == identity.context && incarnation->record == identity.record,
                "Car Grab copied incarnation is unavailable.");
            identity.generation = incarnation->generation;
        }
        TrafficDiagnostics diagnostic;
        diagnostic.observed = static_cast<std::uint32_t>(ids.size());
        diagnostic.posed = static_cast<std::uint32_t>(observations.size());
        diagnostic.sampled = static_cast<std::uint32_t>(motion.vehicles.size());
        // All collision API calls remain in the native park phase. The chosen
        // point is frozen in the car's local coordinates, never recentered on
        // a new camera/steering position. Only copied values reach physics.
        overlay::DebugModel local_skater;
        bool have_skater{};
        try {
            (void)debug_skater(base, scope.client, local_skater);
            have_skater = local_skater.skater_position_valid;
            diagnostic.skater_status = have_skater ? SkaterCaptureStatus::ready : SkaterCaptureStatus::invalid_position;
        }
        catch (...) {}
        const car_grab::Vec3 skater_position{local_skater.skater_position[0],
            local_skater.skater_position[1], local_skater.skater_position[2]};
        if (have_skater && !surface_math::finite(skater_position)) {
            have_skater = false; diagnostic.skater_status = SkaterCaptureStatus::invalid_position;
        }
        diagnostic.skater_valid = have_skater;
        if (have_skater) diagnostic.skater_position = skater_position;
        if (have_skater) {
            const auto nearest = [&](const auto& candidates, NearbyCandidateDiagnostic& out) {
                float smallest = std::numeric_limits<float>::infinity();
                for (const auto& candidate : candidates) {
                    const auto delta = surface_math::sub(candidate.position, skater_position);
                    const auto distance = surface_math::dot(delta, delta);
                    if (!candidate.id || !std::isfinite(distance) || distance < 0 || distance >= smallest) continue;
                    smallest = distance;
                    out = {true, candidate.id, candidate.position,
                        std::hypot(delta.x, delta.z), std::abs(delta.y)};
                }
            };
            nearest(observations, diagnostic.nearest_posed);
            nearest(motion.vehicles, diagnostic.nearest_sampled);
        }
        std::vector<std::pair<float, std::uint64_t>> nearby;
        if (have_skater) for (const auto& vehicle : motion.vehicles) {
            const auto delta = surface_math::sub(vehicle.position, skater_position);
            const auto distance = surface_math::dot(delta, delta);
            if (std::isfinite(distance) && distance <= 14.f*14.f)
                nearby.emplace_back(distance, vehicle.id);
        }
        const auto selected_hint = p.selected_vehicle.load(std::memory_order_acquire);
        std::sort(nearby.begin(), nearby.end(), [selected_hint](const auto& a, const auto& b) {
            if ((a.second == selected_hint) != (b.second == selected_hint)) return a.second == selected_hint;
            return a.first < b.first;
        });
        if (nearby.size() > 3) nearby.resize(3);
        diagnostic.nearby = static_cast<std::uint32_t>(nearby.size());
        for (auto it = p.surfaces.begin(); it != p.surfaces.end();) {
            if (!poses.contains(it->first) || (have_skater && std::none_of(nearby.begin(), nearby.end(),
                [&](const auto& v) { return v.second == it->first; }))) it = p.surfaces.erase(it);
            else ++it;
        }
        const auto update_contact = [&](car_grab::Vehicle& vehicle, Provider::SurfaceTrack& track,
                                        const surface_math::Transform& transform) {
            const auto point = surface_math::world_point(track.surface.localpoint, transform);
            const auto contact = track.motion.sample(now, point);
            if (!contact.valid) return;
            if (contact.sampled_at != now) {
                // A retained point/velocity pair keeps its original timestamp
                // and orientation; do not relabel it as this fresh observation.
                if (track.cached.valid && track.cached.sampled_at == contact.sampled_at) vehicle.grip = track.cached;
                return;
            }
            vehicle.grip = {true, contact.point,
                surface_math::world_normal(track.surface.localnormal, transform),
                surface_math::world_tangent(track.surface.localtangent, transform),
                contact.velocity, contact.sampled_at, track.serial};
            if (track.surface.wrap_valid) {
                vehicle.grip.wrap_valid = true;
                vehicle.grip.top_point = surface_math::world_point(track.surface.localtop_point, transform);
                vehicle.grip.bottom_point = surface_math::world_point(track.surface.localbottom_point, transform);
                vehicle.grip.top_normal = surface_math::world_normal(track.surface.localtop_normal, transform);
                vehicle.grip.bottom_normal = surface_math::world_normal(track.surface.localbottom_normal, transform);
            }
            track.cached = vehicle.grip;
        };
        // Refresh already owned contacts before publishing traffic. An early
        // pose publication must not transiently clear a valid measured tether.
        for (auto& vehicle : motion.vehicles) {
            const bool is_nearby = std::any_of(nearby.begin(), nearby.end(), [&](const auto& v) { return v.second == vehicle.id; });
            if (!is_nearby && (have_skater || !p.surfaces.contains(vehicle.id))) continue;
            const auto identity = std::find_if(identities.begin(), identities.end(),
                [&](const auto& i) { return i.value == vehicle.id; });
            const auto pose = poses.find(vehicle.id);
            surface_math::Transform transform;
            if (identity == identities.end() || pose == poses.end() || !surface_math::transform(pose->second, transform)) continue;
            auto& track = p.surfaces[vehicle.id];
            if (track.generation != vehicle.generation || track.context != identity->context || track.record != identity->record) {
                track = {}; track.generation = vehicle.generation;
                track.context = identity->context; track.record = identity->record;
                track.local_forward = surface_math::unit(surface_math::rotate(vehicle.forward, transform.inverse));
            }
            if (track.surface.valid && !surface_current(scope.client_context, vehicle.id, track.surface)) {
                track.surface = {}; track.motion.reset(); track.serial = 0; track.cached = {}; track.next_probe = 0;
            }
            if (!track.surface.valid) continue;
            ++diagnostic.surfaces;
            update_contact(vehicle, track, transform);
        }
        diagnostic.grips = static_cast<std::uint32_t>(std::count_if(motion.vehicles.begin(), motion.vehicles.end(),
            [](const auto& vehicle) { return vehicle.grip.valid; }));
        const auto validate_lifetime = [&] {
            reader.verify();
            const auto latest_gate = client_gate();
            source_require(epoch_current(epoch) && latest_gate.ready && latest_gate.base == gate.base &&
                latest_gate.client == gate.client && latest_gate.context == gate.context &&
                p.root_changes.load(std::memory_order_acquire) == roots_before &&
                p.removals.current_sequence() == events.through && p.active_removals.load(std::memory_order_acquire) == 0,
                "Car Grab native lifetime changed during surface capture.");
            return gate_live(latest_gate);
        };
        const auto capture_fresh = [now](double at) noexcept {
            return std::isfinite(at) && at >= now && at - now <= .1;
        };
        const auto copy_last_probe = [&](double at) {
            // Called only while the publication mutex is held.
            if (p.last_probe_epoch != epoch) {
                p.last_probe = {}; p.last_probe_vehicle = p.last_probe_epoch = 0; p.last_probe_at = 0;
            }
            diagnostic.last_probe = p.last_probe;
            diagnostic.last_probe_vehicle = p.last_probe_vehicle;
            diagnostic.last_probe_at = p.last_probe_at;
            diagnostic.last_probe_age_seconds = p.last_probe_vehicle && std::isfinite(at) &&
                std::isfinite(p.last_probe_at) && at >= p.last_probe_at ? at - p.last_probe_at : 0;
        };
        const bool core_gate_live = validate_lifetime();
        const auto core_published_at = now_seconds();
        source_require(std::isfinite(core_published_at) && core_published_at >= now,
            "Car Grab capture clock changed before publication.");
        if (!core_gate_live || !capture_fresh(core_published_at)) return;
        diagnostic.core_publish_age_seconds = core_published_at - now;
        TrafficSnapshot next;
        next.status = motion.status == car_grab::VehicleSampleStatus::ready ? TrafficStatus::ready :
            motion.status == car_grab::VehicleSampleStatus::warming ? TrafficStatus::warming : TrafficStatus::incomplete;
        next.epoch = epoch; next.removals = events.through; next.sampled_at = now; next.scope = scope;
        next.diagnostic = diagnostic;
        next.vehicles = std::move(motion.vehicles); next.identities = std::move(identities);
        {
            std::lock_guard lock(p.mutex);
            if (!epoch_current(epoch) || !gate_live(p.client)) return;
            source_require(p.root_changes.load(std::memory_order_acquire) == roots_before &&
                p.removals.current_sequence() == events.through && p.active_removals.load(std::memory_order_acquire) == 0,
                "Car Grab native lifetime changed before core publication.");
            const auto publish_at = now_seconds();
            source_require(std::isfinite(publish_at) && publish_at >= now, "Car Grab publication clock is invalid.");
            if (!capture_fresh(publish_at)) return;
            diagnostic.core_publish_age_seconds = publish_at - now;
            copy_last_probe(publish_at);
            next.diagnostic = diagnostic;
            p.published = next;
            p.published_epoch.store(epoch, std::memory_order_release);
        }
        // New surface discovery is optional and synchronous. Start at most one
        // bounded probe while the core capture is young, then cool down the
        // exact incarnation even if no owned contact was found. A native ray
        // can still exceed this budget; its delay never forges a newer pose.
        const auto discovery_started = now_seconds();
        bool attempted{};
        std::uint64_t discovered_vehicle{};
        for (const auto& candidate : nearby) {
            const auto vehicle = std::find_if(next.vehicles.begin(), next.vehicles.end(),
                [&](const auto& value) { return value.id == candidate.second; });
            const auto track_entry = p.surfaces.find(candidate.second);
            const auto pose = poses.find(candidate.second);
            if (vehicle == next.vehicles.end() || track_entry == p.surfaces.end() || pose == poses.end()) continue;
            auto& track = track_entry->second;
            if (track.surface.valid) continue;
            if (attempted || !std::isfinite(discovery_started) || discovery_started < now ||
                discovery_started - now > .025 || discovery_started < track.next_probe) {
                ++diagnostic.discovery_deferred; continue;
            }
            surface_math::Transform transform;
            if (!surface_math::transform(pose->second, transform)) continue;
            const auto forward = surface_math::unit(surface_math::rotate(track.local_forward, transform.rotation));
            track.next_probe = discovery_started + .5;
            attempted = true;
            SurfaceProbeReport report;
            track.surface = probe_surface(scope.client_context, vehicle->id, pose->second, forward, skater_position,
                &report, local_skater.skater_identity);
            const auto probed_at = now_seconds();
            if (std::isfinite(probed_at) && probed_at >= discovery_started) {
                std::lock_guard lock(p.mutex);
                if (epoch_current(epoch) && p.published.epoch == epoch && p.published.scope == scope) {
                    p.last_probe = report; p.last_probe_vehicle = vehicle->id;
                    p.last_probe_epoch = epoch; p.last_probe_at = probed_at;
                }
            }
            ++diagnostic.probes;
            diagnostic.ray_calls += report.ray_calls;
            diagnostic.closest_hits += report.closest_hits;
            diagnostic.owner_rejects += report.owner_rejects;
            diagnostic.no_world += report.stage == SurfaceProbeStage::no_world;
            diagnostic.no_owned_rear += report.stage == SurfaceProbeStage::no_owned_rear;
            diagnostic.neighborhood += report.stage == SurfaceProbeStage::neighborhood;
            diagnostic.footprint += report.stage == SurfaceProbeStage::footprint;
            diagnostic.probe = report; diagnostic.selected_attempt = vehicle->id;
            if (track.surface.valid) {
                if (!p.next_surface) { track.surface = {}; continue; }
                track.serial = p.next_surface;
                p.next_surface = p.next_surface == UINT64_MAX ? 0 : p.next_surface + 1;
                track.motion.reset();
                discovered_vehicle = vehicle->id;
                ++diagnostic.surfaces;
                update_contact(*vehicle, track, transform);
            }
        }
        // Genuine changes still revoke the early publication, even when rays
        // ran late. Elapsed optional work alone only skips cosmetic republish.
        const bool cosmetic_gate_live = validate_lifetime();
        {
            std::lock_guard lock(p.mutex);
            if (!epoch_current(epoch) || p.published.epoch != epoch ||
                p.published.removals != events.through || p.published.scope != scope || p.published.sampled_at != now) return;
            source_require(p.root_changes.load(std::memory_order_acquire) == roots_before &&
                p.removals.current_sequence() == events.through && p.active_removals.load(std::memory_order_acquire) == 0,
                "Car Grab native lifetime changed before cosmetic publication.");
            const bool client_fresh = cosmetic_gate_live && gate_live(p.client);
            const auto publish_at = now_seconds();
            copy_last_probe(publish_at);
            diagnostic.discovery_seconds = std::isfinite(discovery_started) && std::isfinite(publish_at) &&
                publish_at >= discovery_started ? publish_at - discovery_started : 0.;
            diagnostic.discovery_late = !client_fresh || !capture_fresh(publish_at);
            if (diagnostic.discovery_late) {
                // A late collision point was paired with this older server
                // pose. Discard it before a later capture could reuse that
                // geometry, preserving the cooldown and monotonic serials.
                if (discovered_vehicle) {
                    auto& track = p.surfaces.at(discovered_vehicle);
                    track.surface = {}; track.motion.reset(); track.serial = 0; track.cached = {};
                }
                diagnostic.surfaces = p.published.diagnostic.surfaces;
                diagnostic.grips = p.published.diagnostic.grips;
                // Diagnostic values may describe a late attempt, but traffic,
                // grips and their actual capture timestamps remain untouched.
                p.published.diagnostic = diagnostic;
            } else {
                diagnostic.grips = static_cast<std::uint32_t>(std::count_if(next.vehicles.begin(), next.vehicles.end(),
                    [](const auto& vehicle) { return vehicle.grip.valid; }));
                next.diagnostic = diagnostic;
                p.published = std::move(next);
            }
        }
    } catch (const SourceGuard& failure) { failed(TrafficStatus::incomplete, failure.message); }
    catch (...) { failed(TrafficStatus::incomplete, "Unexpected native capture failure."); }
}
TrafficSnapshot snapshot() {
    auto& p = provider();
    std::lock_guard lock(p.mutex);
    auto result = p.published;
    if (!epoch_current(result.epoch)) {
        result.diagnostic.last_probe = {}; result.diagnostic.last_probe_vehicle = 0;
        result.diagnostic.last_probe_at = result.diagnostic.last_probe_age_seconds = 0;
    }
    return result;
}
bool current(const TrafficSnapshot& sample, std::uint64_t selected, CurrentReport* report) noexcept {
    SourceLastError preserve;
    auto& p = provider();
    CurrentReport discarded;
    auto& evidence = report ? *report : discarded;
    evidence = {}; evidence.failure = CurrentFailure::unexpected;
    try {
        const auto age = now_seconds() - sample.sampled_at;
        evidence.initial_age = age; evidence.initial_age_valid = std::isfinite(age);
        evidence.initial_status = sample.status;
        evidence.initial_epoch_current = epoch_current(sample.epoch);
        evidence.initial_publication_current = p.published_epoch.load(std::memory_order_acquire) == sample.epoch;
        evidence.initial_removal_free = p.active_removals.load(std::memory_order_acquire) == 0;
        evidence.initial_failure = sample.status != TrafficStatus::ready ? CurrentLeaseFailure::status :
            !std::isfinite(age) || age < 0 || age > .1 ? CurrentLeaseFailure::age :
            !evidence.initial_epoch_current ? CurrentLeaseFailure::epoch :
            !evidence.initial_publication_current ? CurrentLeaseFailure::published_epoch :
            !evidence.initial_removal_free ? CurrentLeaseFailure::active_removal : CurrentLeaseFailure::none;
        evidence.motion_warming = sample.status == TrafficStatus::warming;
        evidence.failure = CurrentFailure::initial_lease;
        // A temporal failure can retain attachment identity only after the same
        // guarded native checks below prove it. This never authorizes an old
        // velocity write: the exact 100ms motion lease is enforced at the end.
        source_require((sample.status == TrafficStatus::ready || sample.status == TrafficStatus::warming) &&
            std::isfinite(age) && age >= 0 && evidence.initial_epoch_current &&
            evidence.initial_publication_current && evidence.initial_removal_free,
            "Car Grab vehicle identity lease unavailable.");
        evidence.failure = CurrentFailure::scope_unavailable;
        const auto gate = client_gate();
        TrafficScope scope;
        SourceReader reader;
        read_scope(reader, scope, gate, sample.scope.park);
        scope.root = sample.scope.root; scope.root_data = sample.scope.root_data; scope.root_owner = sample.scope.root_owner;
        scope.root_generation = sample.scope.root_generation; scope.map = sample.scope.map;
        evidence.failure = CurrentFailure::scope_changed;
        source_require(scope == sample.scope, "Car Grab vehicle world changed.");
        evidence.failure = CurrentFailure::root_unavailable;
        read_root(reader, scope);
        if (selected) {
            // Traffic identity is independent of a measured collision face.
            // Preserve the latest synthetic incarnation check even when this
            // vehicle has no grip. Contact consumers use grip_current() too.
            evidence.failure = CurrentFailure::selected_unavailable;
            const auto old_vehicle = std::find_if(sample.vehicles.begin(), sample.vehicles.end(),
                [&](const auto& value) { return value.id == selected; });
            source_require(old_vehicle != sample.vehicles.end() && old_vehicle->generation,
                "Car Grab selected traffic vehicle is unavailable.");
            {
                evidence.failure = CurrentFailure::incarnation_changed;
                std::lock_guard lock(p.mutex);
                const auto latest = std::find_if(p.published.vehicles.begin(), p.published.vehicles.end(),
                    [&](const auto& value) { return value.id == selected; });
                const auto latest_identity = std::find_if(p.published.identities.begin(), p.published.identities.end(),
                    [&](const auto& value) { return value.value == selected; });
                const auto old_identity = std::find_if(sample.identities.begin(), sample.identities.end(),
                    [&](const auto& value) { return value.value == selected; });
                const bool latest_ready = p.published.status == TrafficStatus::ready;
                const bool latest_warming = p.published.status == TrafficStatus::warming;
                source_require((latest_ready || latest_warming) && p.published.epoch == sample.epoch &&
                    p.published.scope == sample.scope && latest_identity != p.published.identities.end() &&
                    old_identity != sample.identities.end() && old_identity->generation == old_vehicle->generation &&
                    latest_identity->generation == old_vehicle->generation &&
                    latest_identity->context == old_identity->context && latest_identity->record == old_identity->record &&
                    (latest == p.published.vehicles.end() || latest->generation == old_vehicle->generation),
                    "Car Grab selected traffic incarnation changed.");
                // A ready collection may still contain other cars while this
                // exact selected car is motion-warming. Copied incarnations
                // preserve its identity; absence of fresh motion denies writes.
                evidence.motion_warming = evidence.motion_warming || latest_warming || latest == p.published.vehicles.end();
            }
            evidence.failure = CurrentFailure::entity_changed;
            const auto identity = std::find_if(sample.identities.begin(), sample.identities.end(),
                [&](const auto& value) { return value.value == selected; });
            source_require(identity != sample.identities.end() && source_object(identity->context) && source_object(identity->record) &&
                reader.value<std::uint32_t>(identity->context, 8) == static_cast<std::uint32_t>(selected >> 32) &&
                reader.value<std::uint32_t>(identity->record) == static_cast<std::uint32_t>(selected),
                "Car Grab selected entity is no longer current.");
            evidence.failure = CurrentFailure::traffic_unavailable;
            const auto ids = read_table(reader, scope.traffic);
            evidence.failure = CurrentFailure::traffic_missing;
            source_require(std::find(ids.begin(), ids.end(), selected) != ids.end(), "Car Grab selected car left traffic.");
        }
        evidence.failure = CurrentFailure::removal;
        const auto events = p.removals.read_since(sample.removals);
        source_require(events.status == car_grab::LifecycleReadStatus::complete &&
            (!selected || std::find(events.ids.begin(), events.ids.end(), selected) == events.ids.end()),
                "Car Grab selected car was removed.");
        evidence.failure = CurrentFailure::read_changed;
        reader.verify();
        const auto final_age = now_seconds() - sample.sampled_at;
        evidence.final_age = final_age; evidence.final_age_valid = std::isfinite(final_age);
        evidence.failure = CurrentFailure::final_lease;
        const auto final_gate = client_gate();
        source_require(epoch_current(sample.epoch) && p.published_epoch.load(std::memory_order_acquire) == sample.epoch &&
            final_gate.base == gate.base && final_gate.client == gate.client && final_gate.context == gate.context &&
            gate_live(final_gate) && std::isfinite(final_age) && final_age >= 0 &&
            p.active_removals.load(std::memory_order_acquire) == 0 && p.removals.current_sequence() == events.through,
            "Car Grab vehicle lifetime changed before write.");
        evidence.identity_verified = true;
        if (age > .1 || sample.status != TrafficStatus::ready || evidence.motion_warming) {
            evidence.failure = CurrentFailure::initial_lease;
            if (evidence.motion_warming && evidence.initial_failure == CurrentLeaseFailure::none)
                evidence.initial_failure = CurrentLeaseFailure::status;
            return false;
        }
        if (final_age > .1) return false;
        if (selected) p.selected_vehicle.store(selected, std::memory_order_release);
        evidence.failure = CurrentFailure::none;
        return true;
    } catch (...) { return false; }
}
}
