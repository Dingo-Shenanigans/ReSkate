#include "bmx_chassis_probe.h"
#include "bmx_native_motion.h"
#include "bmx_runtime.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Engine/Game/Build/image_identity.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <vector>

namespace dingosdk::bmx {
namespace {
using V4 = std::array<float, 4>;
using namespace addr::remote_collision;
constexpr std::size_t sample_limit = 2048;
constexpr ULONGLONG wall_limit_ms = 60000;
constexpr double pulse_seconds = .15, pulse_latest_seconds = .30, end_seconds = 6;
constexpr std::uint32_t all_categories = 0xffffffffU;
// Retained full supported-image evidence: rea-fbphysics-native-v2-evidence.json.
// Run56 pending-to-velocity readback establishes force/torque-like one-step
// acceleration accumulation at60Hz, not immediate delta-v/delta-omega impulses.
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
constexpr bool global_worker_barrier_proven = false;

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
    SetVector com{}, transform{}, linear{}, angular{}, force{}, torque{}, inertia_ratio{};
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
struct ChainWatch {
    std::uintptr_t address{};
    std::array<unsigned char, 16> value{};
    std::size_t size{};
};
struct Configuration {
    const char* variant = "baseline";
    float mass = 50, inverse_mass = .02f;
    V4 inertia_ratio{1,1,1,0};
    std::uint32_t categories = all_categories, collides_with = all_categories;
};
// Fixed-size immutable publication. No SourceReader/vector/JSON/logging work
// occurs in the native hook, and no native lifetime is owned by its destructor.
struct PulseLease {
    Api api;
    Configuration configuration;
    Handle body, shape;
    std::uintptr_t world{}, native_state{}, client_context{};
    std::uintptr_t client{}, entity{}, core{}, rig{}, skater_context{};
    std::array<std::uintptr_t, 32> wrappers{};
    std::array<ChainWatch, 96> watches{};
    std::size_t watch_count{};
    ULONGLONG until{};
    double start_time{}, queued_elapsed{};
    std::uint64_t queued_serial{};
    bool active{};
};
struct PulseObservation {
    std::uintptr_t handle_address{}, wrapper{}, world{};
    std::uint32_t index{}, generation{};
    DWORD thread{};
    bool wrapper_match{}, same_world{};
};
struct PulsePacket {
    bool attempted{}, force_submitted{}, torque_submitted{}, pre_valid{}, post_valid{};
    const char* status = "not_observed";
    std::uintptr_t handle_address{}, wrapper{};
    Handle incoming, own_body;
    DWORD thread{};
    std::size_t matched_part{};
    std::uint32_t wrapper_flags_after_original{};
    std::uint64_t native_serial{};
    double elapsed{};
    V4 before_linear_acceleration{}, before_angular_acceleration{}, after_linear_acceleration{}, after_angular_acceleration{};
    std::uintptr_t before_linear_acceleration_source{}, before_angular_acceleration_source{}, after_linear_acceleration_source{}, after_angular_acceleration_source{};
};
struct PulseShared {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<Synchronize> original{};
    std::atomic<bool> ready{}, consumed{};
    std::atomic<std::uint64_t> calls{}, lock_misses{}, lease_calls{}, expired{}, wrapper_matches{},
        world_matches{}, owner_rejected{}, clock_rejected{};
    PulseLease lease;
    PulsePacket packet;
    std::array<PulseObservation, 8> observations{};
    std::size_t observation_count{};
    const char* last_rejection = "none";
};
PulseShared& pulse_shared() { static auto* value = new PulseShared; return *value; }
struct State {
    std::atomic<bool> accepted{}, queued{};
    std::filesystem::path root;
    Api api;
    Configuration configuration;
    NativeFrame owner;
    Handle body, shape;
    std::uintptr_t world{}, native_state{}, client_context{};
    DWORD client_thread{};
    ULONGLONG requested_at{}, report_at{};
    std::uint64_t serial{}, skipped{};
    double start_time{}, last_time{};
    bool allocation_attempted{}, seeded{}, pulsed{}, pulse_marked{}, done{}, defaults_verified{}, stopped{};
    bool body_observed{}, shape_observed{}, forgotten{};
    bool pulse_requested{}, pulse_hook_attempted{};
    const char* pulse_status = "withheld_unverified_native_sync_phase";
    Snapshot defaults;
    std::array<float, 3> floor{}, spawn{};
    const char* status = "not_requested";
    const char* cleanup = "not_allocated";
    std::vector<Json> samples;
};
State& state() { static auto* value = new State; return *value; }
struct LastError { DWORD value = GetLastError(); ~LastError() { SetLastError(value); } };
template<class T> bool read(std::uintptr_t p, T& out) noexcept { return memory::peek(p, out); }
bool pointer(std::uintptr_t p) noexcept { return p >= 0x10000 && p < memory::highest_user_address - 0x20000; }
template<std::size_t N> bool finite(const std::array<float, N>& v, float bound = 1000000) noexcept {
    for (float f : v) if (!std::isfinite(f) || std::abs(f) > bound) return false;
    return true;
}
bool opt_in(std::filesystem::path* root = nullptr) noexcept {
    try {
        wchar_t value[2]{};
        if (GetEnvironmentVariableW(L"RESKATE_BMX_CHASSIS_PROBE", value, 2) != 1 || value[0] != L'1' ||
            !user_data::development_requested()) return false;
        const auto isolated = user_data::override_root();
        if (!isolated) return false;
        if (root) *root = *isolated;
        return true;
    } catch (...) { return false; }
}
Configuration selected_configuration() noexcept {
    Configuration result;
    wchar_t value[32]{};
    const auto n = GetEnvironmentVariableW(L"RESKATE_BMX_CHASSIS_PROBE_VARIANT",value,32);
    if (n == 20 && std::wstring_view(value,n) == L"mass-inertia-terrain") {
        result.variant = "mass-inertia-terrain";
        result.mass = 25; result.inverse_mass = .04f;
        result.inertia_ratio = {2,3,4,0};
        // Authored OnlyTerrainCollision: InCategories=[], CollidesWith=[8,0].
        result.categories = 0; result.collides_with = 0x101;
    }
    return result;
}
bool solo() {
    const auto mp = multiplayer::model();
    return !mp.active && !mp.lobby_joining;
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
        &body_quaternion, &set_inertia_ratio}) if (!matches(base, *f)) return false;
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
bool capsule(const Api* a, std::uintptr_t world, const Handle* body, Handle* shape) noexcept {
    __try {
        __declspec(align(16)) const float bottom[4]{0,-.15f,0,0}, top[4]{0,.15f,0,0};
        return a->capsule(world, shape, body, bottom, top, .2f) == shape &&
            shape->world == world && valid(a->shape_valid, shape);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
bool current_world(State& s, const NativeFrame& f) noexcept {
    std::uintptr_t world{}, native_state{};
    std::uintptr_t entity_context{};
    return GetCurrentThreadId() == s.client_thread && same_owner(s.owner, f) &&
        read(f.entity + 0x20, entity_context) && entity_context == s.client_context &&
        client_context(s.api.base, f.client, s.client_context) && world_of(s.api.world, s.client_context, &world) &&
        world == s.world && read(world + 0x28, native_state) && native_state == s.native_state;
}
bool own_body(State& s, const NativeFrame& f) noexcept {
    return s.body_observed && current_world(s, f) && s.body.world == s.world &&
        allocated_body(s.native_state,s.body.index) && valid(s.api.body_valid, &s.body) &&
        current_world(s,f) && allocated_body(s.native_state,s.body.index) && valid(s.api.body_valid,&s.body);
}
bool fresh_shape(State& s, const NativeFrame& f) noexcept {
    return own_body(s, f) && s.shape.world == s.world && valid(s.api.shape_valid, &s.shape) &&
        current_world(s, f);
}
bool own_shape(State& s, const NativeFrame& f) noexcept {
    std::uintptr_t owners{}, categories{}, with{};
    std::uint32_t owner{}, category{}, collides{};
    if (!own_body(s, f) || s.shape.world != s.world || !valid(s.api.shape_valid, &s.shape) ||
        !read(s.native_state + 0x1008, owners) || !pointer(owners) ||
        !read(s.native_state + 0xfe8, categories) || !pointer(categories) ||
        !read(s.native_state + 0xff0, with) || !pointer(with) ||
        !read(owners + std::uintptr_t{s.shape.index} * 4, owner) || owner != 0xffffffffU ||
        !read(categories + std::uintptr_t{s.shape.index} * 4, category) ||
        !read(with + std::uintptr_t{s.shape.index} * 4, collides) ||
        (category != 0 && category != s.configuration.categories) ||
        (collides != 0 && collides != s.configuration.collides_with)) return false;
    return current_world(s, f) && valid(s.api.body_valid, &s.body) && valid(s.api.shape_valid, &s.shape);
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
bool ring4(std::uintptr_t native_state, std::uintptr_t offset, std::uint32_t index, Ring& ring, V4& out) noexcept {
    if (!read(native_state + offset, ring) || !pointer(ring.values) || !ring.elements ||
        ring.elements > 1048576 || index >= ring.elements || !ring.planes || ring.planes > 64 ||
        ring.current_plane >= ring.planes) return false;
    // Exact native arithmetic (25d5cc0 / reset2641070). elements is capacity;
    // current_plane is a u16 ring head, not a byte stride. Bounds above prohibit
    // the native 27-bit mask or 32-bit addition from discarding useful bits.
    const std::uint32_t quads = ring.elements * 4;
    const std::uint32_t plane_bytes = (((quads & 0x7ffffffU) * 4 + 0x3cU) & 0xffffffc0U);
    const auto address = ring.values + std::uintptr_t{plane_bytes} * ring.current_plane + std::uintptr_t{index} * 16;
    return address >= ring.values && read(address, out) && finite(out);
}
bool snapshot(State& s, const NativeFrame& f, Snapshot& out) noexcept {
    if (!own_body(s, f)) return false;
    Snapshot v;
    v.native_state = s.native_state;
    V4 raw_quaternion{};
    if (!ring4(s.native_state, 0x900, s.body.index, v.rings[0], v.position) ||
        !ring4(s.native_state, 0x918, s.body.index, v.rings[1], raw_quaternion) ||
        !ring4(s.native_state, 0x930, s.body.index, v.rings[2], v.linear) ||
        !ring4(s.native_state, 0x940, s.body.index, v.rings[3], v.angular) ||
        !array4(s.native_state, 0x910, s.body.index, v.local_com, v.direct_arrays[0]) ||
        !array4(s.native_state, 0x928, s.body.index, v.local_quaternion_wxyz, v.direct_arrays[1]) ||
        !array4(s.native_state, 0x988, s.body.index, v.inertia_ratio, v.direct_arrays[2])) return false;
    std::uintptr_t inverse_masses{}, inverse_check{};
    if (!read(s.native_state + 0x980, inverse_masses) || !pointer(inverse_masses) ||
        !read(inverse_masses + std::uintptr_t{s.body.index} * 4, v.inverse_mass) ||
        !std::isfinite(v.inverse_mass) || v.inverse_mass <= 0 || v.inverse_mass > 1000 ||
        !read(s.native_state + 0x980, inverse_check) || inverse_check != inverse_masses) return false;
    v.direct_arrays[3] = inverse_masses;
    // Direct SoA pose is used only for this newly allocated zero-COM,
    // identity-local-frame body. Shape pose composition is a different helper.
    if (v.local_com != V4{} || v.local_quaternion_wxyz != V4{1,0,0,0}) return false;
    float norm{};
    for (float value : raw_quaternion) norm += value * value;
    if (!std::isfinite(norm) || norm < .9f || norm > 1.1f || !finite(v.linear, 100) || !finite(v.angular, 100)) return false;
    v.quaternion_xyzw = {raw_quaternion[1], raw_quaternion[2], raw_quaternion[3], raw_quaternion[0]};
    constexpr std::array<std::uintptr_t, 4> offsets{0x900,0x918,0x930,0x940};
    const std::array<V4, 4> expected{v.position,raw_quaternion,v.linear,v.angular};
    // Header stability alone does not exclude a native in-place write. Require
    // equal second content reads and reroot every source before accepting the
    // diagnostic sample. This is not an engine-wide atomic snapshot guarantee.
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        Ring again;
        V4 data{};
        if (!ring4(s.native_state, offsets[i], s.body.index, again, data) ||
            again != v.rings[i] || data != expected[i]) return false;
    }
    constexpr std::array<std::uintptr_t, 3> direct_offsets{0x910,0x928,0x988};
    const std::array<V4, 3> direct_expected{v.local_com,v.local_quaternion_wxyz,v.inertia_ratio};
    for (std::size_t i = 0; i < direct_offsets.size(); ++i) {
        V4 data{};
        std::uintptr_t source{};
        if (!array4(s.native_state, direct_offsets[i], s.body.index, data, source) ||
            source != v.direct_arrays[i] || data != direct_expected[i]) return false;
    }
    float inverse_again{};
    if (!read(s.native_state+0x980, inverse_check) || inverse_check != v.direct_arrays[3] ||
        !read(inverse_check+std::uintptr_t{s.body.index}*4, inverse_again) || inverse_again != v.inverse_mass ||
        !read(s.native_state+0x980, inverse_masses) || inverse_masses != inverse_check) return false;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        Ring again;
        if (!read(s.native_state + offsets[i], again) || again != v.rings[i]) return false;
    }
    if (!own_body(s, f)) return false;
    out = v;
    return true;
}
bool pulse_opt_in() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_CHASSIS_PROBE_PULSE",value,2) == 1 && value[0] == L'1';
}
bool watched_chain(const PulseLease& l) noexcept {
    if (!l.active || !l.watch_count || l.watch_count > l.watches.size()) return false;
    for (std::size_t i = 0; i < l.watch_count; ++i) {
        const auto& watch = l.watches[i];
        std::array<unsigned char,16> actual{};
        if (!watch.size || watch.size > actual.size() ||
            !memory::peek_bytes(watch.address,actual.data(),watch.size) || actual != watch.value) return false;
    }
    return true;
}
bool pulse_owner(const PulseLease& l, const Handle& incoming, const char*& rejection) noexcept {
    std::uintptr_t world{}, native_state{}, owners{}, categories{}, collides{};
    std::uint32_t capacity{}, owner{}, category{}, with{};
    if (!watched_chain(l)) { rejection = "fresh_published_owner_chain_changed"; return false; }
    if (!client_context(l.api.base,l.client,l.client_context)) { rejection = "producer_client_tls_or_loading_guard_rejected"; return false; }
    if (!world_of(l.api.world,l.client_context,&world) || world != l.world || incoming.world != world ||
        l.body.world != world || l.shape.world != world || !read(world+0x28,native_state) || native_state != l.native_state ||
        !read(native_state+0x908,capacity) || !capacity || capacity > 1048576 ||
        incoming.index >= capacity || l.body.index >= capacity || !valid(l.api.body_valid,&incoming) ||
        !allocated_body(native_state,incoming.index) || !allocated_body(native_state,l.body.index,true) ||
        !valid(l.api.body_valid,&l.body) || !valid(l.api.shape_valid,&l.shape)) {
        rejection = "native_world_capacity_or_handle_generation_changed"; return false;
    }
    if (!read(native_state+0x1008,owners) || !pointer(owners) ||
        !read(native_state+0xfe8,categories) || !pointer(categories) ||
        !read(native_state+0xff0,collides) || !pointer(collides) ||
        !read(owners+std::uintptr_t{l.shape.index}*4,owner) || owner != 0xffffffffU ||
        !read(categories+std::uintptr_t{l.shape.index}*4,category) || category != l.configuration.categories ||
        !read(collides+std::uintptr_t{l.shape.index}*4,with) || with != l.configuration.collides_with) {
        rejection = "probe_shape_owner_or_collision_masks_changed"; return false;
    }
    if (!configured_body(native_state,l.body.index,l.configuration)) {
        rejection = "probe_inverse_mass_or_inertia_ratio_changed"; return false;
    }
    // Revalidate the immutable owner chain and the handle generations at each
    // producer call. No foreign-body or old-world fallback is permitted.
    if (!watched_chain(l) || !read(world+0x28,native_state) || native_state != l.native_state ||
        !allocated_body(native_state,incoming.index) || !allocated_body(native_state,l.body.index,true) ||
        !valid(l.api.body_valid,&incoming) || !valid(l.api.body_valid,&l.body) || !valid(l.api.shape_valid,&l.shape) ||
        !configured_body(native_state,l.body.index,l.configuration)) {
        rejection = "final_owner_world_or_generation_recheck_rejected"; return false;
    }
    return true;
}
bool pending_accelerations(std::uintptr_t native_state, std::uint32_t index, V4& linear, V4& angular,
    std::uintptr_t& linear_source, std::uintptr_t& angular_source) noexcept {
    V4 a{},b{};
    std::uintptr_t as{},bs{};
    return array4(native_state,0x970,index,linear,linear_source) &&
        array4(native_state,0x978,index,angular,angular_source) &&
        array4(native_state,0x970,index,a,as) && a == linear && as == linear_source &&
        array4(native_state,0x978,index,b,bs) && b == angular && bs == angular_source;
}
void pulse_after_original(Handle* incoming_pointer, std::uintptr_t wrapper) noexcept {
    auto& p = pulse_shared();
    p.calls.fetch_add(1,std::memory_order_relaxed);
    if (!p.ready.load(std::memory_order_acquire) || p.consumed.load(std::memory_order_acquire)) return;
    if (!TryAcquireSRWLockExclusive(&p.lock)) { p.lock_misses.fetch_add(1,std::memory_order_relaxed); return; }
    // All work while holding this SDK-only lock is bounded POD reads, clock
    // copy and the two identified native force/torque arithmetic helpers. No allocation,
    // logging, JSON, SourceReader, callback registration or original call.
    const auto attempt = [&]() noexcept {
        auto& l = p.lease;
        if (!l.active) return;
        p.lease_calls.fetch_add(1,std::memory_order_relaxed);
        const auto now = GetTickCount64();
        if (now >= l.until) { p.expired.fetch_add(1,std::memory_order_relaxed); return; }
        Handle incoming;
        if (!read(reinterpret_cast<std::uintptr_t>(incoming_pointer),incoming)) return;
        std::size_t part = l.wrappers.size();
        for (std::size_t i = 0; i < l.wrappers.size(); ++i) if (l.wrappers[i] == wrapper) { part = i; break; }
        const bool exact = part < l.wrappers.size(), same_world = incoming.world == l.world;
        if (p.observation_count < p.observations.size()) p.observations[p.observation_count++] = {
            reinterpret_cast<std::uintptr_t>(incoming_pointer),wrapper,incoming.world,incoming.index,incoming.generation,
            GetCurrentThreadId(),exact,same_world};
        if (!exact) return;
        p.wrapper_matches.fetch_add(1,std::memory_order_relaxed);
        if (!same_world) return;
        p.world_matches.fetch_add(1,std::memory_order_relaxed);
        if (!pulse_owner(l,incoming,p.last_rejection) || mounted()) {
            if (mounted()) p.last_rejection = "mounted_bmx_takes_precedence";
            p.owner_rejected.fetch_add(1,std::memory_order_relaxed); return;
        }
        std::uint64_t serial{};
        double time{};
        if (!read_native_clock(l.entity,l.core,serial,time) || serial < l.queued_serial ||
            !std::isfinite(time) || time-l.start_time < pulse_seconds || time-l.start_time > pulse_latest_seconds) {
            p.last_rejection = "native_clock_not_in_point15_to_point30_window";
            p.clock_rejected.fetch_add(1,std::memory_order_relaxed); return;
        }
        PulsePacket packet;
        packet.handle_address = reinterpret_cast<std::uintptr_t>(incoming_pointer);
        packet.wrapper = wrapper; packet.incoming = incoming; packet.own_body = l.body;
        packet.thread = GetCurrentThreadId(); packet.matched_part = part;
        packet.native_serial = serial; packet.elapsed = time-l.start_time;
        (void)read(wrapper+0x50,packet.wrapper_flags_after_original);
        packet.pre_valid = pending_accelerations(l.native_state,l.body.index,packet.before_linear_acceleration,packet.before_angular_acceleration,
            packet.before_linear_acceleration_source,packet.before_angular_acceleration_source);
        if (!packet.pre_valid || !pulse_owner(l,incoming,p.last_rejection) || GetTickCount64() >= l.until) {
            if (!packet.pre_valid) p.last_rejection = "pending_vectors_or_sources_not_equal_on_double_read";
            p.owner_rejected.fetch_add(1,std::memory_order_relaxed); return;
        }
        // Consume BEFORE invoking either helper. Partial native failures are
        // reported and never retried. No direct stores to970/978 occur here.
        bool expected = false;
        if (!p.consumed.compare_exchange_strong(expected,true,std::memory_order_acq_rel)) return;
        packet.attempted = true;
        alignas(16) const V4 force{5,0,0,0}, torque{0,5,0,0};
        packet.force_submitted = vector(l.api.force,&l.body,force.data());
        if (packet.force_submitted && pulse_owner(l,incoming,p.last_rejection) && GetTickCount64() < l.until &&
            read_native_clock(l.entity,l.core,serial,time) && serial >= packet.native_serial &&
            std::isfinite(time) && time-l.start_time >= pulse_seconds && time-l.start_time <= pulse_latest_seconds)
            packet.torque_submitted = vector(l.api.torque,&l.body,torque.data());
        packet.post_valid = pulse_owner(l,incoming,p.last_rejection) && pending_accelerations(l.native_state,l.body.index,packet.after_linear_acceleration,packet.after_angular_acceleration,
            packet.after_linear_acceleration_source,packet.after_angular_acceleration_source);
        packet.status = packet.force_submitted && packet.torque_submitted ? "submitted_once_in_matched_retail_producer_phase" :
            "partial_or_failed_native_pulse_never_retried";
        p.packet = packet;
        l.active = false;
    };
    attempt();
    ReleaseSRWLockExclusive(&p.lock);
}
std::uintptr_t synchronize_hook(Handle* handle, std::uintptr_t wrapper) {
    // The prepared permanent relay is
    // published before enabling this hook and is never removed or replaced.
    // Forward native args exactly once; retain the original's LastError result.
    const auto original = pulse_shared().original.load(std::memory_order_acquire);
    const auto result = original(handle,wrapper);
    const auto original_error = GetLastError();
    pulse_after_original(handle,wrapper);
    SetLastError(original_error);
    return result;
}
bool install_pulse_hook(State& s) noexcept {
    if (s.pulse_hook_attempted) return pulse_shared().ready.load(std::memory_order_acquire);
    s.pulse_hook_attempted = true;
    auto& p = pulse_shared(); // allocate SDK bookkeeping before the hook exists
    if (!matches(s.api.base,synchronizer)) return false;
    const auto initialized = hook_initialize();
    if (initialized != HookOk && initialized != HookAlreadyInitialized) return false;
    void* original{};
    auto* const target = reinterpret_cast<void*>(s.api.base+synchronizer.rva);
    if (hook_prepare(target,reinterpret_cast<void*>(&synchronize_hook),&original) != HookOk || !original) return false;
    p.original.store(reinterpret_cast<Synchronize>(original),std::memory_order_release);
    if (hook_enable(target) != HookOk) return false;
    p.ready.store(true,std::memory_order_release);
    return true;
}
template<class T> T chain_read(client_source::detail::SourceReader& reader, PulseLease& l,
    std::uintptr_t address) {
    client_source::detail::source_require(l.watch_count < l.watches.size(),"Probe chain watch bound exceeded");
    const auto value = reader.value<T>(address);
    ChainWatch watch{address,{},sizeof(T)};
    std::memcpy(watch.value.data(),&value,sizeof(T));
    l.watches[l.watch_count++] = watch;
    return value;
}
bool publish_pulse(State& s, const NativeFrame& frame) noexcept {
    using namespace client_source::detail;
    try {
        if (!s.pulse_requested || !pulse_opt_in() || !pulse_shared().ready.load(std::memory_order_acquire) ||
            pulse_shared().consumed.load(std::memory_order_acquire) || !own_shape(s,frame)) return false;
        auto& sources = source_state();
        if (!sources.initialized.load(std::memory_order_acquire) || sources.trial.base != s.api.base ||
            sources.busy.test_and_set(std::memory_order_acquire)) return false;
        SourceBusyScope busy{sources.busy};
        const auto bodies = debug_noclip_bodies(s.api.base,frame.client,frame.entity);
        if (bodies.core != frame.core || bodies.context != frame.context || bodies.rig_wrapper != frame.rig) return false;
        PulseLease l;
        l.api = s.api; l.configuration = s.configuration;
        l.body = s.body; l.shape = s.shape; l.world = s.world; l.native_state = s.native_state;
        l.client_context = s.client_context; l.client = frame.client; l.entity = frame.entity;
        l.core = frame.core; l.rig = frame.rig; l.skater_context = frame.context;
        l.start_time = s.start_time; l.queued_elapsed = frame.simulation_time-s.start_time; l.queued_serial = frame.serial;
        SourceReader reader;
        const auto ptr = [&](std::uintptr_t at) { return chain_read<std::uintptr_t>(reader,l,at); };
        const auto u32 = [&](std::uintptr_t at) { return chain_read<std::uint32_t>(reader,l,at); };
        const auto u8 = [&](std::uintptr_t at) { return chain_read<std::uint8_t>(reader,l,at); };
        source_require(ptr(frame.client) == s.api.base+addr::engine::client_vtable &&
            ptr(frame.client+8) == s.client_context,"Probe client changed");
        const auto phase = u32(frame.client+0xc4);
        source_require(phase == 13 || phase == 21,"Probe client loading");
        const auto offset = u32(s.api.base+addr::engine::context_player_manager_offset);
        source_require(offset <= 0x1000000,"Probe player manager offset changed");
        const auto manager = ptr(s.client_context+offset);
        source_require(ptr(manager) == s.api.base+addr::engine::local_player_manager_vtable,"Probe player manager changed");
        const auto begin = ptr(manager+0x4c8), end = ptr(manager+0x4d0);
        source_require(reader.count(begin,end,8) == 1,"Probe needs unique local player");
        const auto player = ptr(begin);
        source_require(ptr(player) == s.api.base+addr::engine::local_player_vtable && ptr(player+0x78) == s.client_context &&
            u8(player+0x45) == 1 && u8(player+0x44) == 0 && ptr(player+0xb8) == frame.entity,"Probe local player changed");
        const auto handle = ptr(player+0xb0);
        source_require(ptr(frame.entity) == s.api.base+addr::engine::skater_entity_vtable &&
            ptr(frame.entity+0x20) == s.client_context && ptr(frame.entity+0xf8) == player && ptr(handle) == frame.entity+8 &&
            u8(frame.entity+0x7e0) == 0,"Probe player entity changed");
        const auto collection = ptr(frame.entity+0x70), component = ptr(frame.entity+0x628);
        source_require(ptr(collection) == frame.entity && ptr(component) == s.api.base+addr::engine::skater_component_vtable &&
            ptr(component+0x18) == collection && ptr(component+0x70) == frame.core &&
            ptr(frame.core) == s.api.base+addr::no_bail::bail_core_vtable && ptr(frame.core+0x3c0) == frame.context &&
            ptr(frame.core+0x438) == frame.rig && ptr(frame.rig) == frame.context && ptr(frame.rig+0x4630) == frame.core,
            "Probe physics owner changed");
        const auto board_link = ptr(frame.core+0x430), board = ptr(board_link+0x18), rig = ptr(frame.rig+0x2f10);
        source_require(ptr(board) == s.api.base+spawn::board_physics_vtable && ptr(rig) == s.api.base+spawn::rig_physics_vtable,
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
        if (!own_shape(s,frame) || !watched_chain(l)) return false;
        l.until = GetTickCount64()+100; l.active = true;
        auto& p = pulse_shared();
        AcquireSRWLockExclusive(&p.lock);
        if (!p.consumed.load(std::memory_order_acquire)) p.lease = l;
        ReleaseSRWLockExclusive(&p.lock);
        return true;
    } catch (...) { return false; }
}
void cancel_pulse() noexcept {
    auto& p = pulse_shared();
    AcquireSRWLockExclusive(&p.lock); p.lease.active = false; ReleaseSRWLockExclusive(&p.lock);
}
Json handle_json(const Handle& h) { return Json{{"world",h.world},{"index",h.index},{"generation",h.generation}}; }
void report(State& s) {
    if (s.root.empty()) return;
    auto& shared_pulse = pulse_shared();
    PulsePacket packet;
    PulseLease published;
    const char* last_rejection{};
    std::array<PulseObservation,8> observations{};
    std::size_t observation_count{};
    AcquireSRWLockShared(&shared_pulse.lock);
    packet = shared_pulse.packet; published = shared_pulse.lease; observations = shared_pulse.observations;
    observation_count = shared_pulse.observation_count;
    last_rejection = shared_pulse.last_rejection;
    ReleaseSRWLockShared(&shared_pulse.lock);
    s.pulsed = packet.force_submitted || packet.torque_submitted;
    if (packet.attempted) s.pulse_status = packet.status;
    Json::Array observed;
    for (std::size_t i = 0; i < std::min(observation_count,observations.size()); ++i) {
        const auto& o = observations[i];
        observed.emplace_back(Json{{"handle_address",o.handle_address},{"wrapper",o.wrapper},{"world",o.world},
            {"index",o.index},{"generation",o.generation},{"thread",o.thread},
            {"wrapper_match",o.wrapper_match},{"same_world",o.same_world}});
    }
    const Json pulse{{"requested",s.pulse_requested},{"hook_ready",shared_pulse.ready.load()},
        {"hook_rva",synchronizer.rva},{"calls",shared_pulse.calls.load()},{"lock_misses",shared_pulse.lock_misses.load()},
        {"lease_calls",shared_pulse.lease_calls.load()},{"expired",shared_pulse.expired.load()},
        {"wrapper_matches",shared_pulse.wrapper_matches.load()},{"world_matches",shared_pulse.world_matches.load()},
        {"owner_rejected",shared_pulse.owner_rejected.load()},{"clock_rejected",shared_pulse.clock_rejected.load()},
        {"last_rejection",last_rejection},
        {"observations",Json(observed)},{"observation_limit",observations.size()},
        {"attempted",packet.attempted},{"force_submitted",packet.force_submitted},{"torque_submitted",packet.torque_submitted},
        {"pre_valid",packet.pre_valid},{"post_valid",packet.post_valid},{"thread",packet.thread},
        {"handle_address",packet.handle_address},{"incoming_handle",handle_json(packet.incoming)},
        {"own_body",handle_json(packet.own_body)},{"wrapper",packet.wrapper},{"matched_part",packet.matched_part},
        {"wrapper_flags_after_original",packet.wrapper_flags_after_original},
        {"native_serial",packet.native_serial},{"elapsed_native_seconds",packet.elapsed},
        {"before_pending970",packet.before_linear_acceleration},{"before_pending978",packet.before_angular_acceleration},
        {"after_pending970",packet.after_linear_acceleration},{"after_pending978",packet.after_angular_acceleration},
        {"before_970_source",packet.before_linear_acceleration_source},{"before_978_source",packet.before_angular_acceleration_source},
        {"after_970_source",packet.after_linear_acceleration_source},{"after_978_source",packet.after_angular_acceleration_source},
        {"phase_interpretation","Exact observed local wrapper and same-world native producer call; experimental inference, not proof of a global worker barrier"},
        {"wrapper_hypothesis","SynchronizeBody arg2 equals one verified local native part+0x10; observed match required"},
        {"published_wrappers",published.wrappers},{"published_chain_watch_count",published.watch_count},
        {"published_client",published.client},{"published_entity",published.entity},{"published_core",published.core},
        {"published_rig",published.rig},{"published_client_context",published.client_context},
        {"published_skater_context",published.skater_context},{"published_native_serial",published.queued_serial},
        {"published_elapsed",published.queued_elapsed},{"published_until_wall_ms",published.until},
        {"published_expected_inverse_mass",published.configuration.inverse_mass},
        {"published_expected_inertia_ratio",published.configuration.inertia_ratio},
        {"published_expected_categories",published.configuration.categories},
        {"published_expected_collides_with",published.configuration.collides_with},
        {"lease_limit_ms",100},{"latest_native_seconds",pulse_latest_seconds}};
    const Json value{{"diagnostic",s.pulse_requested ? "independent native capsule drop and optional producer-phase pulse" : "independent native capsule drop"},
        {"not_a_riding_backend",true},{"completed",s.done},{"status",s.status},
        {"process_id",GetCurrentProcessId()},{"allocation_attempted",s.allocation_attempted},
        {"allocation_attempt_limit",1},{"body",handle_json(s.body)},{"shape",handle_json(s.shape)},
        {"world",s.world},{"native_state",s.native_state},{"client_context",s.client_context},
        {"defaults_verified",s.defaults_verified},
        {"default_inverse_mass",s.defaults.inverse_mass},{"default_inertia_ratio",s.defaults.inertia_ratio},
        {"source_variant",s.configuration.variant},{"configured_inverse_mass",s.configuration.inverse_mass},
        {"configured_mass",s.configuration.mass},{"configured_ratio",s.configuration.inertia_ratio},
        {"configured_inertia_diagonal",Json::array({
            1.0/(s.configuration.inverse_mass*s.configuration.inertia_ratio[0]),
            1.0/(s.configuration.inverse_mass*s.configuration.inertia_ratio[1]),
            1.0/(s.configuration.inverse_mass*s.configuration.inertia_ratio[2])})},
        {"capsule_a",Json::array({0,-.15,0})},{"capsule_b",Json::array({0,.15,0})},{"capsule_radius",.2},
        {"collision_categories",s.configuration.categories},{"collides_with",s.configuration.collides_with},
        {"collision_filter_source",s.configuration.categories == 0 ? "authored OnlyTerrainCollision" : "baseline all32 diagnostic masks"},
        {"floor",s.floor},{"initial_com",s.spawn},{"pulse_at_native_seconds",pulse_seconds},
        {"pulse_applied",s.pulsed},{"pulse_force",Json::array({5,0,0,0})},
        {"pulse_torque",Json::array({0,5,0,0})},
        {"pending970_units","linear acceleration accumulator; force-like single-native-step input"},
        {"pending978_units","angular acceleration accumulator; torque-like single-native-step input"},
        {"unit_interpretation","Run56 at observed60Hz: pending0.1 produced velocity delta0.001666667, consistent with pending times native dt, not an immediate impulse"},
        {"variable_dt_behavior_validated",false},{"impulse_conversion_implemented",false},
        {"prediction_at_60hz_identity_freefall",Json{{"dt",1.0/60},
            {"delta_velocity_x",5.0*s.configuration.inverse_mass/60},
            {"delta_angular_velocity_y",5.0*s.configuration.inverse_mass*s.configuration.inertia_ratio[1]/60}}},
        {"pulse_status",s.pulse_status},{"pulse",pulse},
        {"global_worker_barrier_proven",global_worker_barrier_proven},
        {"native_simulation_seconds",s.seeded ? s.last_time-s.start_time : 0},
        {"wall_elapsed_ms",GetTickCount64()-s.requested_at},{"wall_limit_ms",wall_limit_ms},
        {"samples",s.samples},{"sample_limit",sample_limit},{"skipped_snapshots",s.skipped},
        {"snapshot_validation","equal double reads of content and source pointers, final ring headers and owner checks; not an engine-wide atomic snapshot guarantee"},
        {"stopped_in_same_world",s.stopped},{"forgot_old_world",s.forgotten},{"cleanup_status",s.cleanup},
        {"allocation_cleanup_proven",false},{"whole_owned_process_teardown_required",true},
        {"cleanup_warning","No per-body destruction contract is established. Root must stop this isolated process after the report; disabling the capsule does not reclaim its allocation."}};
    const auto path = s.root / L"bmx-chassis-probe.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << value.dump(2) << '\n';
    if (!file) throw std::runtime_error("Cannot write isolated chassis probe report");
    s.report_at = GetTickCount64();
}
void add_sample(State& s, const NativeFrame& f, const Snapshot& body, const char* stage, double dt) {
    if (s.samples.size() >= sample_limit) return;
    Json::Array rings;
    for (const auto& ring : body.rings) rings.emplace_back(Json{{"values",ring.values},
        {"elements",ring.elements},{"current_plane",ring.current_plane},{"plane_count",ring.planes}});
    V4 pending_linear{},pending_angular{};
    std::uintptr_t pending_linear_source{},pending_angular_source{};
    const bool pending_valid = own_body(s,f) && pending_accelerations(s.native_state,s.body.index,
        pending_linear,pending_angular,pending_linear_source,pending_angular_source) && own_body(s,f);
    s.samples.emplace_back(Json{{"stage",stage},{"native_serial",f.serial},{"native_time",f.simulation_time},
        {"elapsed_native_seconds",s.seeded ? f.simulation_time-s.start_time : 0},{"dt",dt},
        {"native_frame_seconds",f.seconds},{"wall_ms",GetTickCount64()-s.requested_at},
        {"client",f.client},{"entity",f.entity},{"core",f.core},{"skater_context",f.context},
        {"client_context",s.client_context},{"rig",f.rig},
        {"native_state",body.native_state},{"position",body.position},{"quaternion_xyzw",body.quaternion_xyzw},
        {"linear_velocity",body.linear},{"angular_velocity",body.angular},
        {"inverse_mass",body.inverse_mass},{"inertia_ratio",body.inertia_ratio},
        {"pending_accelerations_valid",pending_valid},
        {"pending_linear_acceleration970",pending_valid ? Json(pending_linear) : Json{}},
        {"pending_angular_acceleration978",pending_valid ? Json(pending_angular) : Json{}},
        {"pending970_source",pending_valid ? Json(pending_linear_source) : Json{}},
        {"pending978_source",pending_valid ? Json(pending_angular_source) : Json{}},
        {"local_com",body.local_com},{"local_quaternion_wxyz",body.local_quaternion_wxyz},
        {"direct_array_sources_910_928_988_980",body.direct_arrays},
        {"rings",Json(rings)}});
}
void finish(State& s, const char* reason, const NativeFrame* frame = nullptr) {
    cancel_pulse(); // serialized with any in-flight diagnostic producer call
    s.status = reason;
    s.done = true;
    if (s.allocation_attempted) {
        s.cleanup = "unreclaimed allocation; terminate owned process";
        // A loading/replaced owner may make old pointers unsafe. Never reach
        // back into that old world, even to disable our former shape.
        if (frame && own_body(s, *frame)) {
            alignas(16) const V4 zero{};
            bool stopped = true;
            if (s.shape_observed) stopped = own_shape(s, *frame) &&
                mask(s.api.categories, &s.shape, 0) && own_shape(s, *frame) &&
                mask(s.api.collides, &s.shape, 0) && own_shape(s, *frame);
            stopped = stopped && own_body(s, *frame) && vector(s.api.linear, &s.body, zero.data()) &&
                own_body(s, *frame) && vector(s.api.angular, &s.body, zero.data()) &&
                own_body(s, *frame) && integer(s.api.motion, &s.body, 2) && own_body(s, *frame);
            s.stopped = stopped;
            if (stopped) s.cleanup = "own capsule disabled/keyframed; allocation unreclaimed; terminate owned process";
        } else s.forgotten = true;
    }
    report(s);
    logging::log(logging::Level::info, logging::Channel::skater,
        "BMX chassis diagnostic {}. Capsule allocation cleanup is unproven; stop the owned isolated process. Report: {}",
        reason, (s.root / L"bmx-chassis-probe.json").string());
}
bool seed(State& s, const NativeFrame& frame) {
    s.allocation_attempted = true; // no retry, including a partial failure
    if (!allocate(&s.api, s.world, &s.body)) return false;
    s.body_observed = true;
    if (!own_body(s, frame) || !snapshot(s, frame, s.defaults)) return false;
    if (s.defaults.inverse_mass != 1 || s.defaults.inertia_ratio != V4{1,1,1,0}) return false;
    s.defaults_verified = true;
    add_sample(s, frame, s.defaults, "allocated_defaults", 0);
    alignas(16) const V4 zero{};
    alignas(16) const V4 ratio = s.configuration.inertia_ratio;
    if (!integer(s.api.motion, &s.body, 2) || !own_body(s, frame) ||
        !scalar(s.api.inverse_mass, &s.body, s.configuration.inverse_mass) || !own_body(s, frame) ||
        !vector(s.api.inertia_ratio, &s.body, ratio.data()) || !own_body(s,frame) ||
        !configured_body(s.native_state,s.body.index,s.configuration) ||
        !touch(s.api.clear_aa0, &s.body) || !own_body(s, frame) ||
        !vector(s.api.com, &s.body, zero.data()) || !own_body(s, frame) ||
        !capsule(&s.api, s.world, &s.body, &s.shape)) return false;
    // Before clearing the owner the shape belongs to the exact handle just
    // returned by our capsule constructor, revalidated in the same client realm.
    // Native constructor mask defaults are not an ownership claim. Restrict
    // first configuration to the exact freshly returned, generation-validated
    // handle before adopting our zero/configured-mask ownership policy.
    if (!fresh_shape(s, frame) || !clear_owner(s.api.owner, &s.shape) || !fresh_shape(s, frame) ||
        !mask(s.api.categories, &s.shape, 0) || !fresh_shape(s, frame) ||
        !mask(s.api.collides, &s.shape, 0) || !fresh_shape(s, frame)) return false;
    s.shape_observed = true;
    if (!own_shape(s, frame)) return false;
    alignas(16) const std::array<float, 8> pose{0,0,0,1,s.spawn[0],s.spawn[1],s.spawn[2],1};
    // All collision stays disabled until this independent capsule is placed.
    if (!vector(s.api.linear, &s.body, zero.data()) || !own_shape(s, frame) ||
        !vector(s.api.angular, &s.body, zero.data()) || !own_shape(s, frame) ||
        !vector(s.api.transform, &s.body, pose.data()) || !own_shape(s, frame)) return false;
    Snapshot placed;
    if (!snapshot(s, frame, placed) || placed.inverse_mass != s.configuration.inverse_mass ||
        placed.inertia_ratio != s.configuration.inertia_ratio) return false;
    for (std::size_t i = 0; i < 3; ++i) if (std::abs(placed.position[i] - s.spawn[i]) > .01f) return false;
    if (!mask(s.api.categories, &s.shape, s.configuration.categories) || !own_shape(s, frame) ||
        !mask(s.api.collides, &s.shape, s.configuration.collides_with) || !own_shape(s, frame) ||
        !integer(s.api.motion, &s.body, 3) || !own_shape(s, frame) ||
        !touch(s.api.wake, &s.body) || !own_shape(s, frame)) return false;
    s.seeded = true; s.start_time = s.last_time = frame.simulation_time; s.serial = frame.serial;
    add_sample(s, frame, placed, "dynamic_seed", 0);
    return true;
}
} // namespace

bool request_chassis_probe() noexcept {
    LastError error;
    try {
        std::filesystem::path root;
        if (!opt_in(&root) || mounted() || !solo()) return false;
        auto& s = state();
        bool expected = false;
        if (!s.accepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
        s.root = std::move(root); s.requested_at = GetTickCount64(); s.status = "queued_waiting_for_native_owner";
        s.configuration = selected_configuration(); // fixed publication; worker never reads this environment
        s.pulse_requested = pulse_opt_in();
        if (s.pulse_requested) s.pulse_status = "waiting_for_supported_native_producer_hook";
        s.samples.reserve(sample_limit);
        s.queued.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}

void tick_chassis_probe(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept {
    LastError error;
    auto& s = state();
    if (!s.queued.load(std::memory_order_acquire) || s.done) return;
    try {
        std::filesystem::path root;
        if (!opt_in(&root) || root != s.root || !solo() || mounted()) { finish(s, "cancelled_gate_changed"); return; }
        if (GetTickCount64() - s.requested_at >= wall_limit_ms) { finish(s, "wall_timeout"); return; }
        if (!gameplay_ready) {
            if (s.allocation_attempted) finish(s, "cancelled_loading_world_forgotten");
            else if (GetTickCount64()-s.report_at >= 1000) report(s);
            return;
        }
        if (!s.api.base) {
            if (!resolve(base, s.api) || !enable_native_motion(base)) { finish(s, "unsupported_image_or_native_api"); return; }
            s.client_thread = GetCurrentThreadId();
            if (s.pulse_requested) {
                s.pulse_status = install_pulse_hook(s) ? "waiting_for_native_pulse_window" : "withheld_native_hook_unavailable";
            }
        }
        if (base != s.api.base || GetCurrentThreadId() != s.client_thread) { finish(s, "client_thread_or_image_changed"); return; }
        auto frame = read_native_frame(base, client);
        std::uintptr_t context{}, entity_context{};
        if (!frame.available || !frame.timing_available || !std::isfinite(frame.simulation_time) ||
            !std::isfinite(frame.seconds) || frame.seconds < 0 || frame.seconds > .1f ||
            !finite(frame.position) || !read(client+8,context) || !pointer(context) ||
            !read(frame.entity+0x20,entity_context) || entity_context != context ||
            !client_context(base, client, context)) {
            if (s.allocation_attempted) finish(s, "cancelled_native_owner_or_timing_unavailable");
            else if (GetTickCount64()-s.report_at >= 1000) report(s);
            return;
        }
        if (!s.allocation_attempted) {
            s.owner = frame;
            s.client_context = context;
            if (!world_of(s.api.world, context, &s.world) || !read(s.world+0x28, s.native_state) || !pointer(s.native_state)) return;
            // A lateral target three metres away, derived from the actual local
            // transform. The client ray excludes the local skater, and must
            // resolve exactly the world in which we will allocate.
            float x = frame.transform[0], z = frame.transform[2];
            const float length = std::hypot(x,z);
            if (!std::isfinite(length) || length < .5f || length > 1.5f) { finish(s, "invalid_native_right_basis"); return; }
            const std::array<float,3> from{frame.position[0]+3*x/length,frame.position[1]+2,frame.position[2]+3*z/length};
            const std::array<float,3> to{from[0],frame.position[1]-4,from[2]};
            const auto floor = probe_local_surface(from,to,{},frame.entity);
            if (!floor.available || !floor.hit || floor.context != context || floor.world != s.world || !finite(floor.position)) {
                s.status = "waiting_for_verified_client_floor";
                if (GetTickCount64()-s.report_at >= 1000) report(s);
                return;
            }
            const auto fresh = read_native_frame(base,client);
            if (!same_owner(frame,fresh) || !current_world(s,fresh)) { finish(s,"owner_changed_before_allocation"); return; }
            s.floor = floor.position; s.spawn = {floor.position[0],floor.position[1]+1.5f,floor.position[2]};
            if (!seed(s,fresh)) { finish(s,"native_seed_or_defaults_rejected",&fresh); return; }
            s.status = "observing_native_dynamic_capsule";
            report(s);
            return;
        }
        if (!own_shape(s,frame)) { finish(s,"ownership_or_world_changed_forgotten"); return; }
        if (frame.serial < s.serial || frame.simulation_time < s.last_time) { finish(s,"native_clock_reset",&frame); return; }
        if (frame.serial == s.serial || frame.simulation_time == s.last_time) return;
        const double dt = frame.simulation_time-s.last_time;
        if (!std::isfinite(dt) || dt > .1) { finish(s,"native_clock_discontinuity",&frame); return; }
        Snapshot body;
        if (!snapshot(s,frame,body)) {
            ++s.skipped;
            if (s.skipped > 120) finish(s,"native_snapshot_not_coherent",&frame);
            return;
        }
        double distance_squared{};
        for (std::size_t i = 0; i < 3; ++i) {
            const double delta = static_cast<double>(body.position[i])-s.spawn[i];
            distance_squared += delta*delta;
        }
        if (!std::isfinite(distance_squared) || distance_squared > 100 || body.position[1] < s.floor[1]-4) {
            finish(s,"bounded_motion_violation",&frame);
            return;
        }
        s.serial = frame.serial; s.last_time = frame.simulation_time;
        add_sample(s,frame,body,"observed",dt);
        const auto elapsed = s.last_time-s.start_time;
        if (s.pulse_requested && pulse_shared().ready.load(std::memory_order_acquire) &&
            !pulse_shared().consumed.load(std::memory_order_acquire) && elapsed >= pulse_seconds && elapsed <= pulse_latest_seconds) {
            s.pulse_status = publish_pulse(s,frame) ? "queued_waiting_exact_local_producer" : "withheld_fresh_owner_lease_unavailable";
        }
        if (!s.pulse_marked && elapsed > pulse_latest_seconds) {
            s.pulse_marked = true;
            cancel_pulse();
            if (s.pulse_requested && !pulse_shared().consumed.load(std::memory_order_acquire) &&
                pulse_shared().ready.load(std::memory_order_acquire)) s.pulse_status = "withheld_no_verified_producer_in_native_window";
            report(s);
        }
        if (s.last_time-s.start_time >= end_seconds) { finish(s,"completed_native_observation",&frame); return; }
        if (s.samples.size() >= sample_limit) { finish(s,"bounded_sample_limit",&frame); return; }
        if (GetTickCount64()-s.report_at >= 1000) report(s);
    } catch (...) {
        // On an unexpected C++ failure no old-world native calls are attempted.
        cancel_pulse();
        s.done = true; s.status = "exception_fail_closed";
        s.forgotten = s.allocation_attempted;
        s.cleanup = s.allocation_attempted ? "unreclaimed allocation; terminate owned process" : "not_allocated";
        try { report(s); } catch (...) {}
    }
}
} // namespace dingosdk::bmx
