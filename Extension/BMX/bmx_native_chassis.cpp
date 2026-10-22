#include "bmx_native_chassis.h"
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "bmx_native_coupled.h"
#endif
#include "bmx_collision_envelope.h"
#include "bmx_impact_test.h"
#include "Engine/Game/BMX/bmx_owner_observation.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/image_identity.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include <intrin.h>
#include <cmath>
#include <atomic>
#include <utility>
#include <type_traits>
#include <algorithm>
#include <limits>

namespace dingosdk::bmx::native_chassis {
namespace {
using V4 = std::array<float,4>;
using namespace addr::remote_collision;
constexpr double native_h = 1.0/60.0;
constexpr float maximum_force = 40000, maximum_torque = 4000, maximum_hop = 600;
constexpr game::build::Fingerprint linear_force{0x25d14b0, {
    0x44,0x8b,0x41,0x08,0x4c,0x8b,0xc9,0x48,0x8b,0x01,0x41,0x0f,0xb6,0xc8,0x80,0xe1,
    0x07,0x4c,0x8b,0x50,0x28,0x49,0x8b,0x82,0x80,0x09,0x00,0x00,0xc4,0xa2,0x79,0x18}};
constexpr game::build::Fingerprint angular_torque{0x25d19a0, {
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0xda,0x48,0x8b,0xf9,
    0x48,0x8d,0x54,0x24,0x20,0xe8,0x06,0x59,0x00,0x00,0xc5,0xf8,0x10,0x2b,0x44,0x8b}};
// REA v3 ev_a5bcfe744152869b61513e6559ee5948584bea3649f9ce7477cc3ad98a108c13:
// (body,const vec4*) stores the authored m/I principal-axis ratio in state988.
constexpr game::build::Fingerprint set_inertia_ratio{0x25dce90, {
    0x48,0x8b,0x01,0x44,0x8b,0x41,0x08,0xc5,0xf8,0x10,0x02,0x4d,0x03,0xc0,0x48,0x8b,
    0x48,0x28,0x48,0x8b,0x81,0x88,0x09,0x00,0x00,0xc4,0xa1,0x78,0x11,0x04,0xc0,0xc3}};
constexpr game::build::Fingerprint body_reset{0x2641070, {
    0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x48,0x8b,0xe9,0x8b,0xc2,0x8b,0xca,0x8b,0xf2,
    0x83,0xe1,0x1f,0x41,0xb8,0x01,0x00,0x00,0x00,0x41,0xd3,0xe0,0x48,0xc1,0xe8,0x05}};
constexpr game::build::Fingerprint body_quaternion{0x25d5cc0, {
    0x48,0x83,0xec,0x38,0x48,0x8b,0x01,0x4c,0x8b,0xd2,0x44,0x8b,0x49,0x08,0x33,0xd2,
    0xc5,0xf8,0x29,0x74,0x24,0x20,0xc5,0xf8,0x29,0x7c,0x24,0x10,0x4c,0x8b,0x40,0x28}};
// The retail routine itself produces force/torque inputs in25d14b0/19a0. An exact local
// wrapper match is evidence for that producer phase, NOT a world-wide barrier.
constexpr game::build::Fingerprint synchronizer{0xfc06b0, {
    0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,
    0xd0,0x00,0x00,0x00,0xf6,0x42,0x50,0x01,0x48,0x8b,0xf2,0xc5,0xf8,0x29,0x70,0xc8}};


constexpr game::build::Fingerprint overwrite_force{0x25da0b0, {
0x48,0x89,0x5c,0x24,0x08,0x48,0x8b,0x01,0x4c,0x8b,0xd1,0x44,0x8b,0x49,0x08,0x4c,
0x8b,0xda,0x45,0x8b,0xc1,0x41,0x0f,0xb6,0xc9,0x49,0xc1,0xe8,0x03,0x80,0xe1,0x07}};
constexpr game::build::Fingerprint overwrite_torque{0x25d7fe0, {
0x48,0x89,0x5c,0x24,0x08,0x48,0x8b,0x01,0x4c,0x8b,0xd1,0x44,0x8b,0x49,0x08,0x4c,
0x8b,0xda,0x45,0x8b,0xc1,0x41,0x0f,0xb6,0xc9,0x49,0xc1,0xe8,0x03,0x80,0xe1,0x07}};
struct Handle {
    std::uintptr_t world{};
    std::uint32_t index = 0xffffffffU, generation{};
    bool operator==(const Handle&) const = default;
};
static_assert(sizeof(Handle) == 16);
using World = std::uintptr_t (*)(std::uintptr_t);
using Allocate = Handle* (*)(std::uintptr_t, Handle*);
using Capsule = Handle* (*)(std::uintptr_t, Handle*, const Handle*, const float*, const float*, float);
using SetInt = void (*)(Handle*, int);
using SetFloat = void (*)(Handle*, float);
using Touch = void (*)(Handle*);
using SetMask = void (*)(Handle*, const std::uint32_t*);
using SetOwner = void (*)(Handle*, const Handle*);
using SetVector = void (*)(Handle*, const float*);
using Valid = bool (*)(const Handle*);
// The two incoming arguments are established. Preserve the original RAX even
// though the inspected side-effect-only callers do not consume a result.
using Synchronize = std::uintptr_t (*)(Handle*, std::uintptr_t);
struct Api {
    World world{};
    Allocate allocate{};
    Capsule capsule{};
    SetInt motion{};
    SetFloat inverse_mass{};
    Touch clear_aa0{}, wake{};
    SetMask categories{}, collides{};
    SetOwner owner{};
    SetVector com{}, transform{}, linear{}, angular{}, force{}, torque{}, inertia_ratio{}, overwrite_force{}, overwrite_torque{};
    Valid body_valid{}, shape_valid{};
    std::uintptr_t base{};
};
struct Ring {
    std::uintptr_t values{};
    std::uint32_t elements{};
    std::uint16_t current_plane{}, planes{};
    bool operator==(const Ring&) const = default;
};
static_assert(sizeof(Ring) == 16);
struct Snapshot {
    std::array<Ring, 4> rings{};
    std::array<std::uintptr_t, 4> direct_arrays{}; // 910,928,988,980
    V4 position{}, quaternion_xyzw{}, linear{}, angular{}, local_com{}, local_quaternion_wxyz{}, inertia_ratio{};
    float inverse_mass{};
    std::uintptr_t native_state{};
};
struct Configuration {
    float mass = 106, inverse_mass = 1.f/106.f;
    V4 inertia_ratio{106.f/26.f,106.f/32.f,106.f/10.f,0};
    std::uint32_t categories = 0, collides_with = 0x101;
};
struct ChainWatch { std::uintptr_t address{}; std::array<unsigned char,16> value{}; std::size_t size{}; };
struct WorldKey { std::uintptr_t world{},state{},context{}; bool operator==(const WorldKey&) const = default; };
struct Pool {
    Api api; Configuration configuration; NativeFrame owner;
    Handle body; std::array<Handle,collision_envelope::capsules.size()> shapes{};
    std::array<bool,collision_envelope::capsules.size()> shape_observed{};
    std::uintptr_t world{},native_state{},client_context{};
    DWORD client_thread{};
    bool attempted{},body_observed{},ready{},active{},parked{},detached{},failed{},blocked{};
    bool observation_suspended{},deferred_park{},owner_quarantined{};
    ReleaseWitness release_witness;
    ReleasedKey acquire_predecessor;
    owner_observation::RecreationSuffix recreation_suffix;
    unsigned attempts{},mounts{};
    std::uint64_t token{};
    Frame latest;
    std::uint64_t snapshot_reads{},snapshot_retries{},snapshot_recoveries{},snapshot_retry_exhaustions{};
    snapshot_retry::Issue last_snapshot_issue=snapshot_retry::Issue::none;
    snapshot_retry::VelocityRejections velocity_rejections; // retained across park/reset/invalidate
    const char* issue = "idle";
    std::array<WorldKey,16> attempted_worlds{}; std::size_t world_count{};
};
Pool& pool() { static auto* value = new Pool; return *value; }
struct Lease {
    Api api; Configuration configuration; Handle body; std::array<Handle,collision_envelope::capsules.size()> shapes{};
    std::uintptr_t world{},native_state{},client_context{},client{},entity{},core{},rig{},skater_context{};
    std::array<std::uintptr_t,32> wrappers{};
    std::array<ChainWatch,96> watches{}; std::size_t watch_count{};
    ULONGLONG until{}; std::uint64_t token{},serial{},hop_event{};
    double simulation_time{}; float step_seconds{};
    Vec force{},torque{},hop{}; bool active{};
};
struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<Synchronize> original{};
    std::atomic<bool> hook_ready{};
    bool hook_attempted{};
    Lease lease; Status info;
    ReleasedOwnershipStatus released;
    std::uint64_t last_hop{};
};
Shared& shared() { static auto* value = new Shared; return *value; }
struct LastError { DWORD value = GetLastError(); ~LastError() { SetLastError(value); } };
template<class T> bool read(std::uintptr_t p, T& out) noexcept { return memory::peek(p, out); }
bool pointer(std::uintptr_t p) noexcept { return p >= 0x10000 && p < memory::highest_user_address - 0x20000; }
template<std::size_t N> bool finite(const std::array<float, N>& v, float bound = 1000000) noexcept {
    for (float f : v) if (!std::isfinite(f) || std::abs(f) > bound) return false;
    return true;
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& f) noexcept {
    std::array<unsigned char, 32> actual{};
    return read(base + f.rva, actual) && actual == f.bytes;
}
bool resolve(std::uintptr_t base, Api& a) noexcept {
    if (!supported_build::running_image_matches(base)) return false;
    for (const auto* f : {&physics_world, &allocate_body, &set_motion_type, &set_body_980,
        &clear_body_aa0, &create_capsule, &set_categories, &set_collides_with, &set_shape_owner,
        &set_center_of_mass, &set_transform, &set_linear_velocity, &set_angular_velocity,
        &wake_body, &body_valid, &shape_valid, &linear_force, &angular_torque, &body_reset,
        &body_quaternion, &set_inertia_ratio, &overwrite_force, &overwrite_torque}) if (!matches(base, *f)) return false;
    a.base = base;
    a.world = reinterpret_cast<World>(base + physics_world.rva);
    a.allocate = reinterpret_cast<Allocate>(base + allocate_body.rva);
    a.capsule = reinterpret_cast<Capsule>(base + create_capsule.rva);
    a.motion = reinterpret_cast<SetInt>(base + set_motion_type.rva);
    a.inverse_mass = reinterpret_cast<SetFloat>(base + set_body_980.rva);
    a.clear_aa0 = reinterpret_cast<Touch>(base + clear_body_aa0.rva);
    a.wake = reinterpret_cast<Touch>(base + wake_body.rva);
    a.categories = reinterpret_cast<SetMask>(base + set_categories.rva);
    a.collides = reinterpret_cast<SetMask>(base + set_collides_with.rva);
    a.owner = reinterpret_cast<SetOwner>(base + set_shape_owner.rva);
    a.com = reinterpret_cast<SetVector>(base + set_center_of_mass.rva);
    a.transform = reinterpret_cast<SetVector>(base + set_transform.rva);
    a.linear = reinterpret_cast<SetVector>(base + set_linear_velocity.rva);
    a.angular = reinterpret_cast<SetVector>(base + set_angular_velocity.rva);
    a.force = reinterpret_cast<SetVector>(base + linear_force.rva);
    a.torque = reinterpret_cast<SetVector>(base + angular_torque.rva);
    a.inertia_ratio = reinterpret_cast<SetVector>(base + set_inertia_ratio.rva);
    a.body_valid = reinterpret_cast<Valid>(base + body_valid.rva);
    a.shape_valid = reinterpret_cast<Valid>(base + shape_valid.rva);
    a.overwrite_force = reinterpret_cast<SetVector>(base + overwrite_force.rva);
    a.overwrite_torque = reinterpret_cast<SetVector>(base + overwrite_torque.rva);
    return true;
}
// SEH wrappers are intentionally leaf functions with no C++ unwind objects.
bool world_of(World call, std::uintptr_t context, std::uintptr_t* result) noexcept {
    __try { *result = call(context); return pointer(*result); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool valid(Valid call, const Handle* h) noexcept {
    __try { return pointer(h->world) && h->index < 1048576 && call(h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool allocated_body(std::uintptr_t native_state, std::uint32_t index, bool require_dynamic = false) noexcept {
    // allocateBody266a6c0 sets state9c8[index>>5] bit(index&31).
    // setMotionType25daa90 and both force/torque accumulator helpers establish a08 as
    // the dynamic bitset. body_valid25d7b10 alone compares generation only.
    std::uint32_t capacity{}, capacity_again{}, allocated{}, allocated_again{}, dynamic{}, dynamic_again{};
    std::uintptr_t bits{}, bits_again{}, dynamic_bits{}, dynamic_bits_again{};
    if (!read(native_state+0x908,capacity) || !capacity || capacity > 1048576 || index >= capacity ||
        !read(native_state+0x9c8,bits) || !pointer(bits)) return false;
    const auto offset = std::uintptr_t{index>>5}*4;
    const auto bit = std::uint32_t{1} << (index&31U);
    if (!read(bits+offset,allocated) || !(allocated&bit)) return false;
    if (require_dynamic && (!read(native_state+0xa08,dynamic_bits) || !pointer(dynamic_bits) ||
        !read(dynamic_bits+offset,dynamic) || !(dynamic&bit))) return false;
    return read(native_state+0x908,capacity_again) && capacity_again == capacity &&
        read(native_state+0x9c8,bits_again) && bits_again == bits &&
        read(bits+offset,allocated_again) && allocated_again == allocated &&
        (!require_dynamic || (read(native_state+0xa08,dynamic_bits_again) && dynamic_bits_again == dynamic_bits &&
            read(dynamic_bits+offset,dynamic_again) && dynamic_again == dynamic));
}
bool allocate(const Api* a, std::uintptr_t world, Handle* h) noexcept {
    __try { return a->allocate(world, h) == h && h->world == world && valid(a->body_valid, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool integer(SetInt call, Handle* h, int value) noexcept {
    __try { call(h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool scalar(SetFloat call, Handle* h, float value) noexcept {
    __try { call(h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool touch(Touch call, Handle* h) noexcept {
    __try { call(h); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool vector(SetVector call, Handle* h, const float* value) noexcept {
    __try { call(h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool mask(SetMask call, Handle* h, std::uint32_t value) noexcept {
    __try { call(h, &value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool capsule(const Api* a, std::uintptr_t world, const Handle* body, Handle* shape,
    const float* bottom, const float* top, float radius) noexcept {
    __try { return a->capsule(world,shape,body,bottom,top,radius) == shape &&
        shape->world == world && valid(a->shape_valid,shape); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool clear_owner(SetOwner call, Handle* shape) noexcept {
    __try { const Handle nobody{shape->world, 0xffffffffU, 0}; call(shape, &nobody); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool client_context(std::uintptr_t base, std::uintptr_t client, std::uintptr_t context) noexcept {
    std::uint32_t index{}, phase{};
    std::uintptr_t tls{}, current{}, canonical{};
    std::uint8_t active{};
    if (!pointer(client) || !pointer(context) || !read(client + 8, canonical) || canonical != context ||
        !read(client + 0xc4, phase) || (phase != 13 && phase != 21) ||
        !read(base + addr::engine::tls_index, index) || index > 4095 ||
        !read(static_cast<std::uintptr_t>(__readgsqword(0x58)) + std::uintptr_t{index} * 8, tls) || !pointer(tls) ||
        !read(tls + 0xb19, active) || !active || !read(tls + 0x550, current) || current != context) return false;
    return true;
}
bool same_owner(const NativeFrame& a, const NativeFrame& b) noexcept {
    return a.available && b.available && a.client == b.client && a.entity == b.entity &&
        a.core == b.core && a.context == b.context && a.rig == b.rig;
}

bool allowed() noexcept {
    try {
        if (!user_data::runtime_development_selected()) return false;
        const auto mp = multiplayer::model();
        return !mp.active && !mp.lobby_joining;
    } catch (...) { return false; }
}
bool step_supported(float seconds) noexcept {
    return std::isfinite(seconds) && std::abs(static_cast<double>(seconds)-native_h) <= 1e-7;
}
double length(const Vec& v) noexcept {
    double value{}; for (float x : v) value += static_cast<double>(x)*x;
    return std::sqrt(value);
}
void cancel_lease() noexcept {
    auto& s = shared(); AcquireSRWLockExclusive(&s.lock);
    s.released.available = false;
    s.lease.active = false;
    ReleaseSRWLockExclusive(&s.lock);
}
void publish_info(Pool& p) noexcept {
    auto& s = shared(); AcquireSRWLockExclusive(&s.lock);
    s.info.ready = p.ready; s.info.active = p.active; s.info.parked = p.parked; s.info.detached = p.detached;
    s.info.blocked = p.blocked; s.info.observation_suspended = p.observation_suspended; s.info.deferred_park = p.deferred_park;
    s.info.owner_quarantined=p.owner_quarantined;
    s.info.failed = p.failed || s.info.failed; s.info.live_shapes = 0;
    for (bool observed : p.shape_observed) if (observed) ++s.info.live_shapes;
    s.info.allocation_attempts = p.attempts; s.info.mounts = p.mounts;
    s.info.world = p.world; s.info.issue = p.issue; s.info.allocation_cleanup_proven = false;
    s.info.snapshot_reads=p.snapshot_reads;s.info.snapshot_retries=p.snapshot_retries;
    s.info.snapshot_recoveries=p.snapshot_recoveries;s.info.snapshot_retry_exhaustions=p.snapshot_retry_exhaustions;
    s.info.last_snapshot_issue=p.last_snapshot_issue;
    s.info.velocity_rejections=p.velocity_rejections;
    // Pool is client-thread-owned. Only this scalar copy crosses to callbacks,
    // under the same existing status publication lock. No native freshness is
    // implied; cancellation keeps the copy unavailable during Pool mutations.
    s.released.available = !s.info.failed && !p.observation_suspended;
    s.released.ready = p.ready; s.released.active = p.active; s.released.detached = p.detached;
    s.released.failed = s.info.failed;
    s.released.client = p.owner.client; s.released.entity = p.owner.entity;
    s.released.core = p.owner.core; s.released.context = p.owner.context; s.released.rig = p.owner.rig;
    s.released.world = p.world; s.released.native_state = p.native_state;
    s.released.body_index = p.body.index; s.released.body_generation = p.body.generation;
    s.released.token = p.token;
    ReleaseSRWLockExclusive(&s.lock);
}
void invalidate(Pool& p, const char* issue) noexcept {
    cancel_lease(); p.active = p.parked = p.detached = p.ready = false;
    p.blocked = p.failed = true; p.issue = issue; ++p.token;
    p.observation_suspended = p.deferred_park = false;
    p.owner_quarantined=false;p.release_witness={};p.acquire_predecessor={};p.recreation_suffix.reset();
    // The engine owns lifetime. Never dereference these old-world handles again.
    p.body = {}; p.shapes = {}; p.shape_observed = {}; p.body_observed = false; p.latest = {};
    publish_info(p);
}
bool raw_world(const Api& api, const NativeFrame& f, WorldKey& key) noexcept {
    std::uintptr_t context{}, entity_context{};
    return f.available && finite(f.position) && read(f.client+8,context) && pointer(context) &&
        read(f.entity+0x20,entity_context) && entity_context == context &&
        client_context(api.base,f.client,context) && world_of(api.world,context,&key.world) &&
        read(key.world+0x28,key.state) && pointer(key.state) && (key.context = context) != 0;
}
bool current_world(Pool& p, const NativeFrame& f) noexcept {
    WorldKey actual;
    return GetCurrentThreadId() == p.client_thread && same_owner(p.owner,f) && raw_world(p.api,f,actual) &&
        actual == WorldKey{p.world,p.native_state,p.client_context};
}
bool own_body(Pool& p, const NativeFrame& f) noexcept {
    return p.body_observed && current_world(p,f) && p.body.world == p.world &&
        allocated_body(p.native_state,p.body.index) && valid(p.api.body_valid,&p.body) &&
        current_world(p,f) && allocated_body(p.native_state,p.body.index) && valid(p.api.body_valid,&p.body);
}
bool fresh_shape(Pool& p, const NativeFrame& f, std::size_t index) noexcept {
    return index < p.shapes.size() && own_body(p,f) && p.shapes[index].world == p.world &&
        valid(p.api.shape_valid,&p.shapes[index]) && current_world(p,f);
}
bool own_shape(Pool& p, const NativeFrame& f, std::size_t index) noexcept {
    if (index >= p.shapes.size() || !p.shape_observed[index] || !fresh_shape(p,f,index)) return false;
    std::uintptr_t owners{},categories{},with{}; std::uint32_t owner{},category{},collides{};
    const auto offset = std::uintptr_t{p.shapes[index].index}*4;
    return read(p.native_state+0x1008,owners) && pointer(owners) &&
        read(p.native_state+0xfe8,categories) && pointer(categories) && read(p.native_state+0xff0,with) && pointer(with) &&
        read(owners+offset,owner) && owner == 0xffffffffU && read(categories+offset,category) && category == 0 &&
        read(with+offset,collides) && (collides == 0 || collides == p.configuration.collides_with) &&
        fresh_shape(p,f,index);
}
bool own_pool(Pool& p, const NativeFrame& f) noexcept {
    if (!own_body(p,f)) return false;
    for (std::size_t i = 0; i < p.shapes.size(); ++i) if (!own_shape(p,f,i)) return false;
    return own_body(p,f);
}

ReleasedKey released_key(const Pool& p) noexcept {
    return {{p.owner.client,p.owner.entity,p.owner.core,p.owner.context,p.owner.rig},
        {p.world,p.native_state,p.client_context},p.body.index,p.body.generation,p.token};
}
bool released_permission(const Pool& p) noexcept {
    return p.attempted&&p.body_observed&&!p.deferred_park&&native_motion_healthy()&&
        owner_observation::may_suspend({p.ready,p.active,p.parked,p.detached,p.failed,p.blocked,status().failed})&&
        owner_observation::witness_matches(p.release_witness,released_key(p),p.parked,p.detached)&&
        std::all_of(p.shape_observed.begin(),p.shape_observed.end(),[](bool b){return b;});
}
bool quarantine_recreation(Pool& p,const NativeFrame& fresh) noexcept {
    if(!released_permission(p)||!fresh.available)return false;
    WorldKey key;const bool world_available=raw_world(p.api,fresh,key);
    const auto retained=released_key(p);
    const owner_observation::Owner owner{fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig};
    const auto classification=owner_observation::classify_recreation(retained.owner,retained.world,
        {true,world_available,owner,{key.world,key.state,key.context}});
    if(classification!=owner_observation::Recreation::candidate&&
       !(classification==owner_observation::Recreation::unknown&&
         owner_observation::same_actor(retained.owner,owner)&&
         owner.context!=retained.owner.context&&owner.rig!=retained.owner.rig))return false;
    cancel_lease();release_native_motion();
    if(status().failed)return false;
    p.owner_quarantined=p.observation_suspended=true;p.deferred_park=false;p.latest={};
    p.issue="released bike owner recreation quarantined; no native writes";publish_info(p);return true;
}

owner_observation::Result prove_fresh_owner(Pool& p, std::uintptr_t base,
    const NativeFrame& expected, NativeFrame& fresh, bool require_calibrated_step = true) noexcept {
    using Result = owner_observation::Result;
    fresh = {};
    if (!allowed() || base != p.api.base || GetCurrentThreadId() != p.client_thread || !expected.available)
        return Result::unavailable;
    fresh = read_native_frame(base,expected.client);
    if (!fresh.available) return Result::unavailable;
    // Only complete canonical reads establish a replacement. No retained
    // rider pointer or body/shape handle is dereferenced while unavailable.
    if (!same_owner(expected,fresh) || !same_owner(p.owner,fresh)) {
        if(quarantine_recreation(p,fresh))return Result::unavailable;
        return Result::changed;
    }
    WorldKey key;
    const bool world_available = raw_world(p.api,fresh,key);
    const auto result = owner_observation::evaluate(
        {p.owner.client,p.owner.entity,p.owner.core,p.owner.context,p.owner.rig},
        {p.world,p.native_state,p.client_context},
        {true,world_available,{fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig},
            {key.world,key.state,key.context}});
    if (result != Result::matched) return result;
    if (require_calibrated_step && (!fresh.timing_available || !step_supported(fresh.seconds)))
        return Result::unavailable;
    return Result::matched;
}
bool fresh_owner(Pool& p, std::uintptr_t base, const NativeFrame& expected, NativeFrame& fresh,
    bool require_calibrated_step = true) noexcept {
    return prove_fresh_owner(p,base,expected,fresh,require_calibrated_step) == owner_observation::Result::matched;
}
bool array4(std::uintptr_t native_state, std::uintptr_t offset, std::uint32_t index, V4& out,
    std::uintptr_t& source) noexcept {
    std::uintptr_t values{}, check{};
    if (!(read(native_state + offset, values) && pointer(values) &&
        read(values + std::uintptr_t{index} * 16, out) && finite(out) &&
        read(native_state + offset, check) && values == check)) return false;
    source = values;
    return true;
}
bool configured_body(std::uintptr_t native_state, std::uint32_t index, const Configuration& expected) noexcept {
    V4 ratio{}, ratio_again{};
    std::uintptr_t ratio_source{}, ratio_source_again{}, inverse_source{}, inverse_source_again{}, inverse_final{};
    float inverse{}, inverse_again{};
    return array4(native_state,0x988,index,ratio,ratio_source) && ratio == expected.inertia_ratio &&
        read(native_state+0x980,inverse_source) && pointer(inverse_source) &&
        read(inverse_source+std::uintptr_t{index}*4,inverse) && inverse == expected.inverse_mass &&
        array4(native_state,0x988,index,ratio_again,ratio_source_again) &&
        ratio_source_again == ratio_source && ratio_again == ratio &&
        read(native_state+0x980,inverse_source_again) && inverse_source_again == inverse_source &&
        read(inverse_source_again+std::uintptr_t{index}*4,inverse_again) && inverse_again == inverse &&
        read(native_state+0x980,inverse_final) && inverse_final == inverse_source;
}
using SnapshotIssue=snapshot_retry::Issue;
SnapshotIssue ring4(std::uintptr_t native_state, std::uintptr_t offset, std::uint32_t index, Ring& ring, V4& out) noexcept {
    if(!read(native_state+offset,ring))return SnapshotIssue::read_unavailable;
    if (!pointer(ring.values) || !ring.elements || ring.elements > 1048576 || index >= ring.elements ||
        !ring.planes || ring.planes > 64 || ring.current_plane >= ring.planes)return SnapshotIssue::invalid_ring;
    // Exact native arithmetic (25d5cc0 / reset2641070). elements is capacity;
    // current_plane is a u16 ring head, not a byte stride. Bounds above prohibit
    // the native 27-bit mask or 32-bit addition from discarding useful bits.
    const std::uint32_t quads = ring.elements * 4;
    const std::uint32_t plane_bytes = (((quads & 0x7ffffffU) * 4 + 0x3cU) & 0xffffffc0U);
    const auto address = ring.values + std::uintptr_t{plane_bytes} * ring.current_plane + std::uintptr_t{index} * 16;
    if(address<ring.values)return SnapshotIssue::invalid_ring;
    if(!read(address,out))return SnapshotIssue::read_unavailable;
    return finite(out)?SnapshotIssue::none:SnapshotIssue::invalid_ring;
}
SnapshotIssue valid_ring_value(std::size_t index,const V4& value) noexcept {
    if(index==1){
        float norm{};for(float x:value)norm+=x*x;
        if(!std::isfinite(norm)||norm<.9f||norm>1.1f)return SnapshotIssue::invalid_quaternion;
    }
    if(index>=2&&!finite(value,100))return SnapshotIssue::invalid_velocity;
    return SnapshotIssue::none;
}
void record_invalid_velocity(Pool& p,const snapshot_retry::Qualification* initial,unsigned attempt,unsigned pass,
        std::size_t index,const Ring& first,const Ring& rejected,const V4& value) noexcept {
    if(!initial)return; // allocation preflight has no qualified observe epoch
    snapshot_retry::record_velocity_rejection(p.velocity_rejections,*initial,attempt,pass,static_cast<unsigned>(index),
        {first.values,first.elements,first.current_plane,first.planes},
        {rejected.values,rejected.elements,rejected.current_plane,rejected.planes},value);
}
template<class Proof>
SnapshotIssue snapshot_guarded(Pool& s, Snapshot& out,const Configuration* configuration,
    const snapshot_retry::Qualification* initial,unsigned attempt,Proof&& proof) noexcept {
    if (!proof()) return SnapshotIssue::owner_changed;
    Snapshot v;
    v.native_state = s.native_state;
    V4 raw_quaternion{};
    constexpr std::array<std::uintptr_t, 4> offsets{0x900,0x918,0x930,0x940};
    const std::array<V4*,4> outputs{&v.position,&raw_quaternion,&v.linear,&v.angular};
    for(std::size_t i=0;i<offsets.size();++i){
        const auto issue=ring4(s.native_state,offsets[i],s.body.index,v.rings[i],*outputs[i]);
        if(issue!=SnapshotIssue::none)return issue;
        const auto invalid=valid_ring_value(i,*outputs[i]);
        if(invalid!=SnapshotIssue::none) {
            if(invalid==SnapshotIssue::invalid_velocity)record_invalid_velocity(s,initial,attempt,0,i,v.rings[i],v.rings[i],*outputs[i]);
            return invalid;
        }
    }
    if (!array4(s.native_state, 0x910, s.body.index, v.local_com, v.direct_arrays[0]) ||
        !array4(s.native_state, 0x928, s.body.index, v.local_quaternion_wxyz, v.direct_arrays[1]) ||
        !array4(s.native_state, 0x988, s.body.index, v.inertia_ratio, v.direct_arrays[2]))return SnapshotIssue::read_unavailable;
    std::uintptr_t inverse_masses{}, inverse_check{};
    if (!read(s.native_state + 0x980, inverse_masses) || !pointer(inverse_masses) ||
        !read(inverse_masses + std::uintptr_t{s.body.index} * 4, v.inverse_mass) ||
        !read(s.native_state + 0x980, inverse_check))return SnapshotIssue::read_unavailable;
    if(!std::isfinite(v.inverse_mass)||v.inverse_mass<=0||v.inverse_mass>1000)return SnapshotIssue::invalid_mass;
    if(inverse_check!=inverse_masses)return SnapshotIssue::direct_array_changed;
    if(configuration&&(v.inverse_mass!=configuration->inverse_mass||v.inertia_ratio!=configuration->inertia_ratio))
        return SnapshotIssue::configuration_changed;
    v.direct_arrays[3] = inverse_masses;
    // Direct SoA pose is used only for this newly allocated zero-COM,
    // identity-local-frame body. Shape pose composition is a different helper.
    if (v.local_com != V4{} || v.local_quaternion_wxyz != V4{1,0,0,0})return SnapshotIssue::invalid_local_frame;
    v.quaternion_xyzw = {raw_quaternion[1], raw_quaternion[2], raw_quaternion[3], raw_quaternion[0]};
    const std::array<V4, 4> expected{v.position,raw_quaternion,v.linear,v.angular};
    SnapshotIssue raced=SnapshotIssue::none;
    // Header stability alone does not exclude a native in-place write. Require
    // equal second content reads and reroot every source before accepting the
    // diagnostic sample. This is not an engine-wide atomic snapshot guarantee.
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        Ring again;
        V4 data{};
        const auto issue=ring4(s.native_state,offsets[i],s.body.index,again,data);
        if(issue!=SnapshotIssue::none)return issue;
        const auto invalid=valid_ring_value(i,data);
        if(invalid!=SnapshotIssue::none) {
            if(invalid==SnapshotIssue::invalid_velocity)record_invalid_velocity(s,initial,attempt,1,i,v.rings[i],again,data);
            return invalid;
        }
        if(again!=v.rings[i])raced=SnapshotIssue::ring_header_changed;
        else if(data!=expected[i]&&raced==SnapshotIssue::none)raced=SnapshotIssue::ring_content_changed;
    }
    constexpr std::array<std::uintptr_t, 3> direct_offsets{0x910,0x928,0x988};
    const std::array<V4, 3> direct_expected{v.local_com,v.local_quaternion_wxyz,v.inertia_ratio};
    for (std::size_t i = 0; i < direct_offsets.size(); ++i) {
        V4 data{};
        std::uintptr_t source{};
        if(!array4(s.native_state,direct_offsets[i],s.body.index,data,source))return SnapshotIssue::read_unavailable;
        if(source!=v.direct_arrays[i]||data!=direct_expected[i])return SnapshotIssue::direct_array_changed;
    }
    float inverse_again{};
    if(!read(s.native_state+0x980,inverse_check)||!pointer(inverse_check)||
       !read(inverse_check+std::uintptr_t{s.body.index}*4,inverse_again)||!read(s.native_state+0x980,inverse_masses))
        return SnapshotIssue::read_unavailable;
    if(!std::isfinite(inverse_again)||inverse_again<=0||inverse_again>1000)return SnapshotIssue::invalid_mass;
    if(inverse_check!=v.direct_arrays[3]||inverse_again!=v.inverse_mass||inverse_masses!=inverse_check)
        return SnapshotIssue::direct_array_changed;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        Ring again;V4 data{};
        const auto issue=ring4(s.native_state,offsets[i],s.body.index,again,data);
        if(issue!=SnapshotIssue::none)return issue;
        const auto invalid=valid_ring_value(i,data);
        if(invalid!=SnapshotIssue::none) {
            if(invalid==SnapshotIssue::invalid_velocity)record_invalid_velocity(s,initial,attempt,2,i,v.rings[i],again,data);
            return invalid;
        }
        if(again!=v.rings[i])raced=SnapshotIssue::ring_header_changed;
        else if(data!=expected[i]&&raced==SnapshotIssue::none)raced=SnapshotIssue::ring_content_changed;
    }
    if (!proof())return SnapshotIssue::owner_changed;
    if(raced!=SnapshotIssue::none)return raced;
    out = v;
    return SnapshotIssue::none;
}
SnapshotIssue snapshot(Pool& s,const NativeFrame& f,Snapshot& out,const Configuration* configuration=nullptr,
    const snapshot_retry::Qualification* initial=nullptr,unsigned attempt=0) noexcept {
    return snapshot_guarded(s,out,configuration,initial,attempt,[&]() noexcept{return own_body(s,f);});
}


bool watched_chain(const Lease&) noexcept;
template<class T> T chain_read(client_source::detail::SourceReader& reader, Lease& l,
    std::uintptr_t address) {
    client_source::detail::source_require(l.watch_count < l.watches.size(),"Probe chain watch bound exceeded");
    const auto value = reader.value<T>(address);
    ChainWatch watch{address,{},sizeof(T)};
    std::memcpy(watch.value.data(),&value,sizeof(T));
    l.watches[l.watch_count++] = watch;
    return value;
}

bool watched_chain(const Lease& l) noexcept {
    if (!l.watch_count || l.watch_count > l.watches.size()) return false;
    for (std::size_t i = 0; i < l.watch_count; ++i) {
        std::array<unsigned char,16> actual{};
        const auto& w = l.watches[i];
        if (!w.size || w.size > actual.size() || !memory::peek_bytes(w.address,actual.data(),w.size) || actual != w.value) return false;
    }
    return true;
}
bool producer_owner(const Lease& l, const Handle& incoming) noexcept {
    std::uintptr_t world{},state{},owners{},categories{},with{};
    if (!watched_chain(l) || !client_context(l.api.base,l.client,l.client_context) ||
        !world_of(l.api.world,l.client_context,&world) || world != l.world || incoming.world != world ||
        !read(world+0x28,state) || state != l.native_state || l.body.world != world ||
        !allocated_body(state,incoming.index) || !allocated_body(state,l.body.index,true) ||
        !valid(l.api.body_valid,&incoming) || !valid(l.api.body_valid,&l.body) ||
        !configured_body(state,l.body.index,l.configuration) ||
        !read(state+0x1008,owners) || !pointer(owners) || !read(state+0xfe8,categories) || !pointer(categories) ||
        !read(state+0xff0,with) || !pointer(with)) return false;
    for (const auto& shape : l.shapes) {
        std::uint32_t owner{},category{},collides{};
        const auto offset = std::uintptr_t{shape.index}*4;
        if (shape.world != world || !valid(l.api.shape_valid,&shape) || !read(owners+offset,owner) || owner != 0xffffffffU ||
            !read(categories+offset,category) || category != l.configuration.categories ||
            !read(with+offset,collides) || collides != l.configuration.collides_with) return false;
    }
    return watched_chain(l) && read(world+0x28,state) && state == l.native_state &&
        allocated_body(state,incoming.index) && allocated_body(state,l.body.index,true) &&
        valid(l.api.body_valid,&incoming) && valid(l.api.body_valid,&l.body) &&
        configured_body(state,l.body.index,l.configuration);
}
bool producer_clock(const Lease& l) noexcept {
    std::uint64_t serial{}; double time{}; float h{};
    return read_native_clock(l.entity,l.core,serial,time) && serial == l.serial &&
        std::isfinite(time) && time == l.simulation_time &&
        read(l.skater_context+0x17ec,h) && step_supported(h);
}
void produce(Handle* incoming_pointer, std::uintptr_t wrapper) noexcept {
    auto& s = shared();
    if (!s.hook_ready.load(std::memory_order_acquire)) return;
    if (!TryAcquireSRWLockExclusive(&s.lock)) return;
    ++s.info.producer_calls;
    const auto work = [&]() noexcept {
        auto& l = s.lease;
        if (!l.active || GetTickCount64() >= l.until || s.info.failed) return;
        bool wrapper_match{};
        for (auto expected : l.wrappers) if (expected == wrapper) { wrapper_match = true; break; }
        if (!wrapper_match) return;
        ++s.info.wrapper_matches;
        Handle incoming;
        if (!read(reinterpret_cast<std::uintptr_t>(incoming_pointer),incoming) || !producer_owner(l,incoming)) {
            ++s.info.rejected; s.info.issue = "producer owner verification rejected"; return;
        }
        std::uint64_t serial{}; double time{}; float h{};
        if (!read_native_clock(l.entity,l.core,serial,time) || serial != l.serial || !std::isfinite(time) ||
            time != l.simulation_time ||
            !read(l.skater_context+0x17ec,h) || !step_supported(h) || !step_supported(l.step_seconds)) {
            ++s.info.rejected; s.info.issue = "producer requires verified60Hz native clock"; return;
        }
        if (serial <= s.info.last_serial) { ++s.info.duplicate_steps; return; }
        Vec force = l.force;
        const bool hop = l.hop_event && l.hop_event > s.last_hop;
        if (hop) for (std::size_t i = 0; i < 3; ++i) force[i] += l.hop[i]/h;
        if (!finite(force) || length(force) > maximum_force || !finite(l.torque) || length(l.torque) > maximum_torque) {
            ++s.info.rejected; l.active = false; s.info.failed = true; s.released.available = false;
            s.info.issue = "bounded chassis force rejected"; return;
        }
        // Consume before any native call. A partial failure never retries the
        // same step or hop; the client parks/abandons this pool on failure.
        l.active = false;
        s.info.last_serial = serial;
        if (hop) { s.last_hop = l.hop_event; ++s.info.hop_events; }
        alignas(16) const V4 f{force[0],force[1],force[2],0}, t{l.torque[0],l.torque[1],l.torque[2],0};
        const bool ok = producer_clock(l) && producer_owner(l,incoming) && GetTickCount64() < l.until &&
            vector(l.api.force,&l.body,f.data()) && producer_owner(l,incoming) && producer_clock(l) &&
            GetTickCount64() < l.until && vector(l.api.torque,&l.body,t.data()) && producer_owner(l,incoming);
        s.info.last_force = force; s.info.last_torque = l.torque;
        if (ok) { ++s.info.applications; s.info.issue = "native chassis force submitted"; }
        else { ++s.info.rejected; l.active = false; s.info.failed = true; s.released.available = false;
            s.info.issue = "partial native chassis force failure"; }
    };
    work();
    ReleaseSRWLockExclusive(&s.lock);
}
std::uintptr_t synchronize_hook(Handle* handle, std::uintptr_t wrapper) {
    const auto original = shared().original.load(std::memory_order_acquire);
    const auto incoming_error=GetLastError();
    std::uintptr_t result{};
    {
        DINGO_PROFILE_ZONE("native/BMX/retail synchronize");
        SetLastError(incoming_error);
        result = original(handle,wrapper);
    }
    const auto error = GetLastError();
    {
        DINGO_PROFILE_ZONE("hooks/BMX/retail producer");
        produce(handle,wrapper);
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
        native_coupled::detail::produce(handle,wrapper);
#endif
    }
    SetLastError(error); return result;
}
bool install_hook(Pool& p) noexcept {
    auto& s = shared();
    if (s.hook_attempted) return s.hook_ready.load(std::memory_order_acquire);
    s.hook_attempted = true;
    if (!matches(p.api.base,synchronizer)) return false;
    const auto init = hook_initialize();
    if (init != HookOk && init != HookAlreadyInitialized) return false;
    void* original{}; auto* target = reinterpret_cast<void*>(p.api.base+synchronizer.rva);
    if (hook_prepare(target,reinterpret_cast<void*>(&synchronize_hook),&original) != HookOk || !original) return false;
    s.original.store(reinterpret_cast<Synchronize>(original),std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    s.hook_ready.store(true,std::memory_order_release); return true;
}
bool make_lease(Pool& p, const NativeFrame& frame, Lease& result) noexcept {
    using namespace client_source::detail;
    try {
        if (!own_pool(p,frame)) return false;
        auto& sources = source_state();
        if (!sources.initialized.load(std::memory_order_acquire) || sources.trial.base != p.api.base ||
            sources.busy.test_and_set(std::memory_order_acquire)) return false;
        SourceBusyScope busy{sources.busy};
        const auto bodies = debug_noclip_bodies(p.api.base,frame.client,frame.entity);
        if (bodies.core != frame.core || bodies.context != frame.context || bodies.rig_wrapper != frame.rig) return false;
        Lease l; l.api = p.api; l.configuration = p.configuration; l.body = p.body; l.shapes = p.shapes;
        l.world = p.world; l.native_state = p.native_state; l.client_context = p.client_context;
        l.client = frame.client; l.entity = frame.entity; l.core = frame.core; l.rig = frame.rig; l.skater_context = frame.context;
        SourceReader reader;
        const auto ptr = [&](std::uintptr_t at) { return chain_read<std::uintptr_t>(reader,l,at); };
        const auto u32 = [&](std::uintptr_t at) { return chain_read<std::uint32_t>(reader,l,at); };
        const auto u8 = [&](std::uintptr_t at) { return chain_read<std::uint8_t>(reader,l,at); };
        source_require(ptr(frame.client) == p.api.base+addr::engine::client_vtable &&
            ptr(frame.client+8) == p.client_context,"Probe client changed");
        const auto phase = u32(frame.client+0xc4);
        source_require(phase == 13 || phase == 21,"Probe client loading");
        const auto offset = u32(p.api.base+addr::engine::context_player_manager_offset);
        source_require(offset <= 0x1000000,"Probe player manager offset changed");
        const auto manager = ptr(p.client_context+offset);
        source_require(ptr(manager) == p.api.base+addr::engine::local_player_manager_vtable,"Probe player manager changed");
        const auto begin = ptr(manager+0x4c8), end = ptr(manager+0x4d0);
        source_require(reader.count(begin,end,8) == 1,"Probe needs unique local player");
        const auto player = ptr(begin);
        source_require(ptr(player) == p.api.base+addr::engine::local_player_vtable && ptr(player+0x78) == p.client_context &&
            u8(player+0x45) == 1 && u8(player+0x44) == 0 && ptr(player+0xb8) == frame.entity,"Probe local player changed");
        const auto handle = ptr(player+0xb0);
        source_require(ptr(frame.entity) == p.api.base+addr::engine::skater_entity_vtable &&
            ptr(frame.entity+0x20) == p.client_context && ptr(frame.entity+0xf8) == player && ptr(handle) == frame.entity+8 &&
            u8(frame.entity+0x7e0) == 0,"Probe player entity changed");
        const auto collection = ptr(frame.entity+0x70), component = ptr(frame.entity+0x628);
        source_require(ptr(collection) == frame.entity && ptr(component) == p.api.base+addr::engine::skater_component_vtable &&
            ptr(component+0x18) == collection && ptr(component+0x70) == frame.core &&
            ptr(frame.core) == p.api.base+addr::no_bail::bail_core_vtable && ptr(frame.core+0x3c0) == frame.context &&
            ptr(frame.core+0x438) == frame.rig && ptr(frame.rig) == frame.context && ptr(frame.rig+0x4630) == frame.core,
            "Probe physics owner changed");
        const auto board_link = ptr(frame.core+0x430), board = ptr(board_link+0x18), rig = ptr(frame.rig+0x2f10);
        source_require(ptr(board) == p.api.base+spawn::board_physics_vtable && ptr(rig) == p.api.base+spawn::rig_physics_vtable,
            "Probe physics container changed");
        const auto board_parts = ptr(board+0x20), rig_parts = ptr(rig+0x20);
        source_require(u32(board_parts) == 9 && u32(rig_parts) == 26,"Probe physics layout changed");
        for (std::size_t i = 0; i < l.wrappers.size(); ++i) {
            const auto part = i < 9 ? board_parts+i*0x130 : rig_parts+(i-8)*0x130;
            source_require(part == bodies.parts[i] && ptr(part+0x10) == (i < 9 ? board : rig),"Probe body wrapper changed");
            // Hypothesis from matching flags offsets: native part+.60 versus
            // SynchronizeBody arg2+.50. Never act unless observed exactly.
            l.wrappers[i] = part+0x10;
        }
        reader.verify();
        l.active = true;
        if (!own_pool(p,frame) || !watched_chain(l)) return false;
        result = l; return true;
    } catch (...) { return false; }
}
bool pending_zero(Pool& p) noexcept {
    V4 linear{},angular{}; std::uintptr_t a{},b{};
    return array4(p.native_state,0x970,p.body.index,linear,a) && linear == V4{} &&
        array4(p.native_state,0x978,p.body.index,angular,b) && angular == V4{};
}
bool quiesce(Pool& p, const NativeFrame& f) noexcept {
    cancel_lease();
    if (!own_body(p,f)) return false;
    for (std::size_t i = 0; i < p.shapes.size(); ++i) if (p.shape_observed[i]) {
        if (!own_shape(p,f,i) || !mask(p.api.categories,&p.shapes[i],0) || !own_shape(p,f,i) ||
            !mask(p.api.collides,&p.shapes[i],0) || !own_shape(p,f,i)) return false;
    }
    alignas(16) const V4 zero{};
    // These identified overwrite helpers are dynamic-only. Clear accelerations
    // while still dynamic, then velocities, then park as keyframed. No direct
    // stores to native pending arrays are used.
    if (allocated_body(p.native_state,p.body.index,true) &&
        (!vector(p.api.overwrite_force,&p.body,zero.data()) || !own_body(p,f) ||
         !vector(p.api.overwrite_torque,&p.body,zero.data()) || !own_body(p,f))) return false;
    if (!pending_zero(p) || !vector(p.api.linear,&p.body,zero.data()) || !own_body(p,f) ||
        !vector(p.api.angular,&p.body,zero.data()) || !own_body(p,f) ||
        !integer(p.api.motion,&p.body,2) || !own_body(p,f) || !pending_zero(p)) return false;
    p.active = p.detached = false; p.parked = true; p.latest = {}; return true;
}
bool create_pool(Pool& p, const NativeFrame& f) noexcept {
    p.attempted = true; ++p.attempts;
    if (p.world_count >= p.attempted_worlds.size()) return false;
    p.attempted_worlds[p.world_count++] = {p.world,p.native_state,p.client_context};
    if (!allocate(&p.api,p.world,&p.body)) return false;
    p.body_observed = true;
    Snapshot defaults;
    if (snapshot(p,f,defaults)!=SnapshotIssue::none || defaults.inverse_mass != 1 || defaults.inertia_ratio != V4{1,1,1,0}) return false;
    alignas(16) const V4 zero{}, ratio = p.configuration.inertia_ratio;
    if (!integer(p.api.motion,&p.body,2) || !own_body(p,f) ||
        !scalar(p.api.inverse_mass,&p.body,p.configuration.inverse_mass) || !own_body(p,f) ||
        !vector(p.api.inertia_ratio,&p.body,ratio.data()) || !own_body(p,f) ||
        !configured_body(p.native_state,p.body.index,p.configuration) ||
        !touch(p.api.clear_aa0,&p.body) || !own_body(p,f) ||
        !vector(p.api.com,&p.body,zero.data()) || !own_body(p,f)) return false;
    // Constructor association is explicit via its body argument. No guessed
    // shape-to-body array offsets or universal character exclusion are claimed.
    // The rolling tire constraint uses the canonical geometry radius. These
    // rigid attached backups do not have native axle spin; their outer extent
    // retains a 30mm recess inside that tire and its existing penetration guard.
    for (std::size_t i = 0; i < p.shapes.size(); ++i) {
        const auto& shape=collision_envelope::capsules[i];
        alignas(16) V4 bottom{},top{};
        for(unsigned axis=0;axis<3;++axis) {
            bottom[axis]=shape.a[axis]-collision_envelope::com_from_root[axis];
            top[axis]=shape.b[axis]-collision_envelope::com_from_root[axis];
        }
        if (!own_body(p,f) || !capsule(&p.api,p.world,&p.body,&p.shapes[i],bottom.data(),top.data(),shape.radius) ||
            !fresh_shape(p,f,i)) return false;
        for (std::size_t j = 0; j < i; ++j) if (p.shapes[i] == p.shapes[j]) return false;
        if (!clear_owner(p.api.owner,&p.shapes[i]) || !fresh_shape(p,f,i) ||
            !mask(p.api.categories,&p.shapes[i],0) || !fresh_shape(p,f,i) ||
            !mask(p.api.collides,&p.shapes[i],0) || !fresh_shape(p,f,i)) return false;
        p.shape_observed[i] = true;
        if (!own_shape(p,f,i)) return false;
    }
    p.ready = true; p.parked = true; return own_pool(p,f) && pending_zero(p);
}
bool activate(Pool& p, const NativeFrame& f) noexcept {
    if (!own_pool(p,f) || !configured_body(p.native_state,p.body.index,p.configuration) || !pending_zero(p)) return false;
    for (std::size_t i = 0; i < p.shapes.size(); ++i) {
        if (!mask(p.api.categories,&p.shapes[i],p.configuration.categories) || !own_shape(p,f,i) ||
            !mask(p.api.collides,&p.shapes[i],p.configuration.collides_with) || !own_shape(p,f,i)) return false;
    }
    if (!integer(p.api.motion,&p.body,3) || !own_pool(p,f) ||
        !allocated_body(p.native_state,p.body.index,true) || !touch(p.api.wake,&p.body) || !own_pool(p,f)) return false;
    p.active = true; p.parked = p.detached = false; p.latest = {}; return true;
}
void reset_world(Pool& p, const WorldKey& key, const NativeFrame& owner) noexcept {
    cancel_lease(); p.body = {}; p.shapes = {}; p.shape_observed = {}; p.body_observed = false;
    p.ready = p.active = p.parked = p.detached = p.failed = p.blocked = p.attempted = false;
    p.observation_suspended = p.deferred_park = false;
    p.owner_quarantined=false;p.release_witness={};p.acquire_predecessor={};p.recreation_suffix.reset();
    p.world = key.world; p.native_state = key.state; p.client_context = key.context; p.owner = owner; p.latest = {};
    for (std::size_t i = 0; i < p.world_count; ++i) if (p.attempted_worlds[i] == key) p.blocked = p.attempted = true;
    // The old producer lease is canceled under this same lock before changing
    // worlds. Its terminal failure must not poison a fresh world's retryable
    // mount-support wait. The full-key allocation history still fences reuse.
    auto& s = shared(); AcquireSRWLockExclusive(&s.lock);
    s.info.failed = false; s.released.available = false;
    ReleaseSRWLockExclusive(&s.lock);
}
} // namespace

MountAdmission mount_admission(std::uintptr_t base, const NativeFrame& rider) noexcept {
    LastError error; auto& p = pool();
    try {
        if (!allowed() || !base || !rider.available) return {};
        if (p.observation_suspended) return {AdmissionDecision::wait,
            "BMX waiting for fresh same-rider physics observation; all BMX commands are revoked."};
        if ((p.api.base && p.api.base != base) ||
            (p.client_thread && GetCurrentThreadId() != p.client_thread)) return {};
        // The first query must not initialize the API/hook or allocate native
        // resources. Qualify just the world getter in local scratch instead.
        Api query_api = p.api;
        if (!query_api.base) {
            if (!supported_build::running_image_matches(base) || !matches(base,physics_world)) return {};
            query_api.base = base;
            query_api.world = reinterpret_cast<World>(base+physics_world.rva);
        }
        const auto fresh = read_native_frame(base,rider.client);
        WorldKey key,confirmed_key;
        if (!same_owner(rider,fresh) || !raw_world(query_api,fresh,key)) return {};
        const auto confirmed = read_native_frame(base,rider.client);
        if (!same_owner(fresh,confirmed) || !raw_world(query_api,confirmed,confirmed_key) || key != confirmed_key) return {};
        if(p.owner.available&&!same_owner(p.owner,confirmed)&&quarantine_recreation(p,confirmed))
            return {AdmissionDecision::wait,"BMX waiting for released bike owner custody."};
        // Pool is client-thread-owned. The callback's failure copy is read
        // under its publication lock and applies only to this exact key.
        std::array<pool_admission::WorldKey,16> attempted{};
        if (p.world_count > attempted.size()) return {};
        for (std::size_t i=0;i<p.world_count;++i)
            attempted[i]={p.attempted_worlds[i].world,p.attempted_worlds[i].state,p.attempted_worlds[i].context};
        const pool_admission::Owner owner = p.owner.available ?
            pool_admission::Owner{p.owner.client,p.owner.entity,p.owner.core,p.owner.context,p.owner.rig} : pool_admission::Owner{};
        const pool_admission::PoolView view{{p.world,p.native_state,p.client_context},owner,
            p.attempted,p.ready,p.active,p.detached,p.failed,p.blocked,status().failed,
            std::span<const pool_admission::WorldKey>{attempted.data(),p.world_count}};
        return pool_admission::evaluate(view,{key.world,key.state,key.context},
            {confirmed.client,confirmed.entity,confirmed.core,confirmed.context,confirmed.rig});
    } catch (...) { return {}; }
}

bool acquire(std::uintptr_t base, const NativeFrame& rider, const Vec& root, float yaw, const mount_pose::Seed* seed) noexcept {
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
    if (native_coupled::detail::allocation_claimed()) return false;
#endif
    LastError error;
    const auto key_predecessor=pool().acquire_predecessor;
    const ReleaseWitness retained_predecessor{key_predecessor,key_predecessor.token!=0,true,false};
    revoke_released_witness();
    auto& p = pool();
    try {
        if (!allowed() || !rider.available || !finite(root) || !std::isfinite(yaw)) return false;
        Quat seed_rotation{};
        if (seed && !chassis::unit_quaternion(seed->orientation,seed_rotation)) return false;
        if (!p.api.base) {
            if (!resolve(base,p.api) || !enable_native_motion(base)) { p.issue = "native chassis API unavailable"; publish_info(p); return false; }
            p.client_thread = GetCurrentThreadId();
            if (!install_hook(p)) { p.issue = "retail producer hook unavailable"; publish_info(p); return false; }
        }
        if (p.api.base != base || GetCurrentThreadId() != p.client_thread || !shared().hook_ready.load()) return false;
        const auto fresh = read_native_frame(base,rider.client); WorldKey key;
        if(!same_owner(rider,fresh)&&fresh.available) {
            p.release_witness=retained_predecessor;
            const bool quarantined=quarantine_recreation(p,fresh);
            p.release_witness={}; // cancellation-only; runtime must provide a new idle witness
            if(quarantined)return false;
        }
        if (!same_owner(rider,fresh) || !fresh.timing_available || !step_supported(fresh.seconds) || !raw_world(p.api,fresh,key)) return false;
        const WorldKey previous{p.world,p.native_state,p.client_context};
        if (key != previous) reset_world(p,key,fresh);
        else if (p.owner.available && !same_owner(p.owner,fresh)) {
            p.release_witness=retained_predecessor;
            const bool quarantined=quarantine_recreation(p,fresh);p.release_witness={};
            if(quarantined)return false;
            invalidate(p,"same-world rider replacement; pool access abandoned");return false;
        }
        if (seed && (!mount_pose::matches(*seed,key.world,key.context,fresh.serial,fresh.simulation_time) ||
            seed->root != root || std::abs(std::remainder(seed->request.yaw-yaw,6.283185307179586f)) > .00001f)) {
            p.issue = "fresh mount support lease unavailable"; publish_info(p); return false;
        }
        if (p.blocked || p.failed || (p.attempted && !p.ready)) { p.issue = "same-world chassis allocation cannot be retried"; publish_info(p); return false; }
        if (p.observation_suspended) { p.issue = "chassis observation suspended; acquisition deferred"; publish_info(p); return false; }
        if (p.active || p.detached) { p.issue = "chassis must be parked before acquisition"; publish_info(p); return false; }
        cancel_lease(); // Invalidate the callback mirror before owner/body/token mutations.
        p.owner = fresh;
        if (!p.attempted && !create_pool(p,fresh)) {
            // A confirmed partial pool may be stopped, but never replaced.
            (void)quiesce(p,fresh); p.failed = p.blocked = true; p.issue = "partial chassis allocation; retry prohibited"; publish_info(p); return false;
        }
        if (!quiesce(p,fresh)) { invalidate(p,"chassis ownership lost before mount seed"); return false; }
        const float half = yaw*.5f;
        alignas(16) std::array<float,8> transform{0,std::sin(half),0,std::cos(half),
            root[0]-.05f*std::sin(yaw),root[1]+.9f,root[2]-.05f*std::cos(yaw),1};
        if (seed) {
            const auto offset=chassis::rotate_unit(seed_rotation,geometry::com_from_root);
            transform={seed_rotation[0],seed_rotation[1],seed_rotation[2],seed_rotation[3],
                root[0]+offset[0],root[1]+offset[1],root[2]+offset[2],1};
        }
        const auto impact_variant=impact_test::variant();
        if(impact_variant&&p.mounts==0) {
            // Explicit isolated diagnostic overrides the ordinary terrain seed.
            // Preserve the previous yaw-based COM/drop and orientation exactly.
            transform={0,std::sin(half),0,std::cos(half),
                root[0]-.05f*std::sin(yaw),root[1]+.9f,root[2]-.05f*std::cos(yaw),1};
            // Hamilton yaw*X(pi) or yaw*Z(pi/2), seed once before activation.
            const float sine=std::sin(half),cosine=std::cos(half),h=.70710678118f;
            if(impact_variant==1) {transform[0]=cosine;transform[1]=0;transform[2]=-sine;transform[3]=0;}
            else {transform[0]=sine*h;transform[1]=sine*h;transform[2]=cosine*h;transform[3]=cosine*h;}
            transform[5]=root[1]+1.85f;
        }
        const auto seed_current = [&] {
            std::uint64_t serial{}; double seconds{};
            return !seed || (read_native_clock(fresh.entity,fresh.core,serial,seconds) &&
                serial==seed->request.serial && seconds==seed->request.simulation_time);
        };
        if (!seed_current()) {
            p.issue = "mount support clock changed before seed"; publish_info(p); return false;
        }
        if (!own_pool(p,fresh) || !vector(p.api.transform,&p.body,transform.data()) || !own_pool(p,fresh)) {
            (void)quiesce(p,fresh); p.failed = p.blocked = true; p.issue = "native chassis seed failed"; publish_info(p); return false;
        }
        if (!seed_current()) {
            (void)quiesce(p,fresh); p.issue = "mount support clock changed before activation"; publish_info(p); return false;
        }
        if (!activate(p,fresh)) {
            (void)quiesce(p,fresh); p.failed = p.blocked = true; p.issue = "native chassis activation failed"; publish_info(p); return false;
        }
        ++p.token; ++p.mounts;
        auto& s = shared(); AcquireSRWLockExclusive(&s.lock);
        s.info.failed = false; s.info.last_serial = 0; s.last_hop = 0; ReleaseSRWLockExclusive(&s.lock);
        p.issue = "native pooled chassis active"; publish_info(p); return true;
    } catch (...) { invalidate(p,"native chassis acquisition failed closed"); return false; }
}
Frame observe(std::uintptr_t base, const NativeFrame& rider, NativeFrame* coherent_rider, bool allow_suspension) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<NativeFrame>);
    LastError error; auto& p = pool(); Frame result;
    const bool aliases_hint = coherent_rider == &rider;
    if (coherent_rider) *coherent_rider = {};
    if (aliases_hint) { result.issue = "native chassis rider output aliases its owner hint"; return result; }
    try {
        if (!p.ready || p.blocked || p.failed) { result.issue = p.issue; return result; }
        if (p.observation_suspended) { result.issue = "native chassis observation suspended"; result.snapshot_issue = snapshot_retry::Issue::read_unavailable; return result; }
        NativeFrame fresh;
        const auto qualify=[&]() noexcept {
            snapshot_retry::Qualification q;
            NativeFrame current;
            // Every retry reroots the complete current owner/world/body/shape
            // chain. An ownership failure is terminal, never a race label.
            const auto owner_proof=prove_fresh_owner(p,base,rider,current,false);
            if(owner_proof==owner_observation::Result::unavailable) {
                if(!allow_suspension||!owner_observation::may_suspend(
                    {p.ready,p.active,p.parked,p.detached,p.failed,p.blocked,status().failed})) {
                    invalidate(p,"active chassis owner observation unavailable");q.issue=SnapshotIssue::owner_changed;
                } else {suspend();q.issue=SnapshotIssue::read_unavailable;}
                return q;
            }
            if(owner_proof==owner_observation::Result::changed||!own_pool(p,current)){
                invalidate(p,"native chassis owner changed");q.issue=SnapshotIssue::owner_changed;return q;
            }
            if(status().failed){p.failed=true;p.issue="native chassis producer failed";publish_info(p);q.issue=SnapshotIssue::producer_failed;return q;}
            q.calibrated=current.timing_available&&step_supported(current.seconds);
            if(!p.detached&&!q.calibrated){q.issue=SnapshotIssue::clock_changed;return q;}
            if(!configured_body(p.native_state,p.body.index,p.configuration)){q.issue=SnapshotIssue::configuration_changed;return q;}
            std::uint64_t serial{};double time{};
            if(!read_native_clock(current.entity,current.core,serial,time)||serial!=current.serial||
               !std::isfinite(time)||time!=current.simulation_time){q.issue=SnapshotIssue::clock_changed;return q;}
            q.epoch.owner_world={current.client,current.entity,current.core,current.context,current.rig,
                p.world,p.native_state,p.client_context};
            q.epoch.body_index=p.body.index;q.epoch.body_generation=p.body.generation;
            q.epoch.token=p.token;q.epoch.serial=serial;q.epoch.time=time;q.epoch.seconds=current.seconds;
            q.valid=true;q.issue=SnapshotIssue::none;fresh=std::move(current);return q;
        };
        const auto initial=qualify();Snapshot body;unsigned snapshot_attempt{};
        const auto observed=snapshot_retry::run(initial,qualify,[&]() noexcept {
            return snapshot(p,fresh,body,&p.configuration,&initial,++snapshot_attempt);
        });
        p.snapshot_reads+=observed.reads;
        if(observed.reads>1)p.snapshot_retries+=observed.reads-1;
        if(observed.accepted&&observed.reads>1)++p.snapshot_recoveries;
        if(observed.exhausted)++p.snapshot_retry_exhaustions;
        p.last_snapshot_issue=observed.accepted?observed.last_race:observed.issue;
        // Producer callbacks use try-exclusive on this lock. The ordinary
        // coherent first-read path does not add an exclusive publication.
        if(observed.reads>1||!observed.accepted)publish_info(p);
        result.snapshot_issue=p.last_snapshot_issue;result.snapshot_reads=observed.reads;
        result.snapshot_retry_exhausted=observed.exhausted;
        if(!observed.accepted){
            result.issue=observed.exhausted?"native chassis coherent snapshot retry limit":snapshot_retry::describe(observed.issue);
            if(allow_suspension&&snapshot_retry::suspend_released_observation(observed.issue,true,released_permission(p))) {
                // SDK-only revocation. Do not publish this rejected body or
                // let a diagnostic export's wall stall discard the user bike.
                // Runtime immediately withdraws presentation/local requests;
                // poll plus a later observe must reprove the exact full key.
                release_native_motion();suspend();
            }
            return result;
        }
        result.available = true; result.active = p.active; result.detached = p.detached;
        for (std::size_t i = 0; i < 3; ++i) {
            result.position[i] = body.position[i]; result.linear_velocity[i] = body.linear[i]; result.angular_velocity[i] = body.angular[i];
            result.inertia[i] = 1.f/(body.inverse_mass*body.inertia_ratio[i]);
        }
        result.orientation = body.quaternion_xyzw; result.mass = 1.f/body.inverse_mass; result.step_seconds = fresh.seconds;
        result.world = p.world; result.native_state = p.native_state; result.body_index = p.body.index; result.body_generation = p.body.generation;
        result.token = p.token; result.serial = fresh.serial; result.simulation_time = fresh.simulation_time; result.issue = "native measured chassis";
        if (coherent_rider) *coherent_rider = std::move(fresh);
        p.latest = result; return result;
    } catch (...) {
        if (coherent_rider) *coherent_rider = {};
        result = {}; result.issue = "native chassis observation failed closed"; return result;
    }
}
bool submit(const Frame& frame, const Vec& force, const Vec& torque, const Vec& hop, std::uint64_t event) noexcept {
    LastError error; auto& p = pool();
    try {
        if (!allowed() || !p.active || !p.ready || p.failed || p.blocked || !frame.available || !frame.active ||
            !p.latest.available || frame.token != p.token || frame.serial != p.latest.serial ||
            frame.simulation_time != p.latest.simulation_time || frame.world != p.world || frame.native_state != p.native_state ||
            frame.body_index != p.body.index || frame.body_generation != p.body.generation || !step_supported(frame.step_seconds) ||
            !finite(force) || !finite(torque) || !finite(hop) || length(force) > maximum_force ||
            length(torque) > maximum_torque || length(hop) > maximum_hop || (length(hop) > 0 && !event)) return false;
        NativeFrame fresh;
        if (!fresh_owner(p,p.api.base,p.owner,fresh) || fresh.serial != frame.serial ||
            fresh.simulation_time != frame.simulation_time) { cancel_lease(); return false; }
        Lease l;
        if (!make_lease(p,fresh,l)) return false;
        std::uint64_t final_serial{}; double final_time{};
        if (!read_native_clock(fresh.entity,fresh.core,final_serial,final_time) || final_serial != frame.serial ||
            final_time != frame.simulation_time) { cancel_lease(); return false; }
        l.force = force; l.torque = torque; l.hop = hop; l.hop_event = event;
        l.token = p.token; l.serial = frame.serial; l.simulation_time = frame.simulation_time; l.step_seconds = frame.step_seconds;
        l.until = GetTickCount64()+100; l.active = true;
        auto& s = shared(); AcquireSRWLockExclusive(&s.lock);
        if (s.info.failed || frame.serial <= s.info.last_serial) { ReleaseSRWLockExclusive(&s.lock); return false; }
        s.lease = l; ReleaseSRWLockExclusive(&s.lock); return true;
    } catch (...) { cancel_lease(); return false; }
}
bool detach(std::uintptr_t base, const NativeFrame& rider) noexcept {
    LastError error; auto& p = pool(); cancel_lease();
    try {
        NativeFrame fresh;
        if (!p.ready || !p.active || p.detached || p.failed || p.blocked || p.observation_suspended ||
            !fresh_owner(p,base,rider,fresh) || !own_pool(p,fresh) ||
            !allocated_body(p.native_state,p.body.index,true)) return false;
        alignas(16) const V4 zero{};
        if (!vector(p.api.overwrite_force,&p.body,zero.data()) || !own_body(p,fresh) ||
            !vector(p.api.overwrite_torque,&p.body,zero.data()) || !own_body(p,fresh) || !pending_zero(p)) return false;
        // The first five shapes are the bike; pelvis, torso and head belong to
        // the separated rider and must no longer collide as part of the bike.
        for (std::size_t i=0;i<p.shapes.size();++i) {
            const auto expected=i<5?p.configuration.collides_with:0U;
            if (!own_shape(p,fresh,i) || !mask(p.api.collides,&p.shapes[i],expected) ||
                !own_shape(p,fresh,i)) return false;
            std::uintptr_t masks{};std::uint32_t actual{};
            if (!read(p.native_state+0xff0,masks) || !pointer(masks) ||
                !read(masks+std::uintptr_t{p.shapes[i].index}*4,actual) || actual!=expected) return false;
        }
        if (!own_pool(p,fresh) || !allocated_body(p.native_state,p.body.index,true) ||
            !pending_zero(p)) return false;
        // Keep velocity, orientation and native dynamic motion unchanged.
        p.active=p.parked=false;p.detached=true;p.latest={};
        p.issue="native detached bike; drive released";publish_info(p);return true;
    } catch (...) { return false; }
}
bool park(std::uintptr_t base, const NativeFrame& rider, bool suspend_unavailable, bool defer_unknown) noexcept {
    LastError error;
    const auto released_predecessor=pool().release_witness;
    revoke_released_witness();
    auto& p = pool(); cancel_lease();
    try {
        if (!p.ready) return !p.body_observed;
        NativeFrame fresh;
        // Pausing can legitimately stop the native clock. Quiescing requires
        // ownership/TLS/world/handle proof, not a live60Hz actuation step.
        const auto proof=prove_fresh_owner(p,base,rider,fresh,false);
        if (proof==owner_observation::Result::unavailable) {
            if(!suspend_unavailable||!owner_observation::may_suspend(
                {p.ready,p.active,p.parked,p.detached,p.failed,p.blocked,status().failed}))
                invalidate(p,"park owner unavailable; old chassis forgotten");
            else suspend(defer_unknown);
            return false;
        }
        if (proof==owner_observation::Result::changed) {
            p.release_witness=released_predecessor;
            const bool quarantined=quarantine_recreation(p,fresh);p.release_witness={};
            if(quarantined)return false; // runtime must rearm an idle retained witness; no setter ran
            invalidate(p,"park owner changed; old chassis forgotten");return false;
        }
        if (!quiesce(p,fresh)) { invalidate(p,"chassis park failed; old handles abandoned"); return false; }
        if (status().failed) { p.failed = p.blocked = true; p.issue = "parked after producer failure; same-world pool disabled"; }
        else p.issue = "native chassis parked; allocation retained";
        p.observation_suspended=p.deferred_park=false;
        publish_info(p); return true;
    } catch (...) { invalidate(p,"chassis park exception; old handles abandoned"); return false; }
}
bool parked_owner_matches(std::uintptr_t base,const NativeFrame& rider,std::uintptr_t world,std::uint64_t token, bool allow_suspension) noexcept {
    LastError error;auto& p=pool();
    try {
        if(!p.ready||!p.parked||p.active||p.failed||p.blocked||p.world!=world||p.token!=token)return false;
        NativeFrame fresh;
        const auto proof=prove_fresh_owner(p,base,rider,fresh,false);
        if(proof==owner_observation::Result::unavailable) {if(allow_suspension)suspend();return false;}
        if(proof==owner_observation::Result::changed||!own_pool(p,fresh)||!pending_zero(p))return false;
        std::uintptr_t masks{};
        if(!read(p.native_state+0xff0,masks)||!pointer(masks))return false;
        for(const auto& shape:p.shapes) {
            std::uint32_t mask{};
            if(!read(masks+std::uintptr_t{shape.index}*4,mask)||mask)return false;
        }
        return own_pool(p,fresh);
    } catch(...) {return false;}
}
bool resume(std::uintptr_t base, const NativeFrame& rider) noexcept {
    LastError error;
    revoke_released_witness();
    auto& p = pool();
    try {
        if (!p.ready || !p.parked || p.detached || p.failed || p.blocked || p.observation_suspended) return false;
        cancel_lease(); // Activation may mutate native masks before the final publication.
        NativeFrame fresh;
        const auto proof=prove_fresh_owner(p,base,rider,fresh);
        if (proof==owner_observation::Result::unavailable) { invalidate(p,"native chassis resume observation unavailable"); return false; }
        if (proof==owner_observation::Result::changed || !activate(p,fresh)) { invalidate(p,"native chassis resume ownership failed"); return false; }
        p.issue = "native chassis resumed at parked pose"; publish_info(p); return true;
    } catch (...) { invalidate(p,"native chassis resume failed closed"); return false; }
}
void suspend(bool park_when_qualified) noexcept {
    LastError error;
    // This exclusive revocation waits for any already-entered producer call
    // to finish. It does not reset failure flags, counters, tokens or history.
    cancel_lease();
    auto& p=pool();
    if (GetCurrentThreadId()!=p.client_thread || !p.ready || p.failed || p.blocked) return;
    if(!owner_observation::may_suspend(
        {p.ready,p.active,p.parked,p.detached,p.failed,p.blocked,status().failed})) {
        invalidate(p,"suspension is restricted to released parked/detached pools");return;
    }
    p.observation_suspended=true;p.deferred_park=p.deferred_park||park_when_qualified;
    p.latest={};publish_info(p);
}
SuspensionPoll poll_suspension(std::uintptr_t base,std::uintptr_t client) noexcept {
    LastError error;auto& p=pool();
    if (!p.observation_suspended) return SuspensionPoll::idle;
    if(p.owner_quarantined)return SuspensionPoll::waiting;
    cancel_lease();
    if(p.failed||p.blocked||status().failed) {
        invalidate(p,"suspended chassis has latched native failure");return SuspensionPoll::invalidated;
    }
    if (!allowed() || base!=p.api.base || GetCurrentThreadId()!=p.client_thread || !client)
        return SuspensionPoll::waiting;
    const auto canonical=read_native_frame(base,client);
    if (!canonical.available) return SuspensionPoll::waiting;
    NativeFrame fresh;
    const auto proof=prove_fresh_owner(p,base,canonical,fresh,false);
    if (proof==owner_observation::Result::unavailable) return SuspensionPoll::waiting;
    if (proof==owner_observation::Result::changed) {
        invalidate(p,"suspended chassis owner or world changed; no rebinding attempted");
        return SuspensionPoll::invalidated;
    }
    // Retained handles are reached only after a complete fresh canonical
    // same-owner and full-world proof. Existing full handle/configuration
    // failures remain terminal; this patch does not weaken those checks.
    if (!own_pool(p,fresh) || !configured_body(p.native_state,p.body.index,p.configuration)) {
        invalidate(p,"suspended chassis native pool proof failed");return SuspensionPoll::invalidated;
    }
    if (p.deferred_park) {
        if (!park(base,fresh,true,true)) return p.failed?SuspensionPoll::invalidated:SuspensionPoll::waiting;
        if (p.failed || p.blocked || status().failed) return SuspensionPoll::invalidated;
    } else {
        p.observation_suspended=false;p.latest={};publish_info(p);
    }
    return SuspensionPoll::resumed;
}

namespace {
// Fresh current-owner/world only. No retained context or rig is dereferenced.
bool custody_world(Pool& p,const NativeFrame& expected) noexcept {
    if(!allowed()||GetCurrentThreadId()!=p.client_thread||p.api.base==0||
       !released_permission(p))return false;
    NoBailSkater current;bool protected_now{};WorldKey key;
    // Cheap fresh canonical chain at each component edge. Full NativeFrame
    // reads still bracket the proof and use SourceState::busy admission.
    return inspect_no_bail_owner(expected.client,expected.entity,current,protected_now)&&
        current.base==p.api.base&&current.entity==expected.entity&&current.core==expected.core&&
        current.context==expected.context&&current.rig==expected.rig&&raw_world(p.api,expected,key)&&
        key==WorldKey{p.world,p.native_state,p.client_context};
}
bool custody_body(Pool& p,const NativeFrame& current) noexcept {
    return custody_world(p,current)&&p.body.world==p.world&&
        allocated_body(p.native_state,p.body.index)&&valid(p.api.body_valid,&p.body)&&
        configured_body(p.native_state,p.body.index,p.configuration)&&
        custody_world(p,current)&&allocated_body(p.native_state,p.body.index)&&valid(p.api.body_valid,&p.body);
}
bool released_motion_mode(Pool& p) noexcept {
    std::uintptr_t bits{},again{};std::uint32_t lane{},confirm{};
    const auto offset=std::uintptr_t{p.body.index>>5U}*4;
    const auto bit=std::uint32_t{1}<<(p.body.index&31U);
    return read(p.native_state+0xa08,bits)&&pointer(bits)&&read(bits+offset,lane)&&
        read(p.native_state+0xa08,again)&&again==bits&&read(bits+offset,confirm)&&confirm==lane&&
        bool(lane&bit)==p.detached;
}
bool custody_pool(Pool& p,const NativeFrame& current,Snapshot& values,SnapshotIssue& issue) noexcept {
    issue=SnapshotIssue::none;
    if(!custody_body(p,current)||!released_motion_mode(p)||!pending_zero(p))return false;
    std::uintptr_t owners{},categories{},masks{};
    if(!read(p.native_state+0x1008,owners)||!pointer(owners)||
       !read(p.native_state+0xfe8,categories)||!pointer(categories)||
       !read(p.native_state+0xff0,masks)||!pointer(masks))return false;
    for(std::size_t i=0;i<p.shapes.size();++i){
        const auto& shape=p.shapes[i];const auto offset=std::uintptr_t{shape.index}*4;
        const auto expected=p.detached&&i<5?p.configuration.collides_with:0U;
        std::uint32_t owner{},category{},mask_value{};
        std::uintptr_t owner_check{},category_check{},mask_check{};
        if(!custody_body(p,current)||!p.shape_observed[i]||shape.world!=p.world||shape.index==0xffffffffU||
           !valid(p.api.shape_valid,&shape)||!read(owners+offset,owner)||owner!=0xffffffffU||
           !read(categories+offset,category)||category!=0||!read(masks+offset,mask_value)||mask_value!=expected||
           !valid(p.api.shape_valid,&shape)||!custody_body(p,current)||
           !read(p.native_state+0x1008,owner_check)||owner_check!=owners||
           !read(p.native_state+0xfe8,category_check)||category_check!=categories||
           !read(p.native_state+0xff0,mask_check)||mask_check!=masks)return false;
    }
    issue=snapshot_guarded(p,values,&p.configuration,nullptr,0,[&]() noexcept{return custody_body(p,current);});
    if(issue!=SnapshotIssue::none)return false;
    return custody_body(p,current)&&released_motion_mode(p)&&pending_zero(p);
}
} // namespace
void revoke_released_witness() noexcept {
    LastError error;auto& p=pool();
    if(p.client_thread&&GetCurrentThreadId()!=p.client_thread)return;
    p.release_witness={};p.acquire_predecessor={};p.recreation_suffix.reset();
}
bool prepare_retained_acquire(std::uintptr_t base,const ReleasedKey& key) noexcept {
    LastError error;auto& p=pool();
    if(p.client_thread&&GetCurrentThreadId()!=p.client_thread)return false;
    p.acquire_predecessor={};
    if(base!=p.api.base||GetCurrentThreadId()!=p.client_thread||p.owner_quarantined||p.observation_suspended||
       p.active||!p.parked||p.detached||p.failed||p.blocked||status().failed||!p.ready||!p.attempted||
       !p.body_observed||key!=released_key(p))return false;
    p.release_witness={};p.recreation_suffix.reset();p.acquire_predecessor=key;return true;
}
ReleasedOwnerReceipt service_released_owner(std::uintptr_t base,std::uintptr_t client,const ReleaseWitness& witness,bool gameplay_ready) noexcept {
    LastError error;auto& p=pool();ReleasedOwnerReceipt result;
    try {
    if(p.client_thread&&GetCurrentThreadId()!=p.client_thread)return result;
    if(!witness.available){revoke_released_witness();
        if(p.owner_quarantined)result.result=ReleasedOwnerResult::waiting;return result;}
    if(base!=p.api.base||client!=witness.key.owner.client)return result;
    if(!gameplay_ready) {
        // A menu/loading guard revokes SDK work before any native owner/world
        // or handle read. It does not imply actor replacement or spend a token.
        if(owner_observation::witness_matches(witness,released_key(p),p.parked,p.detached)&&
           owner_observation::may_suspend({p.ready,p.active,p.parked,p.detached,p.failed,p.blocked,status().failed})){
            cancel_lease();release_native_motion();p.observation_suspended=true;p.latest={};
            p.recreation_suffix.reset();publish_info(p);result.result=ReleasedOwnerResult::waiting;
        }
        return result;
    }
    if(!allowed()||base!=p.api.base||GetCurrentThreadId()!=p.client_thread||client!=witness.key.owner.client)return result;
    p.release_witness=witness;
    if(!released_permission(p)){
        if(p.owner_quarantined){invalidate(p,"released owner custody witness lost");result.result=ReleasedOwnerResult::invalidated;}
        else revoke_released_witness();return result;
    }
    const auto fresh=read_native_frame(base,client);WorldKey key;
    const bool world_available=fresh.available&&raw_world(p.api,fresh,key);
    const auto before=released_key(p);
    const auto classification=owner_observation::classify_recreation(before.owner,before.world,
        {fresh.available,world_available,{fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig},
            {key.world,key.state,key.context}});
    if(classification==owner_observation::Recreation::unchanged&&!p.owner_quarantined)return result;
    if(classification==owner_observation::Recreation::terminal){
        invalidate(p,"released owner custody actor or world changed");result.result=ReleasedOwnerResult::invalidated;return result;
    }
    if(classification==owner_observation::Recreation::unknown){
        p.recreation_suffix.reset();if(p.owner_quarantined)result.result=ReleasedOwnerResult::waiting;return result;
    }
    if(classification==owner_observation::Recreation::candidate&&!quarantine_recreation(p,fresh)){
        result.result=ReleasedOwnerResult::waiting;return result;
    }
    cancel_lease();release_native_motion();
    result.result=ReleasedOwnerResult::waiting;
    if(status().failed){invalidate(p,"released owner custody producer failure");result.result=ReleasedOwnerResult::invalidated;return result;}
    const auto readiness=[](const NativeFrame& f) noexcept {
        return owner_observation::CustodyReadiness{f.timing_available,f.offboard,f.standable,f.serial,f.simulation_time,f.seconds};
    };
    if(!owner_observation::custody_ready(readiness(fresh))){
        p.recreation_suffix.reset();return result;
    }
    Snapshot values;SnapshotIssue custody_issue{};
    if(!custody_pool(p,fresh,values,custody_issue)){
        // A changing/unavailable current world is a revoked wait; confirmed
        // current ownership with an invalid native resource remains terminal.
        if(snapshot_retry::retryable(custody_issue)||custody_issue==SnapshotIssue::read_unavailable||
           !custody_world(p,fresh)){p.recreation_suffix.reset();return result;}
        invalidate(p,"released owner custody body or shape proof failed");result.result=ReleasedOwnerResult::invalidated;return result;
    }
    const auto confirmed=read_native_frame(base,client);WorldKey confirmed_world;
    if(!same_owner(fresh,confirmed)||!raw_world(p.api,confirmed,confirmed_world)||
       confirmed_world!=key||!owner_observation::custody_readiness_matches(readiness(fresh),readiness(confirmed))){
        p.recreation_suffix.reset();return result;
    }
    if(classification==owner_observation::Recreation::unchanged){
        p.owner_quarantined=p.observation_suspended=false;p.deferred_park=false;p.latest={};
        p.recreation_suffix.reset();p.issue="released bike original owner requalified";publish_info(p);
        result.result=ReleasedOwnerResult::resumed;return result;
    }
    if(!p.recreation_suffix.observe({fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig},fresh.serial,fresh.simulation_time))return result;
    if(!owner_observation::custody_token_headroom(p.token)||p.failed||p.blocked||status().failed){
        invalidate(p,"released owner custody token or failure gate");result.result=ReleasedOwnerResult::invalidated;return result;
    }
    // Complete every potentially throwing string copy before association/token mutation.
    auto prepared=owner_observation::prepare_association(confirmed);
    const auto final_owner=read_native_frame(base,client);WorldKey final_world;
    if(!same_owner(confirmed,final_owner)||!raw_world(p.api,final_owner,final_world)||final_world!=key||
       !owner_observation::custody_readiness_matches(readiness(fresh),readiness(final_owner))){p.recreation_suffix.reset();return result;}
    cancel_lease();release_native_motion();
    if(status().failed){invalidate(p,"released owner custody producer failed before commit");result.result=ReleasedOwnerResult::invalidated;return result;}
    result.before=before;result.parked=p.parked;result.detached=p.detached;
    // SDK-only association commit. All native setters keep strict owner checks;
    // neither this receipt nor pointer equality is a native lifetime fence.
    static_assert(std::is_nothrow_move_assignable_v<NativeFrame>);
    p.owner=std::move(prepared.owner);result.rider=std::move(prepared.receipt);++p.token;p.latest={};p.deferred_park=false;
    p.owner_quarantined=p.observation_suspended=false;p.recreation_suffix.reset();
    result.after=released_key(p);
    auto& measured=result.measured_body;measured.available=true;measured.detached=p.detached;
    for(unsigned axis=0;axis<3;++axis){
        measured.position[axis]=values.position[axis];measured.linear_velocity[axis]=values.linear[axis];
        measured.angular_velocity[axis]=values.angular[axis];
    }
    measured.orientation=values.quaternion_xyzw;measured.world=p.world;measured.native_state=p.native_state;
    measured.body_index=p.body.index;measured.body_generation=p.body.generation;measured.token=p.token;
    measured.serial=confirmed.serial;measured.simulation_time=confirmed.simulation_time;measured.step_seconds=confirmed.seconds;
    p.release_witness={};p.issue="released bike owner custody requalified; explicit retry required";publish_info(p);
    result.result=ReleasedOwnerResult::committed;return result;

    }catch(...) {
        cancel_lease();release_native_motion();
        // Allocation/copy failure cannot abandon or partially adopt the pool.
        p.latest={};p.recreation_suffix.reset();if(released_permission(p)){p.owner_quarantined=p.observation_suspended=true;p.deferred_park=false;publish_info(p);}
        result.rider={};result.result=ReleasedOwnerResult::waiting;return result;
    }
}

void forget() noexcept { invalidate(pool(),"native chassis forgotten; no old-world cleanup attempted"); }
Status status() noexcept {
    auto& s = shared(); AcquireSRWLockShared(&s.lock); const auto result = s.info; ReleaseSRWLockShared(&s.lock); return result;
}
ReleasedOwnershipStatus released_ownership_status() noexcept {
    auto& s = shared();
    if (!TryAcquireSRWLockShared(&s.lock)) return {};
    auto result = s.released;
    result.lease_active = s.lease.active;
    result.failed = result.failed || s.info.failed;
    if (result.failed) result.available = false;
    ReleaseSRWLockShared(&s.lock);
    return result;
}
} // namespace dingosdk::bmx::native_chassis


#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "bmx_native_coupled.inc"
#endif
