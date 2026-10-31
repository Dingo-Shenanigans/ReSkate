#include "owned_bodies.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_bodies.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Build/20260929/rigid_bodies.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Engine/Game/World/body_hit_identity.h"
#include "Engine/Game/World/body_contact_read.h"
#include "Engine/Game/World/body_shape_categories.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace dingosdk::owned_bodies {
namespace {
using namespace owned_body;
namespace r = addr::rigid_bodies;
namespace pb = addr::physics_bodies;
namespace c = addr::remote_collision;
namespace e = addr::engine;
namespace k = addr::skater_state;
using V4 = std::array<float, 4>;
using NativeHandle = body_hit_identity::NativeHandle;
static_assert(sizeof(NativeHandle) == 16);
using World = std::uintptr_t (*)(std::uintptr_t);
using Allocate = NativeHandle* (*)(std::uintptr_t, NativeHandle*);
using CapsuleCall = NativeHandle* (*)(std::uintptr_t, NativeHandle*, const NativeHandle*, const float*, const float*, float);
using SetInt = void (*)(NativeHandle*, int);
using SetFloat = void (*)(NativeHandle*, float);
using Touch = void (*)(NativeHandle*);
using SetMask = void (*)(NativeHandle*, const std::uint32_t*);
using SetOwner = void (*)(NativeHandle*, const NativeHandle*);
using SetVector = void (*)(NativeHandle*, const float*);
using Valid = bool (*)(const NativeHandle*);
using Synchronize = std::uintptr_t (*)(NativeHandle*, std::uintptr_t);
struct Api {
    std::uintptr_t base{};
    World world{};
    Allocate allocate{};
    CapsuleCall capsule{};
    SetInt motion{};
    SetFloat inverse_mass{};
    Touch clear{}, wake{};
    SetMask categories{}, collides{};
    SetOwner owner{};
    SetVector com{}, transform{}, linear{}, angular{}, force{}, torque{}, inertia{}, clear_force{}, clear_torque{};
    Valid body_valid{}, shape_valid{};
};
struct Watch { std::uintptr_t address{}; std::size_t size{}; std::array<unsigned char, 16> bytes{}; };
// capture() lists the board's bodies, then the skeleton's from this one, as noclip does.
constexpr std::size_t first_rig_wrapper = 1, wrapper_count = 32;
static_assert(wrapper_count > k::board_body_count && wrapper_count - k::board_body_count + first_rig_wrapper <= k::rig_body_count);
struct Anchor {
    std::uintptr_t client{}, context{}, entity{}, core{}, skater_context{}, rig{}, world{}, state{};
    std::array<Watch, 128> watches{};
    std::size_t count{};
    std::array<std::uintptr_t, wrapper_count> wrappers{};
    bool same(const Anchor& b) const noexcept {
        return client == b.client && context == b.context && entity == b.entity && core == b.core &&
            skater_context == b.skater_context && rig == b.rig && world == b.world && state == b.state;
    }
};
struct Clock {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<std::uint64_t> sequence{};
    NoBailSkater owner{};
    std::uintptr_t game_context{}, world{}, state{};
    std::uint64_t serial{}, observed{};
    double time{};
    float seconds{};
};
struct Slot {
    std::uint64_t owner{};
    Handle handle{};
    Definition definition{};
    Anchor anchor{};
    NativeHandle body{};
    std::array<NativeHandle, maximum_capsules> shapes{};
    std::size_t made{};
    bool attempted{}, body_observed{}, active{}, retained{}, quarantined{}, creation_failed{};
    AllocationProgress allocation{};
    Sample latest{};
    Mailbox mailbox{};
};
struct Service {
    SRWLOCK lock = SRWLOCK_INIT;
    Api api{};
    std::array<Slot, retained_capacity> slots{};
    std::atomic<Synchronize> original{};
    std::atomic<bool> hooked{};
    bool hook_attempted{}, initialization_requested{};
    std::uintptr_t base{}, client{}, world{}, state{}, context{};
    DWORD thread{};
    bool eligible{};
    std::uint64_t generation{}, world_generation{}, tick_at{}, command_sequence{};
};
// Static fixed storage exists before any hook can run. Physics callbacks never construct
// a function-local singleton, allocate, format text, call Lua or wait for a lock.
Service service;
Clock clock_feed;
// The physics-step listener reads its world through this reader, published by initialize()
// once the reader's bytes have been compared.
std::atomic<World> clock_world{};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct LastError { DWORD value = GetLastError(); ~LastError() { SetLastError(value); } };
struct Locked { SRWLOCK& lock; explicit Locked(SRWLOCK& value) : lock(value) { AcquireSRWLockExclusive(&lock); } ~Locked() { ReleaseSRWLockExclusive(&lock); } };
template<class T> bool peek(std::uintptr_t at, T& value) noexcept { return memory::peek_bytes(at, &value, sizeof(value)); }
bool pointer(std::uintptr_t p) noexcept { return p >= 0x10000 && p < memory::highest_user_address - 0x20000; }
bool watched(const Anchor& a) noexcept {
    if (!a.count || a.count > a.watches.size()) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        const auto& w = a.watches[i];
        std::array<unsigned char, 16> bytes{};
        if (!w.size || w.size > bytes.size() || !memory::peek_bytes(w.address, bytes.data(), w.size) || bytes != w.bytes) return false;
    }
    return true;
}
struct ReadFailure {};
struct Reader {
    Anchor& anchor;
    template<class T> T get(std::uintptr_t at) {
        static_assert(sizeof(T) <= 16);
        T value{};
        if (!pointer(at) || !peek(at, value) || anchor.count == anchor.watches.size()) throw ReadFailure{};
        auto& watch = anchor.watches[anchor.count++];
        watch.address = at;
        watch.size = sizeof(T);
        std::memcpy(watch.bytes.data(), &value, sizeof(T));
        return value;
    }
    std::uintptr_t ptr(std::uintptr_t at) { return get<std::uintptr_t>(at); }
};
void require(bool value) { if (!value) throw ReadFailure{}; }
bool native_world(World call, std::uintptr_t context, std::uintptr_t& out) noexcept {
    __try { out = call(context); return pointer(out); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool valid_handle(Valid call, const NativeHandle& h) noexcept {
    __try { return pointer(h.world) && h.index != 0xffffffffU && call(&h); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool allocate(const Api& a, std::uintptr_t world, NativeHandle& out) noexcept {
    __try { return a.allocate(world, &out) == &out && out.world == world && valid_handle(a.body_valid, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool set_int(SetInt call, NativeHandle& h, int value) noexcept {
    __try { call(&h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool scalar(SetFloat call, NativeHandle& h, float value) noexcept {
    __try { call(&h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool vector(SetVector call, NativeHandle& h, const float* value) noexcept {
    __try { call(&h, value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool touch(Touch call, NativeHandle& h) noexcept {
    __try { call(&h); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool mask(SetMask call, NativeHandle& h, std::uint32_t value) noexcept {
    __try { call(&h, &value); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool clear_owner(SetOwner call, NativeHandle& h) noexcept {
    __try { const NativeHandle none{h.world, 0xffffffffU, 0}; call(&h, &none); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool capsule(const Api& a, Slot& slot, std::size_t index) noexcept {
    const auto& definition = slot.definition.capsules[index];
    alignas(16) const V4 bottom{definition.a[0], definition.a[1], definition.a[2], 0};
    alignas(16) const V4 top{definition.b[0], definition.b[1], definition.b[2], 0};
    __try {
        auto& shape = slot.shapes[index];
        return a.capsule(slot.anchor.world, &shape, &slot.body, bottom.data(), top.data(), definition.radius) == &shape &&
            shape.world == slot.anchor.world && valid_handle(a.shape_valid, shape);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& f) noexcept {
    std::array<unsigned char, 32> actual{};
    return memory::peek_bytes(base + f.rva, actual.data(), actual.size()) && actual == f.bytes;
}
bool resolve(std::uintptr_t base, Api& a) noexcept {
    if (!supported_build::running_image_matches(base)) return false;
    for (const auto* f : {&c::physics_world, &c::allocate_body, &c::create_capsule, &c::set_motion_type,
        &c::set_body_980, &c::clear_body_aa0, &c::set_categories, &c::set_collides_with, &c::set_shape_owner,
        &c::set_center_of_mass, &c::set_transform, &c::set_linear_velocity, &c::set_angular_velocity,
        &c::wake_body, &c::body_valid, &c::shape_valid, &r::linear_force, &r::angular_torque,
        &r::set_inertia_ratio, &r::overwrite_force, &r::overwrite_torque, &r::synchronizer}) if (!matches(base, *f)) return false;
    a.base = base;
    a.world = reinterpret_cast<World>(base + c::physics_world.rva);
    a.allocate = reinterpret_cast<Allocate>(base + c::allocate_body.rva);
    a.capsule = reinterpret_cast<CapsuleCall>(base + c::create_capsule.rva);
    a.motion = reinterpret_cast<SetInt>(base + c::set_motion_type.rva);
    a.inverse_mass = reinterpret_cast<SetFloat>(base + c::set_body_980.rva);
    a.clear = reinterpret_cast<Touch>(base + c::clear_body_aa0.rva);
    a.wake = reinterpret_cast<Touch>(base + c::wake_body.rva);
    a.categories = reinterpret_cast<SetMask>(base + c::set_categories.rva);
    a.collides = reinterpret_cast<SetMask>(base + c::set_collides_with.rva);
    a.owner = reinterpret_cast<SetOwner>(base + c::set_shape_owner.rva);
    a.com = reinterpret_cast<SetVector>(base + c::set_center_of_mass.rva);
    a.transform = reinterpret_cast<SetVector>(base + c::set_transform.rva);
    a.linear = reinterpret_cast<SetVector>(base + c::set_linear_velocity.rva);
    a.angular = reinterpret_cast<SetVector>(base + c::set_angular_velocity.rva);
    a.force = reinterpret_cast<SetVector>(base + r::linear_force.rva);
    a.torque = reinterpret_cast<SetVector>(base + r::angular_torque.rva);
    a.inertia = reinterpret_cast<SetVector>(base + r::set_inertia_ratio.rva);
    a.clear_force = reinterpret_cast<SetVector>(base + r::overwrite_force.rva);
    a.clear_torque = reinterpret_cast<SetVector>(base + r::overwrite_torque.rva);
    a.body_valid = reinterpret_cast<Valid>(base + c::body_valid.rva);
    a.shape_valid = reinterpret_cast<Valid>(base + c::shape_valid.rva);
    return true;
}
bool client_context(const Anchor& a) noexcept {
    std::uint32_t index{}, phase{};
    std::uintptr_t tls{}, current{}, context{};
    std::uint8_t active{};
    return peek(a.client + r::client_context, context) && context == a.context &&
        peek(a.client + r::client_phase, phase) && (phase == 13 || phase == 21) &&
        peek(service.base + e::tls_index, index) && index <= 4095 &&
        peek(static_cast<std::uintptr_t>(__readgsqword(0x58)) + std::uintptr_t{index} * sizeof(std::uintptr_t), tls) && pointer(tls) &&
        peek(tls + r::tls_active, active) && active && peek(tls + r::tls_context, current) && current == a.context;
}
bool capture(Anchor& a) noexcept {
    try {
        a = {};
        a.client = service.client;
        Reader reader{a};
        const auto ptr = [&](std::uintptr_t at) { return reader.ptr(at); };
        const auto u32 = [&](std::uintptr_t at) { return reader.get<std::uint32_t>(at); };
        const auto u8 = [&](std::uintptr_t at) { return reader.get<std::uint8_t>(at); };
        require(ptr(a.client) == service.base + e::client_vtable);
        a.context = ptr(a.client + r::client_context);
        const auto phase = u32(a.client + r::client_phase);
        require(phase == 13 || phase == 21);
        const auto offset = u32(service.base + e::context_player_manager_offset);
        require(offset <= 0x1000000);
        const auto manager = ptr(a.context + offset);
        require(ptr(manager) == service.base + e::local_player_manager_vtable);
        const auto begin = ptr(manager + r::player_begin), end = ptr(manager + r::player_end);
        require(pointer(begin) && end == begin + sizeof(std::uintptr_t));
        const auto player = ptr(begin);
        require(ptr(player) == service.base + e::local_player_vtable && ptr(player + r::player_context) == a.context &&
            u8(player + r::player_local) == 1 && u8(player + r::player_remote) == 0);
        a.entity = ptr(player + r::player_entity);
        const auto handle = ptr(player + r::player_handle);
        require(ptr(a.entity) == service.base + e::skater_entity_vtable && ptr(a.entity + r::entity_context) == a.context &&
            ptr(a.entity + r::entity_player) == player && ptr(handle) == a.entity + r::entity_handle_target && u8(a.entity + r::entity_teleport) == 0);
        const auto collection = ptr(a.entity + r::entity_collection), component = ptr(a.entity + r::entity_component);
        require(ptr(collection) == a.entity && ptr(component) == service.base + e::skater_component_vtable && ptr(component + r::component_collection) == collection);
        a.core = ptr(component + r::component_core);
        a.skater_context = ptr(a.core + r::core_context);
        a.rig = ptr(a.core + r::core_rig);
        require(ptr(a.core) == service.base + addr::no_bail::bail_core_vtable && ptr(a.rig) == a.skater_context && ptr(a.rig + r::rig_core) == a.core);
        const auto board = ptr(ptr(a.core + k::board_holder_offset) + k::board_physics_offset), rig = ptr(a.rig + k::rig_physics_offset);
        require(ptr(board) == service.base + k::board_physics_vtable && ptr(rig) == service.base + k::rig_physics_vtable);
        const auto board_parts = ptr(board + k::physics_bodies_offset), rig_parts = ptr(rig + k::physics_bodies_offset);
        require(u32(board_parts) == k::board_body_count && u32(rig_parts) == k::rig_body_count);
        // The wrappers a pending command may be produced on: the same 32 bodies noclip drives
        // (client_source_spawn_internal.h's parts), the board's 9 and then the skeleton's velocity
        // parts 1 to 23, and the list the in-game runs used. The skeleton's body 0 and bodies 24
        // and 25 are not in it. A command is produced once, at the first listed wrapper the
        // synchronizer meets in the step after the command's epoch, so the list needs wrappers
        // that are synchronized every step, not every body.
        for (std::size_t i = 0; i < a.wrappers.size(); ++i) {
            const bool on_board = i < k::board_body_count;
            const auto part = on_board ? board_parts + i * k::physics_body_size :
                rig_parts + (i - k::board_body_count + first_rig_wrapper) * k::physics_body_size;
            require(ptr(part + k::body_owner_offset) == (on_board ? board : rig));
            a.wrappers[i] = part + k::body_owner_offset;
        }
        require(client_context(a) && native_world(service.api.world, a.context, a.world));
        require(peek(a.world + pb::store_offset, a.state) && pointer(a.state));
        return watched(a) && client_context(a);
    } catch (...) { return false; }
}
void physics_step(const NoBailSkater& owner, float seconds, bool) noexcept {
    const auto sequence = clock_feed.sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::uintptr_t context{}, world{}, state{};
    const auto world_call = clock_world.load(std::memory_order_acquire);
    if (!world_call || !calibrated(seconds) || !peek(owner.entity + r::entity_context, context) || !pointer(context) ||
        !native_world(world_call, context, world) || !peek(world + pb::store_offset, state) || !pointer(state) ||
        !TryAcquireSRWLockExclusive(&clock_feed.lock)) return;
    auto& f = clock_feed;
    if (f.owner.entity != owner.entity || f.owner.core != owner.core || f.owner.context != owner.context || f.owner.rig != owner.rig ||
        f.game_context != context || f.world != world || f.state != state) f.time = 0;
    f.owner = owner;
    f.serial = sequence;
    f.time += seconds;
    f.seconds = seconds;
    f.observed = GetTickCount64();
    f.game_context = context;
    f.world = world;
    f.state = state;
    ReleaseSRWLockExclusive(&f.lock);
}
bool clock(const Anchor& a, Epoch& epoch) noexcept {
    if (!TryAcquireSRWLockShared(&clock_feed.lock)) return false;
    const auto& f = clock_feed;
    const bool ok = f.owner.base == service.base && f.owner.entity == a.entity && f.owner.core == a.core &&
        f.owner.context == a.skater_context && f.owner.rig == a.rig && f.game_context == a.context && f.world == a.world && f.state == a.state &&
        f.serial && f.serial == f.sequence.load(std::memory_order_acquire) && GetTickCount64() - f.observed < 250 &&
        calibrated(f.seconds) && std::isfinite(f.time);
    if (ok) { epoch.serial = f.serial; epoch.time = f.time; epoch.seconds = f.seconds; }
    ReleaseSRWLockShared(&clock_feed.lock);
    float h{};
    return ok && peek(a.skater_context + r::context_seconds, h) && calibrated(h);
}
bool allocated(std::uintptr_t state, std::uint32_t index, bool dynamic = false) noexcept {
    std::uint32_t capacity{}, again{}, bits{}, check_bits{}, dynamic_bits{}, dynamic_check{};
    std::uintptr_t source{}, check{}, dynamics{}, dynamics_again{};
    const auto word = std::uintptr_t{index >> 5} * sizeof(std::uint32_t);
    const auto bit = 1U << (index & 31U);
    if (!peek(state + r::body_capacity, capacity) || !capacity || capacity > 1048576 || index >= capacity ||
        !peek(state + r::allocated_bits, source) || !pointer(source) || !peek(source + word, bits) || !(bits & bit)) return false;
    if (dynamic && (!peek(state + r::dynamic_bits, dynamics) || !pointer(dynamics) || !peek(dynamics + word, dynamic_bits) || !(dynamic_bits & bit))) return false;
    return peek(state + r::body_capacity, again) && again == capacity && peek(state + r::allocated_bits, check) && check == source &&
        peek(source + word, check_bits) && check_bits == bits && (!dynamic || (peek(state + r::dynamic_bits, dynamics_again) &&
        dynamics_again == dynamics && peek(dynamics + word, dynamic_check) && dynamic_check == dynamic_bits));
}
bool anchor_current(const Anchor& a) noexcept {
    std::uintptr_t world{}, state{};
    return watched(a) && client_context(a) && native_world(service.api.world, a.context, world) && world == a.world &&
        peek(world + pb::store_offset, state) && state == a.state && watched(a);
}
bool body_current(const Slot& s, bool dynamic = false) noexcept {
    return s.body_observed && anchor_current(s.anchor) && s.body.world == s.anchor.world && allocated(s.anchor.state, s.body.index, dynamic) &&
        valid_handle(service.api.body_valid, s.body) && anchor_current(s.anchor) && allocated(s.anchor.state, s.body.index, dynamic);
}
bool array4(std::uintptr_t state, std::uintptr_t offset, std::uint32_t index, V4& value, std::uintptr_t& source) noexcept {
    std::uintptr_t check{};
    if (!peek(state + offset, source) || !pointer(source) || !peek(source + std::uintptr_t{index} * sizeof(V4), value) ||
        !peek(state + offset, check) || check != source) return false;
    for (float v : value) if (!std::isfinite(v) || std::abs(v) > 1000000) return false;
    return true;
}
bool configured(const Slot& s) noexcept {
    const auto& d = s.definition;
    V4 ratio{}, again{};
    std::uintptr_t source{}, source_again{}, masses{}, masses_again{};
    float inverse{}, inverse_again{};
    const V4 expected{d.mass / d.inertia[0], d.mass / d.inertia[1], d.mass / d.inertia[2], 0};
    return array4(s.anchor.state, r::inertia_ratio_array, s.body.index, ratio, source) && ratio == expected &&
        peek(s.anchor.state + r::inverse_mass_array, masses) && pointer(masses) && peek(masses + std::uintptr_t{s.body.index} * 4, inverse) && inverse == 1.f / d.mass &&
        array4(s.anchor.state, r::inertia_ratio_array, s.body.index, again, source_again) && source == source_again && ratio == again &&
        peek(s.anchor.state + r::inverse_mass_array, masses_again) && masses_again == masses &&
        peek(masses + std::uintptr_t{s.body.index} * 4, inverse_again) && inverse_again == inverse;
}
bool shape_current(const Slot& s, std::size_t i, bool masks = true) noexcept {
    if (i >= s.made || !body_current(s) || s.shapes[i].world != s.anchor.world || !valid_handle(service.api.shape_valid, s.shapes[i])) return false;
    if (!masks) return anchor_current(s.anchor);
    std::uintptr_t owners{}, categories{}, with{};
    std::uint32_t owner{}, category{}, collides{};
    const auto offset = std::uintptr_t{s.shapes[i].index} * 4;
    return peek(s.anchor.state + pb::body_owner_offset, owners) && pointer(owners) && peek(owners + offset, owner) &&
        owner == r::unassigned_shape_owner &&
        peek(s.anchor.state + r::shape_categories, categories) && pointer(categories) && peek(categories + offset, category) && category == r::safe_categories &&
        peek(s.anchor.state + r::shape_collides, with) && pointer(with) && peek(with + offset, collides) &&
        (collides == 0 || collides == r::safe_world_collisions) && body_current(s);
}
bool pool_current(const Slot& s, bool dynamic = false) noexcept {
    if (!body_current(s, dynamic) || s.made != s.definition.count || !configured(s)) return false;
    for (std::size_t i = 0; i < s.made; ++i) if (!shape_current(s, i)) return false;
    return body_current(s, dynamic);
}
bool producer_owner(const Slot& s, const NativeHandle& incoming) noexcept {
    if (!pool_current(s, true) || incoming.world != s.anchor.world || !allocated(s.anchor.state, incoming.index) || !valid_handle(service.api.body_valid, incoming)) return false;
    for (std::size_t i = 0; i < s.made; ++i) {
        std::uintptr_t masks{};
        std::uint32_t collides{};
        if (!peek(s.anchor.state + r::shape_collides, masks) || !pointer(masks) ||
            !peek(masks + std::uintptr_t{s.shapes[i].index} * 4, collides) || collides != r::safe_world_collisions) return false;
    }
    return pool_current(s, true) && allocated(s.anchor.state, incoming.index) && valid_handle(service.api.body_valid, incoming);
}
bool pending_zero(const Slot& s) noexcept {
    V4 force{}, torque{};
    std::uintptr_t a{}, b{};
    return array4(s.anchor.state, r::pending_force, s.body.index, force, a) && force == V4{} &&
        array4(s.anchor.state, r::pending_torque, s.body.index, torque, b) && torque == V4{};
}
bool park(Slot& s) noexcept {
    s.mailbox.cancel();
    if (!body_current(s)) return false;
    for (std::size_t i = 0; i < s.made; ++i)
        if (!shape_current(s, i) || !mask(service.api.categories, s.shapes[i], 0) || !shape_current(s, i) ||
            !mask(service.api.collides, s.shapes[i], 0) || !shape_current(s, i)) return false;
    for (std::size_t i = 0; i < s.made; ++i) {
        std::uintptr_t categories{}, with{};
        std::uint32_t category{}, collides{};
        const auto offset = std::uintptr_t{s.shapes[i].index} * 4;
        if (!body_current(s) || !peek(s.anchor.state + r::shape_categories, categories) || !pointer(categories) ||
            !peek(categories + offset, category) || category != 0 || !peek(s.anchor.state + r::shape_collides, with) || !pointer(with) ||
            !peek(with + offset, collides) || collides != 0 || !shape_current(s, i)) return false;
    }
    alignas(16) const V4 zero{};
    if (allocated(s.anchor.state, s.body.index, true) && (!vector(service.api.clear_force, s.body, zero.data()) || !body_current(s) ||
        !vector(service.api.clear_torque, s.body, zero.data()) || !body_current(s))) return false;
    return pending_zero(s) && vector(service.api.linear, s.body, zero.data()) && body_current(s) &&
        vector(service.api.angular, s.body, zero.data()) && body_current(s) && set_int(service.api.motion, s.body, 2) && body_current(s) && pending_zero(s);
}
void retire(Slot& s, bool may_park) noexcept {
    s.mailbox.cancel();
    const bool parked = may_park && park(s);
    s.active = false;
    s.retained = true;
    s.quarantined = s.allocation.quarantine(parked);
    s.latest = {};
    // No qualified native destructor exists. This slot is never recycled and its old
    // handles are never adopted by another owner, even after a level reload.
}
bool command_clock(const Slot& s, Epoch& out) noexcept {
    out.world = s.latest.epoch.world;
    out.body = s.handle.generation;
    return clock(s.anchor, out);
}
void produce(NativeHandle* incoming_pointer, std::uintptr_t wrapper) noexcept {
    if (!service.hooked.load(std::memory_order_acquire) || !TryAcquireSRWLockExclusive(&service.lock)) return;
    for (auto& s : service.slots) {
        if (!s.active || !s.mailbox.pending) continue;
        if (GetTickCount64() >= s.mailbox.deadline) { Command ignored; (void)s.mailbox.consume(s.mailbox.command.epoch, GetTickCount64(), ignored); continue; }
        if (std::find(s.anchor.wrappers.begin(), s.anchor.wrappers.end(), wrapper) == s.anchor.wrappers.end()) continue;
        NativeHandle incoming{};
        Epoch current{};
        if (!peek(reinterpret_cast<std::uintptr_t>(incoming_pointer), incoming) || !producer_owner(s, incoming) || !command_clock(s, current)) {
            s.mailbox.cancel();
            s.quarantined = true;
            s.active = false;
            continue;
        }
        Command command{};
        if (!s.mailbox.consume(current, GetTickCount64(), command)) continue;
        Vec force = command.force;
        for (std::size_t i = 0; i < 3; ++i) force[i] += command.impulse[i] / command.epoch.seconds;
        alignas(16) const V4 f{force[0], force[1], force[2], 0}, t{command.torque[0], command.torque[1], command.torque[2], 0};
        const auto qualified = [&]() noexcept {
            Epoch epoch{};
            return GetTickCount64() < s.mailbox.deadline && command_clock(s, epoch) && epoch == current && producer_owner(s, incoming);
        };
        bool attempted_force{}, attempted_torque{}, ok{};
        if (qualified()) {
            attempted_force = true;
            if (vector(service.api.force, s.body, f.data()) && qualified()) {
                attempted_torque = true;
                ok = vector(service.api.torque, s.body, t.data()) && qualified();
            }
        }
        s.mailbox.finish(attempted_force, attempted_torque, ok);
        if (!ok) { s.quarantined = true; s.active = false; }
    }
    ReleaseSRWLockExclusive(&service.lock);
}
// The original synchronizer runs first; a pending command for this wrapper is produced after it.
std::uintptr_t synchronize_hook(NativeHandle* incoming, std::uintptr_t wrapper) {
    const auto original = service.original.load(std::memory_order_acquire);
    const auto result = original(incoming, wrapper);
    LastError error;
    produce(incoming, wrapper);
    return result;
}
bool initialize() noexcept {
    if (service.hook_attempted) return service.hooked.load(std::memory_order_acquire);
    service.hook_attempted = true;
    if (!resolve(service.base, service.api)) return false;
    clock_world.store(service.api.world, std::memory_order_release);
    const auto init = hook_initialize();
    if (init != HookOk && init != HookAlreadyInitialized) return false;
    void* original{};
    auto* target = reinterpret_cast<void*>(service.base + r::synchronizer.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&synchronize_hook), &original) != HookOk || !original) return false;
    service.original.store(reinterpret_cast<Synchronize>(original), std::memory_order_release);
    {
        HookBatch batch;
        if (hook_enable(target) != HookOk || !start_no_bail(service.base)) return false;
    }
    // After the batch, the existing attached target answers without another transaction.
    if (hook_enable(target) != HookOk || !add_no_bail_step_listener(&physics_step)) return false;
    service.hooked.store(true, std::memory_order_release);
    return true;
}
bool eligible() noexcept {
    if (!service.eligible || GetCurrentThreadId() != service.thread || GetTickCount64() - service.tick_at >= 250) return false;
    try { const auto mp = multiplayer::model(); return !mp.active && !mp.lobby_joining; } catch (...) { return false; }
}
Slot* lookup(std::uint64_t owner, const Handle& handle) noexcept {
    if (!owner || !handle.generation || handle.slot >= service.slots.size()) return nullptr;
    auto& s = service.slots[handle.slot];
    return s.attempted && s.owner == owner && s.handle == handle ? &s : nullptr;
}
bool fresh(const Slot& s) noexcept {
    Anchor a;
    return eligible() && capture(a) && s.anchor.same(a) && pool_current(s, s.active);
}
// The native ring header, its bounds and its plane stride are body_contact_read's, which the
// contact measurement reads too; the service adds the user-address check its pointers get.
using Ring = body_contact_read::Ring;
bool ring(const Slot& s, std::uintptr_t offset, Ring& header, V4& value) noexcept {
    const auto read = [](std::uintptr_t at, auto& out) { return peek(at, out); };
    return body_contact_read::ring_value(read, s.anchor.state, offset, s.body.index, header, value) && pointer(header.values);
}
// Reads the body twice between two equal clock epochs and publishes nothing; sample()
// publishes latest and may attach a mailbox successor.
bool observe(const Slot& s, Sample& out) noexcept {
    Epoch before{};
    before.world = service.world_generation;
    before.body = s.handle.generation;
    if (!fresh(s) || !clock(s.anchor, before)) return false;
    const std::array<std::uintptr_t, 4> offsets{r::position_ring, r::rotation_ring, r::velocity_ring, r::spin_ring};
    std::array<Ring, 4> headers{};
    std::array<V4, 4> values{};
    for (std::size_t i = 0; i < offsets.size(); ++i) if (!ring(s, offsets[i], headers[i], values[i])) return false;
    double norm{};
    for (float v : values[1]) norm += static_cast<double>(v) * v;
    if (norm < .9 || norm > 1.1) return false;
    for (std::size_t i = 2; i < 4; ++i) for (float v : values[i]) if (std::abs(v) > 100) return false;
    V4 com{}, local{}, ratio{};
    std::uintptr_t com_source{}, local_source{}, ratio_source{}, masses{};
    float inverse{};
    if (!array4(s.anchor.state, r::local_com, s.body.index, com, com_source) || com != V4{} ||
        !array4(s.anchor.state, r::local_rotation, s.body.index, local, local_source) || local != V4{1, 0, 0, 0} ||
        !array4(s.anchor.state, r::inertia_ratio_array, s.body.index, ratio, ratio_source) ||
        !peek(s.anchor.state + r::inverse_mass_array, masses) || !pointer(masses) ||
        !peek(masses + std::uintptr_t{s.body.index} * 4, inverse) || !std::isfinite(inverse) || inverse <= 0) return false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            Ring again;
            V4 value{};
            if (!ring(s, offsets[i], again, value) || headers[i] != again || values[i] != value) return false;
        }
        V4 value{};
        std::uintptr_t source{};
        if (!array4(s.anchor.state, r::local_com, s.body.index, value, source) || source != com_source || value != com ||
            !array4(s.anchor.state, r::local_rotation, s.body.index, value, source) || source != local_source || value != local || !configured(s)) return false;
        std::uintptr_t inverse_source{};
        float inverse_again{};
        if (!array4(s.anchor.state, r::inertia_ratio_array, s.body.index, value, source) || source != ratio_source || value != ratio ||
            !peek(s.anchor.state + r::inverse_mass_array, inverse_source) || inverse_source != masses ||
            !peek(inverse_source + std::uintptr_t{s.body.index} * 4, inverse_again) || inverse_again != inverse) return false;
    }
    Epoch after{};
    after.world = service.world_generation;
    after.body = s.handle.generation;
    if (!fresh(s) || !clock(s.anchor, after) || after != before) return false;
    out = {};
    out.epoch = after;
    for (std::size_t i = 0; i < 3; ++i) { out.position[i] = values[0][i]; out.velocity[i] = values[2][i]; out.angular_velocity[i] = values[3][i]; }
    out.rotation = {values[1][1], values[1][2], values[1][3], values[1][0]};
    out.mass = 1.f / inverse;
    for (std::size_t i = 0; i < 3; ++i) out.inertia[i] = 1.f / (inverse * ratio[i]);
    return true;
}
bool sample(Slot& s, Sample& out) noexcept {
    if (!observe(s, out)) return false;
    s.latest = out;
    (void)s.mailbox.observe_successor(out);
    return true;
}
bool create_native(Slot& s) noexcept {
    if (!anchor_current(s.anchor) || !allocate(service.api, s.anchor.world, s.body)) return false;
    s.body_observed = true;
    s.allocation.body_confirmed = true;
    // Do not configure an unexpected allocation or infer that a zero handle is a body.
    // These are the defaults a freshly allocated native body has.
    V4 initial_ratio{}, initial_com{}, initial_local{};
    std::uintptr_t source{}, masses{};
    float inverse{};
    if (!body_current(s) || !array4(s.anchor.state, r::inertia_ratio_array, s.body.index, initial_ratio, source) || initial_ratio != V4{1, 1, 1, 0} ||
        !array4(s.anchor.state, r::local_com, s.body.index, initial_com, source) || initial_com != V4{} ||
        !array4(s.anchor.state, r::local_rotation, s.body.index, initial_local, source) || initial_local != V4{1, 0, 0, 0} ||
        !peek(s.anchor.state + r::inverse_mass_array, masses) || !pointer(masses) ||
        !peek(masses + std::uintptr_t{s.body.index} * 4, inverse) || inverse != 1 || !pending_zero(s) || !body_current(s)) return false;
    constexpr std::array<std::uintptr_t, 4> rings{r::position_ring, r::rotation_ring, r::velocity_ring, r::spin_ring};
    for (std::size_t i = 0; i < rings.size(); ++i) {
        Ring header;
        V4 value{};
        if (!ring(s, rings[i], header, value)) return false;
        if (i == 1) { double norm{}; for (float v : value) norm += static_cast<double>(v) * v; if (norm < .9 || norm > 1.1) return false; }
        if (i >= 2) for (float v : value) if (std::abs(v) > 100) return false;
    }
    alignas(16) const V4 zero{}, ratio{s.definition.mass / s.definition.inertia[0], s.definition.mass / s.definition.inertia[1], s.definition.mass / s.definition.inertia[2], 0};
    if (!body_current(s) || !set_int(service.api.motion, s.body, 2) || !body_current(s) ||
        !scalar(service.api.inverse_mass, s.body, 1.f / s.definition.mass) || !body_current(s) ||
        !vector(service.api.inertia, s.body, ratio.data()) || !body_current(s) || !configured(s) ||
        !touch(service.api.clear, s.body) || !body_current(s) || !vector(service.api.com, s.body, zero.data()) || !body_current(s)) return false;
    for (std::size_t i = 0; i < s.definition.count; ++i) {
        if (!body_current(s)) return false;
        ++s.allocation.shapes_attempted;
        if (!capsule(service.api, s, i)) return false;
        ++s.made;
        ++s.allocation.shapes_confirmed;
        for (std::size_t j = 0; j < i; ++j) if (s.shapes[i] == s.shapes[j]) return false;
        if (!shape_current(s, i, false) || !clear_owner(service.api.owner, s.shapes[i]) || !shape_current(s, i, false) ||
            !mask(service.api.categories, s.shapes[i], r::safe_categories) || !shape_current(s, i, false) ||
            !mask(service.api.collides, s.shapes[i], 0) || !shape_current(s, i)) return false;
    }
    const auto& d = s.definition;
    alignas(16) const std::array<float, 8> pose{d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3], d.position[0], d.position[1], d.position[2], 1};
    if (!pool_current(s) || !pending_zero(s) || !vector(service.api.transform, s.body, pose.data()) || !pool_current(s)) return false;
    for (std::size_t i = 0; i < s.made; ++i)
        if (!mask(service.api.collides, s.shapes[i], r::safe_world_collisions) || !shape_current(s, i)) return false;
    if (!set_int(service.api.motion, s.body, 3) || !pool_current(s, true) || !touch(service.api.wake, s.body) || !pool_current(s, true)) return false;
    s.active = true;
    return true;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    LastError error;
    if (!service.thread) service.thread = GetCurrentThreadId();
    if (GetCurrentThreadId() != service.thread) return;
    bool prepare{};
    {
        Locked lock(service.lock);
        service.base = base;
        service.client = client;
        service.tick_at = GetTickCount64();
        service.eligible = ready;
        // Until a plugin asks for the producer there is no hook, no slot (create needs the hook)
        // and no request to keep current, so the tick reads nothing more, not even the
        // multiplayer state eligible() copies.
        if (!service.initialization_requested && !service.hooked.load(std::memory_order_acquire)) return;
        if (!eligible()) {
            service.initialization_requested = false;
            for (auto& s : service.slots) if (s.attempted && !s.retained) retire(s, false);
            service.world = service.state = service.context = 0;
            return;
        }
        prepare = service.initialization_requested && !service.hook_attempted;
        service.initialization_requested = false;
    }
    // Process-lifetime producer setup can suspend threads. It runs outside both
    // the plugin VM budget and this service lock, and never queues a definition
    // or allocates a plugin body. Only a later explicit create can do that.
    if (prepare) (void)initialize();
    Locked lock(service.lock);
    // Setup may be slow. Do not refresh tick_at to disguise stale readiness.
    if (!eligible()) {
        for (auto& s : service.slots) if (s.attempted && !s.retained) retire(s, false);
        service.world = service.state = service.context = 0;
        return;
    }
    if (!service.hooked.load(std::memory_order_acquire)) return;
    Anchor a;
    if (!capture(a)) { for (auto& s : service.slots) if (s.active) retire(s, false); return; }
    if (a.world != service.world || a.state != service.state || a.context != service.context) {
        for (auto& s : service.slots) if (s.attempted && !s.retained) retire(s, false);
        service.world = a.world;
        service.state = a.state;
        service.context = a.context;
        ++service.world_generation;
    }
    for (auto& s : service.slots) if (s.attempted && !s.retained) {
        if (!s.anchor.same(a)) retire(s, false);
        else if (!s.active || s.quarantined) retire(s, fresh(s));
    }
}
std::string prepare() noexcept {
    LastError error;
    try {
        if (!eligible()) return "owned bodies require the current local client phase outside multiplayer";
        Locked lock(service.lock);
        // The lock may have waited. Do not enqueue setup using stale or foreign
        // client readiness, even though preparation retains no body request.
        if (!eligible()) return "owned bodies require the current local client phase outside multiplayer";
        if (!service.hooked.load(std::memory_order_acquire)) {
            if (service.hook_attempted) return "native body ABI or producer hook unavailable; another producer may own it";
            service.initialization_requested = true;
            return "native body producer setup pending; call prepare again explicitly";
        }
        Anchor anchor;
        Epoch epoch;
        if (!capture(anchor) || !clock(anchor, epoch)) return "waiting for a fresh qualified 60 Hz physics step";
        // Readiness is sampled only. In particular, do not reconcile worlds,
        // retire prior slots, retain a definition, reserve a slot or allocate.
        return {};
    } catch (...) { return "native body preparation failed closed"; }
}
std::string create(std::uint64_t owner, const Definition& definition, Handle& out) noexcept {
    LastError error;
    out = {};
    try {
        if (!owner || !valid(definition)) return "invalid body definition";
        if (!eligible()) return "owned bodies require the current local client phase outside multiplayer";
        Locked lock(service.lock);
        if (!service.hooked.load(std::memory_order_acquire)) {
            if (service.hook_attempted) return "native body ABI or producer hook unavailable; another producer may own it";
            // Preparation is generic and process-lifetime: no owner, definition,
            // position or allocation request survives this refused call.
            service.initialization_requested = true;
            return "waiting for a fresh qualified 60 Hz physics step";
        }
        Anchor anchor;
        Epoch epoch;
        if (!capture(anchor) || !clock(anchor, epoch)) return "waiting for a fresh qualified 60 Hz physics step";
        if (anchor.world != service.world || anchor.state != service.state || anchor.context != service.context) {
            for (auto& s : service.slots) if (s.attempted && !s.retained) retire(s, false);
            service.world = anchor.world;
            service.state = anchor.state;
            service.context = anchor.context;
            ++service.world_generation;
        }
        for (const auto& prior : service.slots)
            if (prior.creation_failed && prior.anchor.world == anchor.world && prior.anchor.state == anchor.state && prior.anchor.context == anchor.context)
                return "a previous partial allocation in this world prohibits another native allocation";
        const auto at = std::find_if(service.slots.begin(), service.slots.end(), [](const Slot& s) { return !s.attempted; });
        if (at == service.slots.end()) return "the eight retained body slots are exhausted; native destruction is unproven";
        auto& s = *at;
        s.attempted = true;
        s.owner = owner;
        s.definition = definition;
        s.anchor = anchor;
        s.handle = {++service.generation, static_cast<std::uint32_t>(at - service.slots.begin())};
        if (!create_native(s)) {
            s.creation_failed = true;
            retire(s, anchor_current(s.anchor));
            return s.quarantined ? "partial native body creation retained in quarantine; allocation retry prohibited" :
                "partial native body creation retained and parked; allocation retry prohibited";
        }
        out = s.handle;
        return {};
    } catch (...) { return "native body creation failed closed"; }
}
std::string read(std::uint64_t owner, const Handle& handle, Sample& out) noexcept {
    LastError error;
    out = {};
    try {
        if (!eligible()) return "body observation unavailable in this client phase";
        Locked lock(service.lock);
        auto* s = lookup(owner, handle);
        if (!s || !s->active || s->retained || s->quarantined) return "body owner or generation unavailable";
        if (!sample(*s, out)) return "body snapshot or native epoch changed; no sample published";
        return {};
    } catch (...) { return "native body observation failed closed"; }
}

body_query::Result probe_batch(std::uint64_t owner, const Handle& handle, const body_query::Request& request, unsigned allowance) noexcept {
    LastError error;
    body_query::Result failed;
    failed.count = request.count <= body_query::maximum_rays ? request.count : 0;
    unsigned performed{};
    bool ray_entered{};
    try {
        if (!body_query::valid(request, 1000)) { failed.failure = body_query::Failure::invalid; return failed; }
        if (!eligible()) { failed.failure = body_query::Failure::client_phase_unavailable; return failed; }
        Locked lock(service.lock);
        const auto* selected = lookup(owner, handle);
        const auto& canonical = request.observation;
        const auto& requested_epoch = canonical.epoch;
        if (!selected || !selected->active || selected->retained || selected->quarantined || !fresh(*selected) ||
            !body_query::same_sample(canonical, selected->latest) || requested_epoch.world != service.world_generation ||
            requested_epoch.body != selected->handle.generation) {
            failed.failure = body_query::Failure::observation_changed;
            return failed;
        }
        const auto& anchor = selected->anchor;
        const auto generation = service.world_generation;
        std::array<const Slot*, body_query::maximum_exclusions + 1> owners{};
        std::size_t excluded_count = 1;
        owners[0] = selected;
        for (std::size_t i = 0; i < request.exclusions; ++i) {
            const auto* other = lookup(owner, request.excluded[i]);
            if (!other || !other->active || other->retained || other->quarantined || !fresh(*other) ||
                !selected->anchor.same(other->anchor)) { failed.failure = body_query::Failure::observation_changed; return failed; }
            if (std::find(owners.begin(), owners.begin() + excluded_count, other) == owners.begin() + excluded_count)
                owners[excluded_count++] = other;
        }
        // The placement root resolver takes a packed entity key, not the raw
        // SkaterEntity pointer held by Anchor. Actor conversion and the root
        // descriptor's actor support are unqualified; never submit that pointer.
        if (request.exclude_local_actor) { failed.failure = body_query::Failure::local_actor_exclusion_unavailable; return failed; }
        const auto read_contact = [](std::uintptr_t at, auto& value) { return peek(at, value); };
        // Read-time categories, when asked for or needed by the named-actor-category
        // check, only while the pinned getter's bytes match this image.
        const bool categories_readable = (request.include_shape_categories || request.require_named_actor_categories_absent) &&
            body_shape_categories::layout_qualified(service.base, read_contact);
        // That check needs the filter the native ray will actually send to share no
        // bit with the named actor categories, and readable categories. Otherwise
        // nothing could vouch for a returned hit, so nothing is cast.
        const auto filter = editor::surface_query_filter(request.preset);
        if (request.require_named_actor_categories_absent &&
            (!filter || (*filter & addr::park_editor::named_actor_categories) || !categories_readable)) {
            failed.failure = body_query::Failure::named_actor_categories_unavailable;
            return failed;
        }
        for (const auto field : {anchor.state + r::shape_allocated_bits, anchor.state + r::shape_generations,
            anchor.state + r::shape_body_indices, anchor.world + r::world_body_generations}) {
            std::uintptr_t array{};
            if (!peek(field, array) || !pointer(array)) { failed.failure = body_query::Failure::body_exclusions_unavailable; return failed; }
        }
        body_hit_identity::Witnesses<body_query::maximum_rays> witnesses;
        std::array<body_contact_read::Motion, body_query::maximum_rays> motions{};
        std::size_t motion_count{};
        const auto world_current = [&] { return service.world_generation == generation && anchor_current(anchor); };
        const auto qualify = [&](const NativeHandle& shape, NativeHandle& body) {
            return body_hit_identity::resolve(anchor.world, anchor.state, shape, body,
                [](std::uintptr_t at, auto& value) { return peek(at, value); }, world_current,
                [&](const NativeHandle& value) { return valid_handle(service.api.shape_valid, value) && world_current(); },
                [&](std::uint32_t index) { return allocated(anchor.state, index); },
                [&](const NativeHandle& value) {
                    return value.world == anchor.world && world_current() && allocated(anchor.state, value.index) &&
                        valid_handle(service.api.body_valid, value) && allocated(anchor.state, value.index) && world_current();
                });
        };
        const auto motion_body_current = [&](const NativeHandle& body) {
            return world_current() && body.world == anchor.world && allocated(anchor.state, body.index) &&
                valid_handle(service.api.body_valid, body) && allocated(anchor.state, body.index) && world_current();
        };
        // Optional read-time categories. A getter byte mismatch leaves every value
        // absent and refuses the batch only for the named-actor-category check
        // above. Nothing here outlives this call.
        body_shape_categories::Sidecars<body_query::maximum_rays> categories;
        // Optional owner slots under the same rules, only when asked for. resolve()
        // already compared set_shape_owner's bytes, which reach state+1008 through
        // World+28 and index it by the shape's own index.
        body_shape_categories::Sidecars<body_query::maximum_rays> hit_owners{pb::body_owner_offset};
        // Around each category or owner read: the same World/state and SDK world generation,
        // the requested listener epoch and sample, and the original shape/body pair.
        const auto hit_current = [&](const NativeHandle& shape, const NativeHandle& body) {
            NativeHandle current;
            Epoch epoch{};
            if (!world_current() || !qualify(shape, current) || current != body || !clock(anchor, epoch)) return false;
            epoch.world = generation;
            epoch.body = selected->handle.generation;
            return epoch == requested_epoch && body_query::same_sample(canonical, selected->latest) && world_current();
        };
        std::size_t ordinal{};
        const auto fence = [&] {
            Anchor current;
            Epoch epoch{};
            if (!eligible() || service.world_generation != generation || !capture(current) || !anchor.same(current) ||
                !watched(anchor) || !watched(current) || !clock(current, epoch)) return false;
            epoch.world = generation;
            epoch.body = selected->handle.generation;
            if (epoch != requested_epoch || !body_query::same_sample(canonical, selected->latest)) return false;
            for (std::size_t i = 0; i < excluded_count; ++i)
                if (!owners[i]->active || owners[i]->retained || owners[i]->quarantined || !fresh(*owners[i])) return false;
            Sample measured;
            if (!observe(*selected, measured) || !body_query::same_sample(canonical, measured) ||
                !witnesses.current([&](const body_hit_identity::Witness& witness) {
                    NativeHandle body;
                    return qualify(witness.shape, body) && body == witness.body;
                })) return false;
            for (std::size_t i = 0; i < motion_count; ++i)
                if (!body_contact_read::motion_current(anchor.world, anchor.state, motions[i], read_contact, motion_body_current)) return false;
            // Identity drift seen while capturing or rechecking a category or owner
            // refuses the batch and stays refused; value-only drift drops just that value.
            if (!categories.current(read_contact, hit_current) || !hit_owners.current(read_contact, hit_current)) return false;
            Epoch after{};
            after.world = generation;
            after.body = selected->handle.generation;
            return world_current() && clock(anchor, after) && after == requested_epoch &&
                body_query::same_sample(canonical, selected->latest);
        };
        // execute allows each ray at least one cast, and the native query makes exactly one.
        const auto query = [&](const body_query::Ray& ray, unsigned) {
            body_query::Probe out;
            // execute calls this once per requested ray, in order, so the count is
            // that ray's ordinal. A category sidecar binds to it, never to a body.
            const auto index = ordinal++;
            // Native ignored IDs are owner indices. Query without either list or
            // the ECS owner resolver; classify only the actual nearest hit below.
            ray_entered = true;
            // The preset selects the native filter only. The nearest hit it returns
            // still passes the same owned-body, identity, contact and fence checks.
            const auto hit = probe_local_surface_counted(ray.from, ray.to, {true, request.require_contact, request.preset});
            performed += hit.native_casts;
            out.native_casts = hit.native_casts;
            if (!hit.available || hit.context != anchor.context || hit.world != anchor.world) return out;
            if (!hit.hit) { out.hit.status = body_query::Status::clear; return out; }
            NativeHandle shape, body;
            std::memcpy(&shape, hit.hit_shape.data(), sizeof(shape));
            // Each refusal of the actual hit names why, so a caller can tell an
            // owned-body hit from a hit whose identity could not be qualified.
            if (!qualify(shape, body)) { out.failure = body_query::Failure::hit_identity_unqualified; return out; }
            for (std::size_t i = 0; i < excluded_count; ++i)
                if (body.world == owners[i]->body.world && body.index == owners[i]->body.index) {
                    out.failure = body_query::Failure::owned_body_hit;
                    return out;
                }
            if (!witnesses.add({shape, body})) return out;
            // Only after qualification and the owned exclusion above, indexed by the
            // original shape. The values stay beside the batch; this probe never has them.
            // An identity loss here is sticky, so the fence execute runs next reports it
            // as an observation change rather than as this ray's reason.
            if ((categories_readable &&
                    !categories.capture(index, anchor.world, anchor.state, shape, body, read_contact, hit_current)) ||
                (request.include_hit_owner &&
                    !hit_owners.capture(index, anchor.world, anchor.state, shape, body, read_contact, hit_current))) return out;
            out.hit.status = body_query::Status::hit;
            out.hit.position = hit.position;
            out.hit.fraction = hit.fraction;
            out.hit.normal = hit.normal;
            out.hit.normal_available = hit.normal_available;
            body_contact_read::Motion motion;
            if (request.require_contact && hit.normal_available && body_contact_read::motion(anchor.world, anchor.state, body, hit.position, motion, read_contact, motion_body_current) &&
                motion_count < motions.size()) {
                motions[motion_count++] = motion;
                out.hit.surface_velocity = motion.velocity;
                out.hit.surface_velocity_available = true;
            }
            // No public native body identity is minted. Unknown details stay absent;
            // actor exclusions above remain unavailable until their own conversion is qualified.
            return out;
        };
        auto result = body_query::execute(request, allowance, 1000, query, fence);
        // The named-actor-category check judges only a result that passed execute's
        // closing fence, over the values that survived it, before anything is
        // published. Any failure refuses the whole batch. The fact is set only here;
        // local_actor_excluded is never set on this path.
        if (result.available && request.require_named_actor_categories_absent) {
            const auto failure = categories.require(result, *filter, addr::park_editor::named_actor_categories);
            if (failure != body_query::Failure::none) body_query::refuse(result, failure);
            else result.named_actor_categories_absent = true;
        }
        // Only a result that passed execute's closing fence receives categories or owner slots.
        categories.publish(request, result);
        hit_owners.publish(request, result);
        return result;
    } catch (...) {
        failed.native_casts = performed;
        failed.accounting_valid = !ray_entered;
        failed.failure = ray_entered ? body_query::Failure::query_backend_exception : body_query::Failure::query_preconditions_unavailable;
        return failed;
    }
}
std::string submit(std::uint64_t owner, const Handle& handle, const Command& command, std::uint64_t& sequence) noexcept {
    LastError error;
    sequence = 0;
    try {
        if (!eligible()) return "body commands require the current local client phase";
        Locked lock(service.lock);
        auto* s = lookup(owner, handle);
        if (!s || !s->active || s->retained || s->quarantined || !fresh(*s)) return "body owner or generation changed";
        Epoch current{};
        if (!command_clock(*s, current) || current != s->latest.epoch) return "measured native body epoch expired";
        if (service.command_sequence == std::numeric_limits<std::uint64_t>::max() ||
            !s->mailbox.submit(command, current, GetTickCount64(), service.command_sequence + 1))
            return "body command invalid, repeated, stopped, pending or receipt ring full";
        sequence = ++service.command_sequence;
        return {};
    } catch (...) { return "native body command failed closed"; }
}
std::string receipt(std::uint64_t owner, const Handle& handle, std::uint64_t selected, Receipt& out) noexcept {
    LastError error;
    out = {};
    try {
        if (GetCurrentThreadId() != service.thread) return "body receipts are client phase only";
        Locked lock(service.lock);
        auto* s = lookup(owner, handle);
        if (!s) return "body owner or generation unavailable";
        if (s->mailbox.pending && GetTickCount64() >= s->mailbox.deadline) {
            Command ignored;
            (void)s->mailbox.consume(s->mailbox.command.epoch, GetTickCount64(), ignored);
        }
        if (s->active && !s->quarantined && s->mailbox.needs_successor()) { Sample observed; (void)sample(*s, observed); }
        if (!s->mailbox.find(selected, out)) return "body receipt sequence unavailable for this owner and generation";
        out.retained = s->retained;
        out.quarantined = s->quarantined;
        return {};
    } catch (...) { return "native body receipt failed closed"; }
}
std::string release(std::uint64_t owner, const Handle& handle) noexcept {
    LastError error;
    try {
        if (GetCurrentThreadId() != service.thread) return "body release is client phase only";
        Locked lock(service.lock);
        auto* s = lookup(owner, handle);
        if (!s) return "body owner or generation unavailable";
        if (!s->retained) retire(*s, eligible() && fresh(*s));
        return s->quarantined ? "body retained in quarantine; fresh owner unavailable, no native destruction attempted" : std::string{};
    } catch (...) { return "native body release failed closed"; }
}
std::string acknowledge(std::uint64_t owner, const Handle& handle, std::uint64_t sequence) noexcept {
    LastError error;
    if (GetCurrentThreadId() != service.thread) return "body acknowledgement is client phase only";
    Locked lock(service.lock);
    auto* s = lookup(owner, handle);
    if (!s || !s->mailbox.acknowledge(sequence)) return "receipt unavailable, already acknowledged or awaiting a measured successor";
    return {};
}
void release_all(std::uint64_t owner) noexcept {
    LastError error;
    if (GetCurrentThreadId() != service.thread) return;
    Locked lock(service.lock);
    for (auto& s : service.slots) if (s.attempted && s.owner == owner && !s.retained) retire(s, eligible() && fresh(s));
}
} // namespace dingosdk::owned_bodies
