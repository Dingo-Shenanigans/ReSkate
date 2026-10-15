#include "traffic_vehicle_surface.h"
#include "traffic_vehicle_surface_math.h"
#include <Windows.h>
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/park_editor.h"
#include "Engine/Game/Build/20260929/profile.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/puppet_cost.h"
#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include "Extension/Objects/ParkEditor/park_editor_picking.h"
#include "Extension/Progression/mission_progression_internal.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>

namespace dingosdk::car_grab_native {
namespace {
namespace sm = surface_math;
using V = car_grab::Vec3;
struct SurfaceRuntime {
    editor::NativeSurfaceApi surface;
    editor::NativePickingApi picking;
    bool (*valid)(const void*){};
    void* (*query)(void*,std::uint64_t){};
    void* (*parent)(const void*,void*){};
    std::uint64_t (*component)(const void*,const void*,void*,std::uint32_t){};
    std::uintptr_t peer_vtable{}, peer_type{}, peer_type_vtable{};
    std::uintptr_t base{};
    bool self_ready{};
    std::atomic<bool> ready{};
};
// The published SkaterEcsInteropComponent constructor initializes its attached
// prefab at +0x40. Native attach writes the full ECS entity at +0x58, with its
// client context at +0x50 and the owning legacy entity at +0x60. These are
// supported-image field contracts, never a conversion of a legacy pointer.
constexpr game::build::Fingerprint self_construct_contract{0xf88670, {
    0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x55,0x41,0x56,
    0x41,0x57,0x48,0x81,0xec,0xf0,0x00,0x00,0x00,0x44,0x0f,0xb7,0x05,0xff,0x85,0x2f}};
constexpr game::build::Fingerprint self_fields_contract{0x104bee0, {
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0x8f,0x18,0x13,
    0x05,0x48,0x89,0x51,0x08,0x48,0x89,0x01,0x48,0x8b,0xf9,0x4c,0x89,0x41,0x10,0x49}};
constexpr game::build::Fingerprint self_attach_contract{0x105d0c3, {
    0x4c,0x8b,0x4e,0x20,0x4c,0x8b,0xc3,0x89,0x4c,0x24,0x20,0x48,0x8b,0xc8,0xe8,0xda,
    0xda,0xfc,0xff,0x48,0x8b,0x08,0x48,0x89,0x4e,0x18,0x4c,0x85,0x76,0x18,0x4c,0x89}};
constexpr std::uintptr_t self_prefab_vtable_rva=0x61714f8;
SurfaceRuntime& runtime() { static auto* value = new SurfaceRuntime; return *value; }
// Exact supported-image native parent-query ABI, corroborated by native pose
// composition and the SDK placement resolver. It does not use thread context.
constexpr game::build::Fingerprint parent_query_contract{0x276f460, {
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xd1,0x48,0x8d,0x4c,0x24,
    0x38,0xe8,0x8a,0xc8,0xff,0xff,0x45,0x33,0xc0,0x48,0x8b,0xcb,0x48,0x8b,0x10,0xe8}};
// Native traffic spawn writes reciprocal full handles through this descriptor;
// removal reads the same borrowed payload and deletes the paired entity first.
// Pin both that relationship and the read-only query/output ABI before use.
constexpr game::build::Fingerprint component_query_contract{0x276c4f0, {
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x28,0x4c,0x8b,0x51,0x10,0x48,0x8b,0xd9,
    0x48,0x83,0xc1,0x08,0x41,0x8b,0xe9,0x49,0x8b,0xf0,0x4d,0x85,0xd2,0x74,0x0c,0x8b}};
constexpr game::build::Fingerprint peer_read_contract{0x329f685, {
    0x48,0x8d,0x05,0xf4,0xd2,0xe2,0x02,0x45,0x33,0xc9,0x48,0x89,0x45,0xf7,0x48,0x8d,
    0x15,0x86,0x46,0x37,0x04,0x48,0x8b,0x45,0x1f,0x4c,0x8d,0x45,0xf7,0xc5,0xf9,0xef}};
constexpr game::build::Fingerprint peer_spawn_type_contract{0x32b6fc0, {
    0x48,0x8d,0x35,0x59,0xcd,0x35,0x04,0x48,0x8b,0x05,0xba,0xb7,0xf2,0x03,0x48,0x89,
    0x84,0x24,0x30,0x01,0x00,0x00,0x8b,0x84,0x24,0xf2,0x01,0x00,0x00,0x89,0x44,0x24}};
constexpr game::build::Fingerprint peer_spawn_link_contract{0x32b70a7, {
    0x4c,0x8d,0x2d,0xb2,0xff,0x14,0xfd,0x44,0x89,0xa4,0x24,0x0c,0x01,0x00,0x00,0xc6,
    0x84,0x24,0x08,0x01,0x00,0x00,0x01,0x48,0x89,0x38,0x49,0x8b,0xc7,0x48,0x8b,0x1d}};
constexpr game::build::Fingerprint peer_output_contract{0x408770, {
    0x4c,0x89,0x49,0x20,0x48,0x89,0x51,0x08,0x4c,0x89,0x41,0x10,0xc3,0xcc,0xcc,0xcc,
    0x48,0x8d,0x42,0x18,0x4c,0x89,0x41,0x08,0x48,0x89,0x41,0x10,0x48,0x8d,0x42,0x20}};
constexpr game::build::Fingerprint peer_type_init_contract{0x5cceec0, {
    0x48,0x83,0xec,0x28,0x48,0x8d,0x0d,0x55,0x4e,0x94,0x01,0xe8,0x30,0x4d,0xae,0xfc,
    0x48,0x8d,0x05,0x29,0x92,0x3a,0x00,0x48,0x89,0x05,0x42,0x4e,0x94,0x01,0x48,0x8d}};
constexpr std::uintptr_t peer_vtable_rva=0x60cc980, peer_type_rva=0x7613d20, peer_type_vtable_rva=0x643cc30;
// Each native query payload is 24 bytes. Aligned wrapper stride is 32 bytes,
// so every payload, including every element of the bounded chain, is aligned.
struct alignas(16) EntityQuery {
    std::array<std::uint64_t,3> value{};
    std::uint64_t alignment_padding{};
    bool operator==(const EntityQuery& other) const { return value==other.value; }
};
static_assert(sizeof(EntityQuery)==32 && alignof(EntityQuery)==16);
struct alignas(16) PeerRead {
    // Borrowed native output, not the writable 56B result that owns a lease.
    std::array<std::uintptr_t,5> value{};
    std::uint64_t peer{};
    bool operator==(const PeerRead&) const = default;
};

// Keep SEH at primitive call boundaries. These helpers have no C++ destructors
// and cannot leak a partly populated native result into the consumer.
std::uintptr_t guarded_world(std::uintptr_t context) noexcept {
    __try { return runtime().surface.world(context); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool guarded_ray(std::uintptr_t world, editor::NativeSurfaceResult* result,
                 const editor::NativeSurfaceRay* ray) noexcept {
    __try { return runtime().surface.ray(world,result,ray,"ReSkate_CarGripSurface") == result; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_valid(const void* reference) noexcept {
    __try { return runtime().valid(reference); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_hit_body(const void* hit, void* out) noexcept {
    __try { return runtime().picking.hit_body(hit,out) == out && runtime().picking.body_valid(out); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_body_entity(void* out, const void* body) noexcept {
    __try { return runtime().picking.body_valid(body) && runtime().picking.body_entity(out,body) == out; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::uint64_t guarded_mapped(std::uint64_t entity, SurfaceMappingStatus* status = nullptr) noexcept {
    auto result = SurfaceMappingStatus::api_unavailable;
    std::uint64_t mapped{};
    __try {
        const auto& api = runtime().picking;
        if (api.ready && entity && api.mapping && api.map_entity) {
            result = SurfaceMappingStatus::table_unavailable;
            // Same ready-table guards and direction as the pinned SDK helper.
            // Copy the boundary result from these checks, without more queries.
            const auto manager = api.mapping();
            std::uintptr_t buckets{}; std::uint32_t count{}, entries{};
            if (manager && memory::read(manager+0x70,buckets) && buckets &&
                memory::read(manager+0x78,count) && count && count<=0x100000 &&
                memory::read(manager+0x7c,entries) && entries) {
                if (api.map_entity(manager,&mapped,entity,1)==&mapped)
                    result = mapped ? SurfaceMappingStatus::mapped : SurfaceMappingStatus::lookup_miss;
                else mapped = 0;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { mapped = 0; }
    if (status) *status = result;
    return mapped;
}
bool guarded_query(std::uint64_t id, EntityQuery* out) noexcept {
    __try { return runtime().query && runtime().query(out->value.data(),id)==out->value.data(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool guarded_parent(const EntityQuery* in, EntityQuery* out) noexcept {
    __try { return runtime().parent && runtime().parent(in->value.data(),out->value.data())==out->value.data(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool pointer(std::uintptr_t p) noexcept {
    return p>=0x10000 && p%8==0 && p<=memory::highest_user_address-16;
}
bool query_identity(const EntityQuery& query) noexcept {
    const auto id = query.value[0], context = query.value[1], record = query.value[2];
    std::uint32_t space{}, incarnation{};
    alignas(16) const std::array<std::uint64_t,2> reference{id,0};
    return (id>>32) && pointer(context) && pointer(record) &&
        memory::read(context+8,space) && space==static_cast<std::uint32_t>(id>>32) &&
        memory::read(record,incarnation) && incarnation==static_cast<std::uint32_t>(id) &&
        guarded_valid(reference.data());
}
bool query_current(const EntityQuery& query) noexcept {
    EntityQuery after;
    return query_identity(query) && guarded_query(query.value[0],&after) &&
        after==query && query_identity(after);
}
bool chain_current(const std::array<EntityQuery,9>& chain,std::size_t depth) noexcept {
    for (std::size_t i=0;i<=depth;++i) {
        if (!query_current(chain[i])) return false;
        if (i<depth) {
            EntityQuery after_parent;
            if (!guarded_parent(&chain[i],&after_parent) || after_parent!=chain[i+1]) return false;
        }
    }
    return true;
}
SurfacePeerStatus guarded_peer(const EntityQuery& query, PeerRead* result) noexcept {
    const auto& r=runtime();
    if (!r.component || !r.peer_vtable || !r.peer_type) return SurfacePeerStatus::api_unavailable;
    // Static descriptors may not yet be initialized when the mod starts. Test
    // readiness at each query so an early unavailable state can recover safely.
    std::uintptr_t type_vtable{};
    if (!r.peer_type_vtable || !memory::read(r.peer_type,type_vtable) || type_vtable!=r.peer_type_vtable)
        return SurfacePeerStatus::api_unavailable;
    result->value={r.peer_vtable,0,0,query.value[0],r.peer_type}; result->peer=0;
    __try {
        // Native return value is unspecified for the borrowed read result.
        // The pinned output callback fills its guard, payload and descriptor.
        r.component(query.value.data(),reinterpret_cast<const void*>(r.peer_type),result->value.data(),0);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return SurfacePeerStatus::invalid_result; }
    if (result->value[0]!=r.peer_vtable || result->value[3]!=query.value[0] || result->value[4]!=r.peer_type)
        return SurfacePeerStatus::invalid_result;
    if (!result->value[1] || !result->value[2]) return SurfacePeerStatus::not_found;
    const auto pointer=[](std::uintptr_t p) { return p>=0x10000 && p%8==0 && p<=memory::highest_user_address-16; };
    // The native guard is an opaque nonzero token; only the payload is read.
    if (!pointer(result->value[2]) || !memory::read(result->value[2],result->peer))
        return SurfacePeerStatus::invalid_result;
    if (!result->peer || !(result->peer>>32)) return SurfacePeerStatus::not_found;
    return SurfacePeerStatus::paired; // The reciprocal identity check is below.
}
SurfacePeerStatus traffic_pair(const EntityQuery& owner, const EntityQuery& selected,
                              PeerRead* from_owner, PeerRead* from_selected, SurfaceProbeReport* report) noexcept {
    auto status=guarded_peer(owner,from_owner);
    if (report) { report->peer_owner=from_owner->peer; report->peer_selected=0; }
    if (status==SurfacePeerStatus::paired) {
        status=guarded_peer(selected,from_selected);
        if (report) report->peer_selected=from_selected->peer;
        if (status==SurfacePeerStatus::paired &&
            (from_owner->peer!=selected.value[0] || from_selected->peer!=owner.value[0]))
            status=SurfacePeerStatus::mismatch;
    }
    if (report) report->peer_status=status;
    return status;
}
bool pair_current(const EntityQuery& owner, const EntityQuery& selected,
                  const PeerRead& from_owner, const PeerRead& from_selected) noexcept {
    PeerRead after_owner, after_selected;
    return query_current(owner) && query_current(selected) &&
        traffic_pair(owner,selected,&after_owner,&after_selected,nullptr)==SurfacePeerStatus::paired &&
        after_owner==from_owner && after_selected==from_selected && query_current(owner) && query_current(selected);
}
mission_progression_detail::ComponentQuery component_api(std::uintptr_t base) {
    std::array<unsigned char,32> bytes{};
    if (memory::read_bytes(base+component_query_contract.rva,bytes.data(),bytes.size()) && bytes==component_query_contract.bytes)
        return reinterpret_cast<mission_progression_detail::ComponentQuery>(base+component_query_contract.rva);
    // Local-profile initialization installs the SDK's verified quest hook
    // before car-grab initialization. Its original trampoline retains the
    // exact native query ABI; arbitrary patched entries remain unavailable.
    auto& state=mission_progression_detail::main_mission_override_state();
    std::lock_guard lock(state.initialization_mutex);
    const auto gate=state.gate.load(std::memory_order_acquire);
    using Gate=mission_progression_detail::GateStatus;
    if (!state.attempted || state.base!=base ||
        !(gate==Gate::prepared || gate==Gate::route_required || gate==Gate::restoring || gate==Gate::active)) return nullptr;
    return state.original_query.load(std::memory_order_acquire);
}
void guarded_release(editor::NativeSurfaceResult* result) noexcept {
    __try {
        if (result->scope[2]) {
            const auto allocator = runtime().surface.scope_allocator();
            std::uintptr_t vtable{}, function{};
            if (memory::read(allocator,vtable) && memory::read(vtable+8,function) && function)
                reinterpret_cast<void (*)(std::uintptr_t,void*)>(function)(allocator,result->scope.data());
        }
        if (result->allocator) runtime().surface.release(&result->allocator_vtable,result->data);
    } __except(EXCEPTION_EXECUTE_HANDLER) { /* Invalid native scopes are never dereferenced again. */ }
}
bool owned(const PhysicsBodyToken& body, std::uint64_t selected, SurfaceProbeReport* report = nullptr) noexcept {
    alignas(16) std::array<std::uint64_t,2> raw{}, selected_ref{selected,0};
    if (report) {
        report->raw_owner = report->mapped_owner = report->ancestor_owner = report->ancestor_mapped = 0;
        report->raw_valid = false; report->ancestor_depth = 0;
        report->owner_stage = SurfaceOwnerStage::none;
        report->mapping_status = SurfaceMappingStatus::none;
        report->peer_status = SurfacePeerStatus::none;
        report->peer_owner = report->peer_selected = 0;
        report->peer_ancestor_owner=0;report->peer_ancestor_depth=0;
    }
    const auto rejected = [&](SurfaceOwnerStage stage) {
        if (report) {
            ++report->owner_rejects;
            report->owner_stage = stage; report->raw_owner = raw[0];
        }
        return false;
    };
    if (!selected || !guarded_valid(selected_ref.data())) return rejected(SurfaceOwnerStage::invalid_selected);
    if (!body.world || body.index==UINT32_MAX || !guarded_body_entity(raw.data(),&body))
        return rejected(SurfaceOwnerStage::invalid_body);
    if (report) report->raw_owner = raw[0];
    if (!raw[0]) return rejected(SurfaceOwnerStage::missing_owner);
    if (!guarded_valid(raw.data())) return rejected(SurfaceOwnerStage::invalid_raw);
    if (report) report->raw_valid = true;
    const auto same_owner = [&] {
        alignas(16) std::array<std::uint64_t,2> after{};
        return guarded_body_entity(after.data(),&body) && after[0]==raw[0] &&
            guarded_valid(after.data()) && guarded_valid(selected_ref.data());
    };
    std::array<EntityQuery,9> chain{};
    const auto accepted = [&](SurfaceOwnerStage stage, std::uint64_t mapped_entity = 0, std::size_t depth = 0) {
        if (!same_owner()) return rejected(SurfaceOwnerStage::changed);
        if (mapped_entity && (guarded_mapped(mapped_entity)!=selected || !same_owner()))
            return rejected(SurfaceOwnerStage::changed);
        if (depth && !chain_current(chain,depth)) return rejected(SurfaceOwnerStage::changed);
        if (report) report->owner_stage = stage;
        return true;
    };
    if (raw[0]==selected) return accepted(SurfaceOwnerStage::direct);
    EntityQuery selected_query;
    const bool raw_queried=runtime().query && guarded_query(raw[0],&chain[0]) &&
        chain[0].value[0]==raw[0] && query_identity(chain[0]);
    const bool peer_available=runtime().component && raw_queried &&
        guarded_query(selected,&selected_query) && selected_query.value[0]==selected && query_identity(selected_query);
    const auto peer_contact=[&](std::size_t depth)->std::optional<bool> {
        if (!peer_available) {
            if (report) report->peer_status=SurfacePeerStatus::api_unavailable;
            return {};
        }
        PeerRead owner_peer, selected_peer;
        if (!query_current(chain[depth]) || !query_current(selected_query)) return rejected(SurfaceOwnerStage::changed);
        SurfaceProbeReport peer_evidence;
        const auto peer_status=traffic_pair(chain[depth],selected_query,&owner_peer,&selected_peer,&peer_evidence);
        // Keep a concrete mismatch associated with the ancestor that produced
        // it, even when a later ancestor has no pair or reaches the root.
        if (report && (peer_status==SurfacePeerStatus::mismatch || peer_status==SurfacePeerStatus::paired ||
                       report->peer_status!=SurfacePeerStatus::mismatch)) {
            report->peer_status=peer_status;
            report->peer_owner=peer_evidence.peer_owner;report->peer_selected=peer_evidence.peer_selected;
            report->peer_ancestor_owner=chain[depth].value[0];
            report->peer_ancestor_depth=static_cast<std::uint32_t>(depth);
        }
        if (peer_status!=SurfacePeerStatus::paired) return {};
        // Prove the actual raw shape's ancestry, then reread both borrowed
        // payloads after each final owner check. No copied link is a lease.
        if (!same_owner() || !chain_current(chain,depth) || !pair_current(chain[depth],selected_query,owner_peer,selected_peer) ||
            !same_owner() || !chain_current(chain,depth) || !pair_current(chain[depth],selected_query,owner_peer,selected_peer)) {
            if (report) report->peer_status=SurfacePeerStatus::changed;
            return rejected(SurfaceOwnerStage::changed);
        }
        if (report) report->owner_stage=depth?SurfaceOwnerStage::parent_traffic_peer:SurfaceOwnerStage::traffic_peer;
        return true;
    };
    if (raw_queried) if (const auto peer=peer_contact(0); peer.has_value()) return *peer;
    SurfaceMappingStatus mapping_status{};
    const auto mapped = guarded_mapped(raw[0],&mapping_status);
    if (report) { report->mapped_owner=mapped; report->mapping_status=mapping_status; }
    if (mapped==selected) return accepted(SurfaceOwnerStage::mapped,raw[0]);
    if (!runtime().query || !runtime().parent) return rejected(SurfaceOwnerStage::query_unavailable);
    // Only actual native ancestry can bridge a collision child to the traffic
    // root. No placement marker, spatial match or unknown-owner exclusion.
    if (!raw_queried)
        return rejected(SurfaceOwnerStage::query_unavailable);
    for (std::size_t depth=1; depth<chain.size(); ++depth) {
        auto& ancestor=chain[depth];
        if (!query_current(chain[depth-1]) || !guarded_parent(&chain[depth-1],&ancestor))
            return rejected(SurfaceOwnerStage::changed);
        if (report) { report->ancestor_depth=static_cast<std::uint32_t>(depth); report->ancestor_owner=ancestor.value[0]; }
        if (!ancestor.value[0]) return rejected(SurfaceOwnerStage::no_parent);
        if (ancestor.value[0]>>32 != raw[0]>>32 || ancestor.value[1]!=chain[0].value[1] || !query_identity(ancestor))
            return rejected(SurfaceOwnerStage::invalid_parent);
        for (std::size_t i=0; i<depth; ++i)
            if (chain[i].value[0]==ancestor.value[0]) return rejected(SurfaceOwnerStage::cycle);
        if (ancestor.value[0]==selected) return accepted(SurfaceOwnerStage::parent_direct,0,depth);
        if (const auto peer=peer_contact(depth); peer.has_value()) return *peer;
        const auto ancestor_mapped=guarded_mapped(ancestor.value[0],&mapping_status);
        if (report) { report->ancestor_mapped=ancestor_mapped; report->mapping_status=mapping_status; }
        if (ancestor_mapped!=selected) continue;
        // Re-read every queried identity AND relationship before accepting. The
        // full collision token/raw owner and selected car are checked afresh too.
        if (!chain_current(chain,depth)) return rejected(SurfaceOwnerStage::changed);
        return accepted(SurfaceOwnerStage::parent_mapped,ancestor.value[0],depth);
    }
    return rejected(SurfaceOwnerStage::depth_limit);
}
struct Hit { V point; PhysicsBodyToken body; };
struct SkaterIdentity {
    std::uintptr_t context{}, entity{}, collection{}, component{}, player{};
    std::uint64_t id{};
    EntityQuery query;
    bool operator==(const SkaterIdentity&) const = default;
};
bool skater_identity(std::uintptr_t context,std::uintptr_t entity,SkaterIdentity& result) noexcept {
    result={};
    auto& r=runtime();
    if (!r.self_ready || !r.base || !pointer(context) || !pointer(entity)) return false;
    std::uintptr_t vtable{}, actual_context{}, collection{}, player{}, player_context{};
    std::uint8_t local{}, remote{}, count{};
    if (!memory::read(entity,vtable) || vtable!=r.base+addr::engine::skater_entity_vtable ||
        !memory::read(entity+0x20,actual_context) || actual_context!=context ||
        !memory::read(entity+0xf8,player) || !pointer(player) ||
        !memory::read(player,vtable) || vtable!=r.base+addr::engine::local_player_vtable ||
        !memory::read(player+0x78,player_context) || player_context!=context ||
        !memory::read(player+0xb8,vtable) || vtable!=entity ||
        !memory::read(player+0x45,local) || local!=1 || !memory::read(player+0x44,remote) || remote!=0 ||
        !memory::read(entity+0x70,collection) || !pointer(collection) ||
        !memory::read(collection,vtable) || vtable!=entity || !memory::read(collection+8,count) || count>128) return false;
    std::uintptr_t component{};
    for (unsigned i=0;i<count;++i) {
        std::uintptr_t candidate{},backlink{};
        if (!memory::read(collection+0x20+i*0x20,candidate)) return false;
        if (!candidate) continue;
        if (!pointer(candidate) || !memory::read(candidate,vtable)) return false;
        if (vtable!=r.base+addr::puppet_cost::ecs_vtable) continue;
        if (component || !memory::read(candidate+0x18,backlink) || backlink!=collection) return false;
        component=candidate;
    }
    std::uint64_t id{}; std::uintptr_t owner{}; std::uint8_t attached{};
    if (!component || !memory::read(component+0x40,vtable) || vtable!=r.base+self_prefab_vtable_rva ||
        !memory::read(component+0x50,actual_context) || actual_context!=context ||
        !memory::read(component+0x60,owner) || owner!=entity ||
        !memory::read(component+0x79,attached) || attached!=1 ||
        !memory::read(component+0x58,id) || !(id>>32)) return false;
    EntityQuery query;
    if (!guarded_query(id,&query) || query.value[0]!=id || !query_current(query)) return false;
    result={context,entity,collection,component,player,id,query};
    return true;
}
bool skater_current(const SkaterIdentity& before) noexcept {
    SkaterIdentity after;
    return before.id && skater_identity(before.context,before.entity,after) && after==before;
}
struct SelfExclusions {
    SkaterIdentity skater;
    std::array<PhysicsBodyToken,8> shapes{};
    unsigned count{};
};
bool exclusions_current(const SelfExclusions& exclusions,SurfaceProbeReport& report) noexcept {
    if (!exclusions.skater.id) return true;
    if (report.self_changed || !skater_current(exclusions.skater)) {
        report.self_changed=true;return false;
    }
    for (unsigned i=0;i<exclusions.count;++i) {
        if (!owned(exclusions.shapes[i],exclusions.skater.id) || !skater_current(exclusions.skater)) {
            report.self_changed=true;return false;
        }
    }
    return true;
}
std::optional<Hit> cast(std::uintptr_t world, std::uint64_t selected, V start, V end, SurfaceProbeReport& report,
                       SelfExclusions* exclusions=nullptr) {
    if (!sm::finite(start) || !sm::finite(end)) return {};
    for (float lane : {start.x,start.y,start.z,end.x,end.y,end.z})
        if (std::abs(lane)>100000) return {};
    alignas(16) editor::NativeSurfaceRay request;
    request.start = {start.x,start.y,start.z,0}; request.end = {end.x,end.y,end.z,0};
    // At most eight exact, freshly verified self shapes can be learned by the
    // entire probe. Each new shape causes one retry; unknown collision blocks.
    for (unsigned attempt=0;attempt<=8;++attempt) {
        if (exclusions && !exclusions_current(*exclusions,report)) return {};
        if (exclusions && exclusions->count) {
            request.ignored_bodies[0]=reinterpret_cast<std::uintptr_t>(exclusions->shapes.data());
            request.ignored_bodies[1]=reinterpret_cast<std::uintptr_t>(exclusions->shapes.data()+exclusions->count);
            request.ignored_bodies[2]=request.ignored_bodies[1];
        }
        alignas(16) editor::NativeSurfaceResult result;
        ++report.ray_calls;
        const auto called = guarded_ray(world,&result,&request);
        struct Release { editor::NativeSurfaceResult& result; ~Release() { guarded_release(&result); } } release{result};
        if (!called || result.world != world || !result.data || result.count>4096 ||
            result.first>UINT32_MAX-result.count) return {};
        std::uintptr_t fractions{};
        if (!memory::read(result.data+8,fractions) || !fractions) return {};
        float closest = 2; std::uint32_t closest_index{};
        for (std::uint32_t i = 0; i<result.count; ++i) {
            float fraction{};
            if (!memory::read(fractions+static_cast<std::uintptr_t>(result.first+i)*4,fraction) ||
                !std::isfinite(fraction) || fraction<0 || fraction>1) return {};
            if (fraction<closest) { closest=fraction; closest_index=result.first+i; }
        }
        if (closest>1) return {};
        ++report.closest_hits;
        alignas(16) const std::array<std::uint64_t,3> hit{world,closest_index,result.data};
        alignas(16) PhysicsBodyToken body{};
        if (!guarded_hit_body(hit.data(),&body) || body.world != world) return {};
        if (!owned(body,selected,&report)) {
            if (!exclusions || !exclusions->skater.id || exclusions->skater.id==selected ||
                exclusions->count==exclusions->shapes.size() || !owned(body,exclusions->skater.id)) return {};
            if (!exclusions_current(*exclusions,report)) return {};
            if (std::find(exclusions->shapes.begin(),exclusions->shapes.begin()+exclusions->count,body)!=
                exclusions->shapes.begin()+exclusions->count) return {};
            exclusions->shapes[exclusions->count++]=body;
            ++report.self_excluded;
            continue;
        }
        // Always test the closest physical surface, even if its entity is unknown.
        // This cannot reach through world collision to select a car behind it.
        if (exclusions && !exclusions_current(*exclusions,report)) return {};
        return Hit{sm::add(start,sm::mul(sm::sub(end,start),closest)),body};
    }
    return {};
}
} // namespace

void surface_start(std::uintptr_t base) noexcept {
    auto& r = runtime(); r.ready.store(false,std::memory_order_release);r.self_ready=false;r.base=base;
    try {
        r.surface = editor::surface_api(base); r.picking = editor::picking_api(base);
        for (const auto* contract : {&addr::profile::placement_valid_contract,
            &addr::profile::placement_query_contract,&parent_query_contract,
            &peer_read_contract,&peer_spawn_type_contract,
            &peer_spawn_link_contract,&peer_output_contract,&peer_type_init_contract}) {
            std::array<unsigned char,32> bytes{};
            if (!memory::read_bytes(base+contract->rva,bytes.data(),bytes.size()) || bytes != contract->bytes) return;
        }
        if (!r.surface.ready || !r.picking.ready) return;
        r.valid = reinterpret_cast<decltype(r.valid)>(base+addr::profile::placement_valid_contract.rva);
        r.query = reinterpret_cast<decltype(r.query)>(base+addr::profile::placement_query_contract.rva);
        r.parent = reinterpret_cast<decltype(r.parent)>(base+parent_query_contract.rva);
        std::uintptr_t output_callback{};
        if (!memory::read(base+peer_vtable_rva+0x30,output_callback) || output_callback!=base+peer_output_contract.rva) return;
        r.component = component_api(base);
        if (!r.component) return;
        r.peer_vtable=base+peer_vtable_rva; r.peer_type=base+peer_type_rva;
        r.peer_type_vtable=base+peer_type_vtable_rva;
        const std::array self_contracts{&self_construct_contract,&self_fields_contract,&self_attach_contract};
        r.self_ready=std::all_of(self_contracts.begin(),self_contracts.end(),[base](const auto* contract) {
            std::array<unsigned char,32> bytes{};
            return memory::read_bytes(base+contract->rva,bytes.data(),bytes.size()) && bytes==contract->bytes;
        });
        r.ready.store(true,std::memory_order_release);
    } catch (...) { r.ready.store(false,std::memory_order_release); }
}
NativeGripSurface probe_surface(std::uintptr_t context, std::uint64_t selected,
    const std::array<float,12>& pose, V world_forward, V rider_position, SurfaceProbeReport* report,
    std::uintptr_t skater_entity) noexcept {
    SurfaceProbeReport discarded;
    auto& evidence = report ? *report : discarded;
    evidence = {}; evidence.stage = SurfaceProbeStage::api_unavailable;
    try {
        if (!runtime().ready.load(std::memory_order_acquire)) return {};
        evidence.stage = SurfaceProbeStage::invalid_input;
        if (!context || !selected || !sm::finite(rider_position) ||
            std::abs(rider_position.x)>100000 || std::abs(rider_position.y)>100000 ||
            std::abs(rider_position.z)>100000) return {};
        sm::Transform transform{};
        if (!sm::transform(pose,transform)) return {};
        const auto forward = sm::unit({world_forward.x,0,world_forward.z});
        if (sm::dot(forward,forward)<.99f) return {};
        const auto rear = sm::mul(forward,-1), side = sm::unit(sm::cross({0,1,0},rear));
        const auto rider_delta = sm::sub(rider_position,transform.position);
        // Match the provider's bounded local query envelope. A rider in front
        // of the observed motion direction cannot acquire a rear contact by
        // casting back through the car or accepting a front-facing hull.
        const auto distance_squared = sm::dot(rider_delta,rider_delta);
        const auto height = rider_position.y+.75f;
        if (!std::isfinite(distance_squared) || distance_squared>14.f*14.f ||
            sm::dot(rider_delta,rear)<.1f || !std::isfinite(height) || std::abs(height)>100000) return {};
        evidence.stage = SurfaceProbeStage::no_world;
        const auto world = guarded_world(context);
        if (!world) return {};
        SelfExclusions exclusions;
        if (skater_identity(context,skater_entity,exclusions.skater)) {
            evidence.self_available=true;evidence.self_entity=exclusions.skater.id;
        }
        evidence.stage = SurfaceProbeStage::no_owned_rear;
        // Distant terrain behind the rider is outside the approach segment.
        // Start at the verified rider instead of twelve metres behind the car.
        // The endpoint is a search extent, never a fabricated contact point.
        auto start = rider_position;
        auto end = sm::add(transform.position,sm::mul(forward,2));
        std::optional<Hit> center; float center_height{}, rear_extent = -1;
        // These are bounded search heights, never fabricated bumper positions.
        // Only a measured intersection can establish the final grip point.
        for (float delta : {0.0f,-.12f,.12f,-.24f,.24f}) {
            start.y = end.y = height+delta;
            const auto hit = cast(world,selected,start,end,evidence,&exclusions);
            if (!hit) continue;
            const auto extent = sm::dot(sm::sub(hit->point,transform.position),rear);
            // Prefer the measured protrusion, which can be the bumper lip, over
            // a recessed body face. Equal extents retain the preferred height.
            if (!center || extent>rear_extent+.01f) {
                center = hit; center_height = height+delta; rear_extent = extent;
            }
        }
        if (!center || evidence.self_changed) return {};
        evidence.stage = SurfaceProbeStage::neighborhood;
        start.y = end.y = center_height;
        constexpr float neighborhood = .035f;
        const auto ray = [&](V offset) {
            return cast(world,selected,sm::add(start,offset),sm::add(end,offset),evidence,&exclusions);
        };
        const auto left = ray(sm::mul(side,-neighborhood)), right = ray(sm::mul(side,neighborhood));
        const auto down = ray({0,-.01f,0}), up = ray({0,.01f,0});
        if (!left || !right || !down || !up || left->body != center->body || right->body != center->body ||
            down->body != center->body || up->body != center->body) return {};
        evidence.stage = SurfaceProbeStage::footprint;
        V normal{}, tangent{};
        if (!sm::footprint(center->point,left->point,right->point,down->point,up->point,rear,normal,tangent) ||
            sm::dot(sm::sub(center->point,transform.position),rear)<.1f || !owned(center->body,selected)) return {};
        NativeGripSurface result;
        result.valid = true; result.point = center->point; result.normal = normal; result.tangent = tangent;
        result.localpoint = sm::local_point(result.point,transform);
        result.localnormal = sm::local_normal(result.normal,transform);
        result.localtangent = sm::local_tangent(result.tangent,transform); result.body = center->body;
        const auto vertical = [&](float depth, float lateral, bool from_above) {
            auto a = sm::add(center->point,sm::add(sm::mul(normal,-depth),sm::mul(tangent,lateral)));
            auto b = a;
            a.y += from_above ? .3f : -.3f; b.y += from_above ? -.3f : .3f;
            return cast(world,selected,a,b,evidence,&exclusions);
        };
        // Top and underside are separately observed, never inferred from the
        // rear normal or an arbitrary finger curl radius. These six rays keep
        // the candidate probe to fifteen segment queries, plus at most eight
        // retries for exact self shapes across the entire probe.
        const auto top = vertical(.015f,0,true), top_inside = vertical(.035f,0,true),
                   top_side = vertical(.015f,.025f,true);
        const auto bottom = vertical(.015f,0,false), bottom_inside = vertical(.035f,0,false),
                   bottom_side = vertical(.015f,.025f,false);
        const auto same = [&](const std::optional<Hit>& hit) { return hit && hit->body == result.body; };
        V top_normal{}, bottom_normal{};
        if (same(top) && same(top_inside) && same(top_side) && same(bottom) && same(bottom_inside) &&
            same(bottom_side) && sm::wrap(center->point,normal,top->point,top_inside->point,top_side->point,
                bottom->point,bottom_inside->point,bottom_side->point,top_normal,bottom_normal) &&
            owned(result.body,selected)) {
            result.wrap_valid = true; result.top_point = top->point; result.bottom_point = bottom->point;
            result.top_normal = top_normal; result.bottom_normal = bottom_normal;
            result.localtop_point = sm::local_point(result.top_point,transform);
            result.localbottom_point = sm::local_point(result.bottom_point,transform);
            result.localtop_normal = sm::local_normal(result.top_normal,transform);
            result.localbottom_normal = sm::local_normal(result.bottom_normal,transform);
        }
        if (!exclusions_current(exclusions,evidence)) return {};
        evidence.stage = result.wrap_valid ? SurfaceProbeStage::ready_lip : SurfaceProbeStage::ready_face;
        return result;
    } catch (...) { return {}; }
}
bool surface_current(std::uintptr_t context, std::uint64_t selected, const NativeGripSurface& surface) noexcept {
    if (!runtime().ready.load(std::memory_order_acquire) || !context || !selected || !surface.valid ||
        !sm::finite(surface.point) || !sm::finite(surface.localpoint)) return false;
    const auto world = guarded_world(context);
    return world && surface.body.world == world && owned(surface.body,selected);
}
} // namespace dingosdk::car_grab_native
