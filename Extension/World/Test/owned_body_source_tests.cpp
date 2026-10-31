// Includes the actual owned_bodies.cpp translation unit. Wall time is substituted and
// named native ABI stand-ins read constructed arrays or return a counted query.
// The actual Service/Slot/Clock, capture/observe/query implementation,
// memory::peek and SRW locks execute below; no retail native code executes.
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace test_clock {
std::atomic<ULONGLONG> now{10000};
}
ULONGLONG WINAPI source_test_now() noexcept { return test_clock::now.load(); }
#define GetTickCount64 source_test_now
#include "../owned_bodies.cpp"
#undef GetTickCount64

// A genuine compiler-created static TLS block, reached through the same GS:58
// module TLS vector as client_context(). No TEB/TLS vector is overwritten.
extern "C" unsigned long _tls_index;
__declspec(align(16)) __declspec(thread) unsigned char source_test_tls[0x4000];

namespace test_facts {
std::atomic<unsigned> model_calls{}, world_calls{}, body_calls{}, shape_calls{}, mutations{}, forbidden_links{};
std::atomic<bool> multiplayer_active{}, inspect_initializer_lock{}, initializer_unlocked{};
std::atomic<ULONGLONG> initializer_elapsed{};
std::function<dingosdk::LocalSurfaceProbe(const std::array<float, 3>&, const std::array<float, 3>&, const dingosdk::LocalSurfaceQuery&)>
    probe_callback;
std::vector<dingosdk::body_query::QueryPreset> probe_presets; // What the Service forwarded, per counted query.
}
namespace dingosdk::multiplayer {
MultiplayerModel model() {
    ++test_facts::model_calls;
    MultiplayerModel out;
    out.active = test_facts::multiplayer_active.load();
    return out;
}
}

// Unrelated create/hook links fail closed and are counted. Only the named
// contact-query cases install the counted, constructed native-query ABI stand-in.
namespace dingosdk {
HookStatus WINAPI hook_initialize() { ++test_facts::forbidden_links; return HookNotInitialized; }
HookStatus WINAPI hook_prepare(void*, void*, void**) { ++test_facts::forbidden_links; return HookNotInitialized; }
HookStatus WINAPI hook_enable(void*) { ++test_facts::forbidden_links; return HookNotInitialized; }
HookBatch::HookBatch() noexcept { ++test_facts::forbidden_links; }
HookBatch::~HookBatch() { ++test_facts::forbidden_links; }
bool start_no_bail(std::uintptr_t) noexcept { ++test_facts::forbidden_links; return false; }
bool add_no_bail_step_listener(NoBailStepObserver) noexcept { ++test_facts::forbidden_links; return false; }
LocalSurfaceProbe probe_local_surface_counted(const std::array<float, 3>& from, const std::array<float, 3>& to,
                                              const LocalSurfaceQuery& query) {
    test_facts::probe_presets.push_back(query.preset);
    if (test_facts::probe_callback) return test_facts::probe_callback(from, to, query);
    ++test_facts::forbidden_links;
    return {};
}
namespace supported_build {
bool running_image_matches(std::uintptr_t) noexcept {
    ++test_facts::forbidden_links;
    if (test_facts::inspect_initializer_lock.load()) {
        const bool unlocked = TryAcquireSRWLockExclusive(&owned_bodies::service.lock) != 0;
        test_facts::initializer_unlocked = unlocked;
        if (unlocked) ReleaseSRWLockExclusive(&owned_bodies::service.lock);
        test_clock::now += test_facts::initializer_elapsed.load();
    }
    return false;
}
}
}

namespace dingosdk::owned_bodies::source_test {
unsigned checks{}, cases{};
void check(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
constexpr std::uint64_t owner = 101;
constexpr std::uintptr_t test_world_field = 0x80; // synthetic ABI stand-in only
constexpr std::uint32_t capacity = 8, body_index = 3, body_generation = 7;
constexpr std::uint32_t shape_index = 5, shape_generation = 11;

class Arena {
public:
    Arena() {
        memory_ = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory_) throw std::runtime_error("synthetic arena allocation failed");
    }
    ~Arena() { VirtualFree(memory_, 0, MEM_RELEASE); }
    Arena(const Arena&) = delete;
    std::uintptr_t take(std::size_t size, std::size_t alignment = 64) {
        used_ = (used_ + alignment - 1) & ~(alignment - 1);
        if (size > bytes - used_) throw std::runtime_error("bounded arena exhausted");
        const auto result = reinterpret_cast<std::uintptr_t>(memory_) + used_;
        used_ += size;
        return result;
    }
private:
    static constexpr std::size_t bytes = 1 << 20;
    void* memory_{};
    std::size_t used_{};
};
template<class T> void put(std::uintptr_t at, const T& value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof(value));
}
template<class T> T load(std::uintptr_t at) {
    T value{};
    check(peek(at, value), "test inspection read failed");
    return value;
}
void install_tls(std::uintptr_t game_context) {
    const auto table = static_cast<std::uintptr_t>(__readgsqword(0x58));
    check(table && _tls_index <= 4095, "real static TLS vector/index absent");
    const auto block = load<std::uintptr_t>(table + _tls_index * sizeof(std::uintptr_t));
    const auto begin = reinterpret_cast<std::uintptr_t>(source_test_tls);
    const auto end = begin + sizeof(source_test_tls);
    check(block + r::tls_context >= begin && block + r::tls_context + sizeof(std::uintptr_t) <= end &&
        block + r::tls_active >= begin && block + r::tls_active + 1 <= end,
        "TLS fixture fields would touch another module/CRT TLS object");
    put(block + r::tls_context, game_context);
    put(block + r::tls_active, std::uint8_t{1});
}

std::uintptr_t world_read(std::uintptr_t context) {
    ++test_facts::world_calls;
    SetLastError(0xB011);
    std::uintptr_t result{};
    return peek(context + test_world_field, result) ? result : 0;
}
// Native validators are read-only ABI stand-ins over constructed native arrays,
// not a precomputed answer for capture(), fresh() or pool_current().
bool body_valid_read(const NativeHandle* handle) {
    ++test_facts::body_calls;
    SetLastError(0xB012);
    std::uintptr_t state{}, generations{}, bits{};
    std::uint32_t limit{}, allocation{}, generation{};
    return handle && peek(handle->world + pb::store_offset, state) &&
        peek(state + r::body_capacity, limit) && limit && limit <= capacity && handle->index < limit &&
        peek(state + r::allocated_bits, bits) && peek(bits, allocation) &&
        (allocation & (1U << handle->index)) && peek(handle->world + r::world_body_generations, generations) &&
        peek(generations + handle->index * sizeof(std::uint32_t), generation) && generation == handle->generation;
}
bool shape_valid_read(const NativeHandle* handle) {
    ++test_facts::shape_calls;
    SetLastError(0xB013);
    std::uintptr_t state{}, generations{}, bits{};
    std::uint32_t allocation{};
    std::uint8_t generation{};
    return handle && handle->index < capacity && peek(handle->world + pb::store_offset, state) &&
        peek(state + r::shape_allocated_bits, bits) && peek(bits, allocation) &&
        (allocation & (1U << handle->index)) && peek(state + r::shape_generations, generations) &&
        peek(generations + handle->index, generation) && generation == handle->generation;
}
void forbidden_vector(NativeHandle*, const float*) { ++test_facts::mutations; }
void forbidden_integer(NativeHandle*, int) { ++test_facts::mutations; }
void forbidden_scalar(NativeHandle*, float) { ++test_facts::mutations; }
void forbidden_touch(NativeHandle*) { ++test_facts::mutations; }
void forbidden_mask(NativeHandle*, const std::uint32_t*) { ++test_facts::mutations; }
void forbidden_owner(NativeHandle*, const NativeHandle*) { ++test_facts::mutations; }
NativeHandle* forbidden_allocate(std::uintptr_t, NativeHandle*) { ++test_facts::mutations; return nullptr; }
NativeHandle* forbidden_capsule(std::uintptr_t, NativeHandle*, const NativeHandle*, const float*, const float*, float) {
    ++test_facts::mutations;
    return nullptr;
}

class Fixture {
public:
    Arena arena;
    std::uintptr_t base{}, client{}, context{}, entity{}, core{}, skater_context{}, rig{}, world{}, state{};
    std::uintptr_t position{}, rotation{}, velocity{}, spin{}, body_generations{}, allocations{}, dynamics{}, categories{}, collisions{};
    Handle handle{17, 0};
    Fixture() {
        test_clock::now = 10000;
        test_facts::model_calls = 0;
        test_facts::world_calls = 0;
        test_facts::body_calls = 0;
        test_facts::shape_calls = 0;
        test_facts::mutations = 0;
        test_facts::multiplayer_active = false;
        test_facts::probe_callback = {};
        // Only reserve address space up to the largest used RVA; commit two
        // data pages. No retail code/data is copied or executed.
        image_ = VirtualAlloc(nullptr, e::tls_index + 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        if (!image_) throw std::runtime_error("synthetic image reservation failed");
        base = reinterpret_cast<std::uintptr_t>(image_);
        for (const auto offset : {e::context_player_manager_offset, e::tls_index}) {
            const auto page = (base + offset) & ~std::uintptr_t{4095};
            check(VirtualAlloc(reinterpret_cast<void*>(page), 4096, MEM_COMMIT, PAGE_READWRITE) != nullptr,
                "sparse image data commit failed");
        }
        put(base + e::context_player_manager_offset, std::uint32_t{0x100});
        put(base + e::tls_index, static_cast<std::uint32_t>(_tls_index));
        client = arena.take(0x1000);
        context = arena.take(0x2000);
        const auto manager = arena.take(0x1000), list = arena.take(64), player = arena.take(0x1000);
        entity = arena.take(0x1000);
        const auto player_handle = arena.take(64);
        const auto collection = arena.take(64), component = arena.take(0x1000);
        core = arena.take(0x1000);
        skater_context = arena.take(0x2000);
        rig = arena.take(0x6000);
        const auto holder = arena.take(64), board = arena.take(0x1000), rig_physics = arena.take(0x1000);
        const auto board_parts = arena.take(0x1000), rig_parts = arena.take(0x3000);
        world = arena.take(0x1000);
        state = arena.take(0x2000);
        put(client, base + e::client_vtable);
        put(client + r::client_context, context);
        put(client + r::client_phase, std::uint32_t{13});
        put(context + 0x100, manager);
        put(manager, base + e::local_player_manager_vtable);
        put(manager + r::player_begin, list);
        put(manager + r::player_end, list + sizeof(std::uintptr_t));
        put(list, player);
        put(player, base + e::local_player_vtable);
        put(player + r::player_context, context);
        put(player + r::player_local, std::uint8_t{1});
        put(player + r::player_remote, std::uint8_t{0});
        put(player + r::player_entity, entity);
        put(player + r::player_handle, player_handle);
        put(player_handle, entity + r::entity_handle_target);
        put(entity, base + e::skater_entity_vtable);
        put(entity + r::entity_context, context);
        put(entity + r::entity_player, player);
        put(entity + r::entity_collection, collection);
        put(entity + r::entity_component, component);
        put(collection, entity);
        put(component, base + e::skater_component_vtable);
        put(component + r::component_collection, collection);
        put(component + r::component_core, core);
        put(core, base + addr::no_bail::bail_core_vtable);
        put(core + r::core_context, skater_context);
        put(core + r::core_rig, rig);
        put(rig, skater_context);
        put(rig + r::rig_core, core);
        put(core + k::board_holder_offset, holder);
        put(holder + k::board_physics_offset, board);
        put(rig + k::rig_physics_offset, rig_physics);
        put(board, base + k::board_physics_vtable);
        put(rig_physics, base + k::rig_physics_vtable);
        put(board + k::physics_bodies_offset, board_parts);
        put(rig_physics + k::physics_bodies_offset, rig_parts);
        put(board_parts, k::board_body_count);
        put(rig_parts, k::rig_body_count);
        for (std::size_t i = 0; i < wrapper_count; ++i) {
            const bool on_board = i < k::board_body_count;
            const auto part = on_board ? board_parts + i * k::physics_body_size :
                rig_parts + (i - k::board_body_count + first_rig_wrapper) * k::physics_body_size;
            put(part + k::body_owner_offset, on_board ? board : rig_physics);
        }
        put(context + test_world_field, world);
        put(world + pb::store_offset, state);
        put(skater_context + r::context_seconds, 1.f / 60.f);
        install_tls(context);
        setup_pool();
        InitializeSRWLock(&service.lock);
        InitializeSRWLock(&clock_feed.lock);
        service.slots = {};
        service.api = {};
        service.thread = GetCurrentThreadId();
        service.base = base;
        service.client = client;
        service.world = world;
        service.state = state;
        service.context = context;
        service.generation = handle.generation;
        service.world_generation = 23;
        service.eligible = true;
        service.tick_at = test_clock::now;
        service.hooked = true;
        service.api.base = base;
        service.api.world = world_read;
        clock_world.store(world_read, std::memory_order_release);
        service.api.body_valid = body_valid_read;
        service.api.shape_valid = shape_valid_read;
        service.api.allocate = forbidden_allocate;
        service.api.capsule = forbidden_capsule;
        service.api.motion = forbidden_integer;
        service.api.inverse_mass = forbidden_scalar;
        service.api.clear = forbidden_touch;
        service.api.wake = forbidden_touch;
        service.api.categories = forbidden_mask;
        service.api.collides = forbidden_mask;
        service.api.owner = forbidden_owner;
        service.api.com = service.api.transform = service.api.linear = service.api.angular = service.api.force =
            service.api.torque = service.api.inertia = service.api.clear_force = service.api.clear_torque = forbidden_vector;
        Anchor actual;
        check(capture(actual), "actual full capture failed in seeded native/TLS fixture");
        auto& s = slot();
        s.owner = owner;
        s.handle = handle;
        s.anchor = actual;
        s.definition.mass = 12;
        s.definition.inertia = {2, 3, 4};
        s.definition.count = 1;
        s.definition.capsules[0] = {{0, 0, 0}, {0, 1, 0}, .2f};
        s.definition.rotation = {0, 0, 0, 1};
        s.body = {world, body_index, body_generation};
        s.shapes[0] = {world, shape_index, shape_generation};
        s.made = 1;
        s.attempted = s.body_observed = s.active = true;
        s.latest.mass = 999;
        s.latest.position = {-13, -0.f, 42};
        s.latest.epoch = {23, handle.generation, 77, 9, 1.f / 60.f};
        s.mailbox.count = 1;
        s.mailbox.sequence = 44;
        s.mailbox.receipts[0].outcome = Outcome::applied;
        s.mailbox.receipts[0].sequence = 44;
        s.mailbox.receipts[0].produced = {23, handle.generation, 0, 0, 1.f / 60.f};
        clock_feed.sequence = 0;
        clock_feed.serial = clock_feed.observed = 0;
        clock_feed.time = 0;
        clock_feed.owner = {};
        clock_feed.game_context = clock_feed.world = clock_feed.state = 0;
        step();
        check(pool_current(s, true) && producer_owner(s, s.body), "actual pool qualification failed in fixture");
    }
    ~Fixture() {
        service.hooked = false;
        service.eligible = false;
        service.slots = {};
        VirtualFree(image_, 0, MEM_RELEASE);
    }
    Slot& slot() { return service.slots[0]; }
    void step() { physics_step({base, entity, core, skater_context, rig}, 1.f / 60.f, false); }
private:
    void* image_{};
    void setup_pool() {
        position = arena.take(4096, 4096);
        rotation = arena.take(4096, 4096);
        velocity = arena.take(4096, 4096);
        spin = arena.take(4096, 4096);
        put(state + r::position_ring, Ring{position, capacity, 0, 1});
        put(state + r::rotation_ring, Ring{rotation, capacity, 0, 1});
        put(state + r::velocity_ring, Ring{velocity, capacity, 0, 1});
        put(state + r::spin_ring, Ring{spin, capacity, 0, 1});
        put(position + body_index * sizeof(V4), V4{12.5f, -0.f, 7, 0});
        put(rotation + body_index * sizeof(V4), V4{1, 0, 0, 0});
        put(velocity + body_index * sizeof(V4), V4{2, -0.f, 3, 0});
        put(spin + body_index * sizeof(V4), V4{});
        const auto com = arena.take(capacity * sizeof(V4)), local = arena.take(capacity * sizeof(V4));
        const auto ratio = arena.take(capacity * sizeof(V4)), inverse = arena.take(capacity * sizeof(float));
        put(state + r::local_com, com);
        put(state + r::local_rotation, local);
        put(state + r::inertia_ratio_array, ratio);
        put(state + r::inverse_mass_array, inverse);
        put(local + body_index * sizeof(V4), V4{1, 0, 0, 0});
        put(ratio + body_index * sizeof(V4), V4{6, 4, 3, 0});
        put(inverse + body_index * sizeof(float), 1.f / 12.f);
        body_generations = arena.take(capacity * sizeof(std::uint32_t));
        allocations = arena.take(64);
        dynamics = arena.take(64);
        put(world + r::world_body_generations, body_generations);
        put(body_generations + body_index * sizeof(std::uint32_t), body_generation);
        put(state + r::allocated_bits, allocations);
        put(allocations, 1U << body_index);
        put(state + r::dynamic_bits, dynamics);
        put(dynamics, 1U << body_index);
        const auto shape_bits = arena.take(64), shape_generations = arena.take(64), shape_owners = arena.take(capacity * 4);
        categories = arena.take(capacity * 4);
        collisions = arena.take(capacity * 4);
        put(state + r::shape_allocated_bits, shape_bits);
        put(shape_bits, 1U << shape_index);
        put(state + r::shape_generations, shape_generations);
        put(shape_generations + shape_index, std::uint8_t{shape_generation});
        put(state + pb::body_owner_offset, shape_owners);
        put(shape_owners + shape_index * 4, r::unassigned_shape_owner);
        put(state + r::shape_categories, categories);
        put(categories + shape_index * 4, r::safe_categories);
        put(state + r::shape_collides, collisions);
        put(collisions + shape_index * 4, r::safe_world_collisions);
    }
};

// These cases execute actual probe_batch/Service/Slot/Clock and body-hit
// qualification over constructed native arrays. The one external native query
// is a counted ABI stand-in; it does not prove retail ray safety or consumption.
class ContactFixture : public Fixture {
public:
    static constexpr std::uint32_t foreign_index = 1, foreign_generation = 9;
    static constexpr std::array<std::uint32_t, 2> hit_shapes{2, 3};
    static constexpr std::array<std::uint8_t, 2> hit_generations{13, 14};
    unsigned calls{};
    bool normals = true, hit_owned{}, hit_unknown{};
    std::function<void(unsigned)> during_query;
    std::vector<bool> contact_options;
    std::uintptr_t shape_bodies{};
    ContactFixture() {
        put(allocations, (1U << body_index) | (1U << foreign_index));
        put(body_generations + foreign_index * 4, foreign_generation);
        put(position + foreign_index * sizeof(V4), V4{0, 0, 0, 0});
        put(velocity + foreign_index * sizeof(V4), V4{2, 0, 0, 0});
        put(spin + foreign_index * sizeof(V4), V4{0, 1, 0, 0});
        const auto bits = load<std::uintptr_t>(state + r::shape_allocated_bits);
        const auto generations = load<std::uintptr_t>(state + r::shape_generations);
        put(bits, (1U << shape_index) | (1U << hit_shapes[0]) | (1U << hit_shapes[1]));
        shape_bodies = arena.take(capacity * 4);
        put(state + r::shape_body_indices, shape_bodies);
        put(shape_bodies + shape_index * 4, body_index);
        for (std::size_t i = 0; i < hit_shapes.size(); ++i) {
            put(generations + hit_shapes[i], hit_generations[i]);
            put(shape_bodies + hit_shapes[i] * 4, foreign_index);
        }
        test_facts::probe_callback = [&](const auto& from, const auto& to, const LocalSurfaceQuery& query) {
            ++calls;
            const bool contact = query.measure_contact;
            contact_options.push_back(contact);
            check(query.read_shape, "the Service did not ask for the hit's shape");
            if (during_query) during_query(calls);
            LocalSurfaceProbe hit;
            hit.native_casts = 1;
            hit.available = true;
            hit.hit = true;
            hit.context = context;
            hit.world = world;
            hit.fraction = .5f;
            for (std::size_t i = 0; i < from.size(); ++i) hit.position[i] = (from[i] + to[i]) * .5f;
            const auto ordinal = (calls - 1) % hit_shapes.size();
            NativeHandle shape{world, hit_owned ? shape_index : hit_shapes[ordinal],
                hit_owned ? shape_generation : hit_generations[ordinal]};
            if (hit_unknown) shape.generation++;
            std::memcpy(hit.hit_shape.data(), &shape, sizeof(shape));
            hit.normal = {0, 1, 0};
            hit.normal_available = contact && normals;
            if (hit.normal_available) hit.position[2] += .01f;
            return hit;
        };
    }
    ~ContactFixture() { test_facts::probe_callback = {}; }
    body_query::Request request(std::size_t rays = 1, bool contact = true) {
        body_query::Request out;
        out.count = rays;
        out.require_contact = contact;
        check(read(owner, handle, out.observation).empty(), "Service test could not publish measured canonical sample");
        for (std::size_t i = 0; i < rays; ++i) out.rays[i] = {{1, 2, 0}, {1, -2, 0}};
        return out;
    }
};

void service_contact_opt_in_and_read_only() {
    ContactFixture f;
    auto request = f.request(1, false);
    std::array<unsigned char, sizeof(Mailbox)> mailbox{};
    std::memcpy(mailbox.data(), &f.slot().mailbox, mailbox.size());
    const auto latest = f.slot().latest;
    SetLastError(0x4321);
    auto out = probe_batch(owner, f.handle, request, 64);
    check(GetLastError() == 0x4321, "Service contact changed LastError");
    check(out.available && out.native_casts == 1 && !out.rays[0].normal_available &&
        !out.rays[0].surface_velocity_available && !f.contact_options.back(), "legacy Service geometry collected contact");
    request.require_contact = true;
    out = probe_batch(owner, f.handle, request, 64);
    check(out.available && out.native_casts == 1 && out.rays[0].normal_available && f.contact_options.back(),
        "actual Service did not join required contact");
    check(out.rays[0].position == Vec{1, 0, .01f} && out.rays[0].surface_velocity_available &&
        out.rays[0].surface_velocity == Vec{2.01f, 0, -1},
        "actual Service fabricated static motion or lost cross sign");
    check((load<std::uint32_t>(f.dynamics) & (1U << f.foreign_index)) == 0,
        "moving foreign witness unexpectedly depended on a dynamic bit");
    f.normals = false;
    out = probe_batch(owner, f.handle, request, 64);
    check(!out.available && out.failure == body_query::Failure::contact_measurement_unavailable && out.native_casts == 1,
        "required unknown normal became support");
    f.normals = true;
    put(f.velocity + f.foreign_index * sizeof(V4), V4{NAN, 0, 0, 0});
    out = probe_batch(owner, f.handle, request, 64);
    check(!out.available && out.failure == body_query::Failure::contact_measurement_unavailable && out.native_casts == 1,
        "required malformed foreign motion became support");
    request.require_contact = false;
    out = probe_batch(owner, f.handle, request, 64);
    check(out.available && !out.rays[0].surface_velocity_available && !f.contact_options.back(),
        "legacy geometry gained a foreign motion read/fence dependency");
    check(body_query::same_sample(latest, f.slot().latest) &&
        std::memcmp(mailbox.data(), &f.slot().mailbox, mailbox.size()) == 0, "contact query published a pose/command/receipt");
}

void service_whole_batch_contact_fences() {
    for (unsigned variant = 0; variant < 3; ++variant) {
        ContactFixture f;
        auto request = f.request(2);
        f.during_query = [&](unsigned calls) {
            if (calls != 2) return;
            if (variant == 0) put(f.velocity + f.foreign_index * sizeof(V4), V4{3, 0, 0, 0});
            if (variant == 1) put(f.shape_bodies + f.hit_shapes[0] * 4, body_index);
            if (variant == 2) f.step();
        };
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(f.calls == 2 && out.native_casts == 2, "contact fence lost actual two-cast accounting");
        check(!out.available && out.failure == body_query::Failure::observation_changed,
            "Service accepted changed foreign motion/individual shape/canonical native clock");
        check(out.rays[0].status == body_query::Status::unavailable && !out.rays[0].normal_available &&
            !out.rays[1].surface_velocity_available, "whole-batch refusal leaked partial contacts");
    }
}

void service_actor_and_nearest_hit_refusals() {
    ContactFixture f;
    auto request = f.request();
    request.exclude_local_actor = true;
    auto out = probe_batch(owner, f.handle, request, 64);
    check(!out.available && out.failure == body_query::Failure::local_actor_exclusion_unavailable &&
        out.native_casts == 0 && f.calls == 0, "actor exclusion was bypassed or reached a native cast");
    request.exclude_local_actor = false;
    f.hit_owned = true;
    out = probe_batch(owner, f.handle, request, 64);
    check(!out.available && out.native_casts == 1 && f.calls == 1, "owned nearest hit became clear or was recast through");
    f.hit_owned = false;
    f.hit_unknown = true;
    out = probe_batch(owner, f.handle, request, 64);
    check(!out.available && out.native_casts == 1 && f.calls == 2, "unknown nearest hit became foreign/clear or retried");
}

// The actual Service forwards the preset to its one counted geometry query and
// changes nothing else: the same measured-contact, owned-hit, actor and fence rules.
void service_forwards_query_preset_without_relaxing_refusals() {
    using body_query::QueryPreset;
    test_facts::probe_presets.clear();
    {
        ContactFixture f;
        auto request = f.request();
        auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && f.calls == 1 && test_facts::probe_presets == std::vector<QueryPreset>{QueryPreset::camera},
            "default Service query did not send the camera preset");
        request.preset = QueryPreset::terrain;
        SetLastError(0x5150);
        out = probe_batch(owner, f.handle, request, 64);
        check(GetLastError() == 0x5150, "terrain Service query changed LastError");
        check(out.available && out.native_casts == 1 && f.calls == 2 && test_facts::probe_presets.back() == QueryPreset::terrain &&
            f.contact_options.back(), "Service dropped the terrain preset or its contact request");
        check(out.rays[0].normal_available && out.rays[0].normal == Vec{0, 1, 0} && out.rays[0].surface_velocity_available &&
            out.rays[0].surface_velocity == Vec{2.01f, 0, -1}, "terrain lost the measured normal or the moving surface's point motion");
        f.normals = false;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == body_query::Failure::contact_measurement_unavailable && out.native_casts == 1 && f.calls == 3,
            "terrain admitted a required contact without a measured normal");
        f.normals = true;
        for (const auto preset : {QueryPreset::camera, QueryPreset::terrain}) {
            request.preset = preset;
            request.exclude_local_actor = true;
            const auto calls = f.calls;
            out = probe_batch(owner, f.handle, request, 64);
            check(!out.available && out.failure == body_query::Failure::local_actor_exclusion_unavailable && !out.native_casts && f.calls == calls,
                "a preset bypassed the zero-cast actor exclusion refusal");
        }
        request.exclude_local_actor = false;
        request.preset = QueryPreset::terrain;
        f.hit_owned = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.native_casts == 1 && f.calls == 4 && test_facts::probe_presets.back() == QueryPreset::terrain,
            "terrain owned nearest hit became clear or was recast through");
        f.hit_owned = false;
        for (const auto unknown : {static_cast<QueryPreset>(3), static_cast<QueryPreset>(255)}) {
            request.preset = unknown;
            const auto presets = test_facts::probe_presets.size();
            out = probe_batch(owner, f.handle, request, 64);
            check(!out.available && out.failure == body_query::Failure::invalid && !out.native_casts && f.calls == 4 &&
                test_facts::probe_presets.size() == presets, "an unknown preset reached the counted geometry query");
        }
    }
    ContactFixture f;
    auto request = f.request(2);
    request.preset = QueryPreset::terrain;
    f.during_query = [&](unsigned calls) { if (calls == 2) f.step(); };
    const auto out = probe_batch(owner, f.handle, request, 64);
    check(f.calls == 2 && out.native_casts == 2 && !out.available && out.failure == body_query::Failure::observation_changed &&
        out.rays[0].status == body_query::Status::unavailable && !out.rays[0].normal_available,
        "terrain batch escaped the closing whole-batch fence");
}

// Optional shape categories over the actual Service: opt-in only, read after the
// actual hit qualified and was not owned, keyed by the hit shape rather than its
// mapped body, gated by the pinned getter bytes, and fenced before publication.
void service_shape_categories_are_opt_in_shape_bound_and_fenced() {
    using body_query::Failure;
    const auto pin = [](ContactFixture& f, bool exact) {
        // Commit only the synthetic image page that holds the pinned getter bytes.
        const auto at = f.base + r::shape_in_categories.rva;
        check(VirtualAlloc(reinterpret_cast<void*>(at & ~std::uintptr_t{4095}), 4096, MEM_COMMIT, PAGE_READWRITE) != nullptr,
            "getter byte page commit failed");
        auto bytes = r::shape_in_categories.bytes;
        if (!exact) bytes[31] = static_cast<unsigned char>(bytes[31] ^ 0x01U);
        put(at, bytes);
    };
    const auto masks = [](ContactFixture& f) {
        put(f.categories + ContactFixture::hit_shapes[0] * 4, 0x80000000U);
        put(f.categories + ContactFixture::hit_shapes[1] * 4, 0xffffffffU);
        // The mapped foreign body's own index holds a decoy for a body-index read.
        put(f.categories + ContactFixture::foreign_index * 4, 7U);
    };
    const auto none = [](const body_query::Result& out) {
        for (const auto& hit : out.rays) if (hit.in_categories_available) return false;
        return true;
    };
    {
        ContactFixture f;
        masks(f);
        auto request = f.request(2, false);
        request.include_shape_categories = true;
        auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && none(out), "unreadable getter bytes refused the batch or still read categories");
        pin(f, true);
        request.include_shape_categories = false;
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && none(out), "a default Service batch carried categories");
        request.include_shape_categories = true;
        SetLastError(0x5157);
        out = probe_batch(owner, f.handle, request, 64);
        check(GetLastError() == 0x5157, "category capture changed LastError");
        check(out.available && out.native_casts == 2 && f.calls == 6 &&
            out.rays[0].in_categories_available && out.rays[0].in_categories == 0x80000000U &&
            out.rays[1].in_categories_available && out.rays[1].in_categories == 0xffffffffU,
            "the Service lost, mixed or body-indexed its hit shape categories");
        check(out.rays[0].status == body_query::Status::hit && !out.rays[0].owner && !out.rays[0].owner_available,
            "categories minted a hit owner");
        pin(f, false);
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && none(out), "a getter byte mismatch refused the batch or still read categories");
        pin(f, true);
        request.exclude_local_actor = true;
        const auto calls = f.calls;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::local_actor_exclusion_unavailable && !out.native_casts && f.calls == calls && none(out),
            "the option bypassed the zero-cast actor exclusion refusal");
        request.exclude_local_actor = false;
        f.hit_owned = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.native_casts == 1 && none(out), "an owned nearest hit was given a category or recast");
    }
    {
        // Category-only drift while the second ray runs drops only the first value.
        ContactFixture f;
        masks(f);
        pin(f, true);
        auto request = f.request(2, false);
        request.include_shape_categories = true;
        const auto second = f.calls + 2;
        f.during_query = [&, second](unsigned calls) { if (calls == second) put(f.categories + ContactFixture::hit_shapes[0] * 4, 1U); };
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && !out.rays[0].in_categories_available &&
            out.rays[1].in_categories_available && out.rays[1].in_categories == 0xffffffffU,
            "category drift refused the batch or kept a stale value");
    }
    {
        // An identical copy at another address is still a replaced array.
        ContactFixture f;
        masks(f);
        pin(f, true);
        auto request = f.request(2, false);
        request.include_shape_categories = true;
        const auto copy = f.arena.take(capacity * 4);
        for (std::uint32_t i = 0; i < capacity; ++i) put(copy + i * 4, load<std::uint32_t>(f.categories + i * 4));
        const auto second = f.calls + 2;
        f.during_query = [&, second, copy](unsigned calls) { if (calls == second) put(f.state + r::shape_categories, copy); };
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && !out.rays[0].in_categories_available && out.rays[1].in_categories_available,
            "a replaced category array kept the old witness or refused the batch");
    }
    {
        // Identity drift is still a whole-batch refusal, with no category at all.
        ContactFixture f;
        masks(f);
        pin(f, true);
        auto request = f.request(2, false);
        request.include_shape_categories = true;
        const auto second = f.calls + 2;
        f.during_query = [&, second](unsigned calls) { if (calls == second) put(f.shape_bodies + ContactFixture::hit_shapes[0] * 4, body_index); };
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::observation_changed && out.native_casts == 2 && none(out),
            "shape identity drift became a missing category instead of a refusal");
    }
}

// The rest of the batch options over the actual Service. The named-actor-category
// requirement is refused with zero casts unless the preset's filter and the pinned
// getter qualify, is judged after the closing fence over the surviving values, and
// only then sets its fact; local_actor_excluded is never set. Each refused nearest
// hit names its reason, and hit owner slots follow the category rules.
void service_named_actor_categories_reasons_and_hit_owners() {
    using body_query::Failure;
    using body_query::QueryPreset;
    constexpr std::uint32_t static_template = 0x2F90U, terrain_template = 0xAD1U;
    const auto pin = [](ContactFixture& f, bool exact) {
        const auto at = f.base + r::shape_in_categories.rva;
        check(VirtualAlloc(reinterpret_cast<void*>(at & ~std::uintptr_t{4095}), 4096, MEM_COMMIT, PAGE_READWRITE) != nullptr,
            "getter byte page commit failed");
        auto bytes = r::shape_in_categories.bytes;
        if (!exact) bytes[0] = static_cast<unsigned char>(bytes[0] ^ 0x01U);
        put(at, bytes);
    };
    const auto masks = [](ContactFixture& f, std::uint32_t first, std::uint32_t second) {
        put(f.categories + ContactFixture::hit_shapes[0] * 4, first);
        put(f.categories + ContactFixture::hit_shapes[1] * 4, second);
    };
    // No value and no fact survives.
    const auto none = [](const body_query::Result& out) {
        for (const auto& hit : out.rays) if (hit.in_categories_available || hit.owner_available) return false;
        return !out.local_actor_excluded && !out.named_actor_categories_absent;
    };
    const auto required = [](ContactFixture& f, bool contact) {
        auto out = f.request(2, contact);
        out.require_named_actor_categories_absent = true;
        return out;
    };
    {
        // Before any cast: unreadable or mismatched getter bytes, a filter with a
        // named bit, and, first of all, the unchanged actor exclusion refusal.
        ContactFixture f;
        masks(f, static_template, static_template);
        auto request = required(f, false);
        test_facts::probe_presets.clear();
        auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::named_actor_categories_unavailable && !out.native_casts && !f.calls && none(out),
            "the requirement cast without readable getter bytes");
        pin(f, false);
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::named_actor_categories_unavailable && !out.native_casts && !f.calls && none(out),
            "the requirement cast past a getter byte mismatch");
        pin(f, true);
        request.preset = QueryPreset::all;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::named_actor_categories_unavailable && !out.native_casts && !f.calls &&
            test_facts::probe_presets.empty() && none(out), "all reached a cast under the requirement");
        for (const auto preset : {QueryPreset::camera, QueryPreset::all}) {
            request.preset = preset;
            request.exclude_local_actor = true;
            out = probe_batch(owner, f.handle, request, 64);
            check(!out.available && out.failure == Failure::local_actor_exclusion_unavailable && !out.native_casts && !f.calls && none(out),
                "the requirement displaced the zero-cast actor exclusion refusal");
        }
        // all as a diagnostic, without the requirement, reaches its counted query.
        request.exclude_local_actor = request.require_named_actor_categories_absent = false;
        request.include_shape_categories = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && f.calls == 2 &&
            test_facts::probe_presets == std::vector<QueryPreset>{QueryPreset::all, QueryPreset::all} &&
            out.rays[0].in_categories == static_template && !out.named_actor_categories_absent && !out.local_actor_excluded,
            "the all diagnostic did not reach its counted query or claimed a fact");
    }
    for (const bool contact : {false, true}) {
        // Static-template hits pass under camera, with or without measured contact,
        // and their values appear only when they are also asked for.
        ContactFixture f;
        masks(f, static_template, static_template);
        pin(f, true);
        auto request = required(f, contact);
        auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && out.named_actor_categories_absent && !out.local_actor_excluded &&
            !out.rays[0].in_categories_available && !out.rays[1].in_categories_available,
            "a Static-template batch was refused, claimed an exclusion or showed categories");
        check(!contact || (out.rays[0].normal_available && out.rays[1].surface_velocity_available), "the requirement lost measured contact");
        request.include_shape_categories = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.named_actor_categories_absent && out.rays[0].in_categories == static_template &&
            out.rays[1].in_categories_available, "the requirement and the category diagnostic did not combine");
    }
    {
        // The overlap rule uses the preset's own filter: Static lacks the Terrain bit.
        ContactFixture f;
        masks(f, terrain_template, static_template);
        pin(f, true);
        auto request = required(f, false);
        request.preset = QueryPreset::terrain;
        auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_categories_inconsistent && out.native_casts == 2 && none(out),
            "a Static-template hit passed the terrain filter's overlap rule");
        masks(f, terrain_template, terrain_template);
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.named_actor_categories_absent && test_facts::probe_presets.back() == QueryPreset::terrain,
            "a Terrain-template batch was refused under terrain");
    }
    for (const unsigned bit : {2U, 3U, 21U, 25U, 26U, 28U, 29U, 31U}) {
        ContactFixture f;
        masks(f, static_template, static_template | (1U << bit));
        pin(f, true);
        auto request = required(f, false);
        request.include_shape_categories = true;
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_named_actor_category && out.native_casts == 2 && none(out) &&
            out.rays[1].status == body_query::Status::unavailable, "a hit carrying a named actor category passed or leaked");
    }
    for (const std::uint32_t missed : {0x40U, 0U}) {
        ContactFixture f;
        masks(f, static_template, missed);
        pin(f, true);
        auto request = required(f, false);
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_categories_inconsistent && out.native_casts == 2 && none(out),
            "a hit whose categories miss the filter passed");
    }
    {
        // A replacement array whose elements 0-3 lie on an unreadable page: both hit
        // shapes' values cannot be read, while the owned shape's element 5 can.
        ContactFixture f;
        pin(f, true);
        const auto pages = f.arena.take(8192, 4096);
        DWORD old{};
        check(VirtualProtect(reinterpret_cast<void*>(pages), 4096, PAGE_NOACCESS, &old) != 0, "category guard failed");
        put(f.state + r::shape_categories, pages + 4096 - 16);
        auto request = required(f, false);
        auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_categories_unavailable && out.native_casts == 2 && none(out),
            "an unreadable category passed the requirement");
        request.require_named_actor_categories_absent = false;
        request.include_shape_categories = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && !out.rays[0].in_categories_available && !out.rays[1].in_categories_available,
            "without the requirement an unreadable category was not merely absent");
        check(VirtualProtect(reinterpret_cast<void*>(pages), 4096, PAGE_READWRITE, &old) != 0, "category guard restore failed");
    }
    {
        // A value changed before the closing fence is dropped, so it cannot pass.
        ContactFixture f;
        masks(f, static_template, static_template);
        pin(f, true);
        auto request = required(f, false);
        f.during_query = [&](unsigned calls) { if (calls == 2) put(f.categories + ContactFixture::hit_shapes[0] * 4, static_template | 1U); };
        const auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_categories_unavailable && out.native_casts == 2 && none(out),
            "a category dropped at the closing fence passed the requirement");
    }
    for (const bool requirement : {false, true}) {
        // Each refused nearest hit names its reason, with its one actual cast.
        ContactFixture f;
        masks(f, static_template, static_template);
        pin(f, true);
        auto request = f.request(2, false);
        request.require_named_actor_categories_absent = requirement;
        f.hit_owned = true;
        auto out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::owned_body_hit && out.native_casts == 1 && f.calls == 1 && none(out),
            "an owned nearest hit did not name its reason");
        f.hit_owned = false;
        f.hit_unknown = true;
        out = probe_batch(owner, f.handle, request, 64);
        check(!out.available && out.failure == Failure::hit_identity_unqualified && out.native_casts == 1 && f.calls == 2 && none(out),
            "an unqualified nearest hit did not name its reason");
    }
    {
        // Owner slots: read at the hit shape's own element, unassigned is not absent,
        // only an opt-in sees them, and a change before the closing fence drops one.
        ContactFixture f;
        auto request = f.request(2, false);
        request.include_hit_owner = true;
        const auto slots = load<std::uintptr_t>(f.state + pb::body_owner_offset);
        put(slots + ContactFixture::hit_shapes[0] * 4, r::unassigned_shape_owner);
        put(slots + ContactFixture::hit_shapes[1] * 4, std::uint32_t{0x2a});
        // A decoy at the mapped foreign body's own index.
        put(slots + ContactFixture::foreign_index * 4, std::uint32_t{0x99});
        auto out = probe_batch(owner, f.handle, request, 64);
        check(out.available && out.native_casts == 2 && out.rays[0].owner_available && !out.rays[0].owner_assigned &&
            out.rays[1].owner_available && out.rays[1].owner_assigned && out.rays[1].owner == 0x2a &&
            !out.rays[0].in_categories_available && !out.local_actor_excluded, "the Service lost, mixed or body-indexed its hit owner slots");
        request.include_hit_owner = false;
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && !out.rays[0].owner_available && !out.rays[1].owner_available, "a default batch carried owner slots");
        request.include_hit_owner = true;
        const auto second = f.calls + 2;
        f.during_query = [&, second, slots](unsigned calls) { if (calls == second) put(slots + ContactFixture::hit_shapes[0] * 4, std::uint32_t{7}); };
        out = probe_batch(owner, f.handle, request, 64);
        check(out.available && !out.rays[0].owner_available && out.rays[1].owner_available && out.rays[1].owner == 0x2a,
            "an owner slot change refused the batch or kept a stale slot");
    }
}

void deferred_initialization_stays_outside_create_and_service_lock() {
    Fixture f;
    const auto definition = f.slot().definition;
    const auto read_api = service.api;
    service.slots = {};
    service.hooked = false;
    service.hook_attempted = false;
    service.initialization_requested = false;
    service.original = nullptr;
    service.api = {};
    test_facts::forbidden_links = 0;
    const auto no_allocation = [] {
        return std::none_of(service.slots.begin(), service.slots.end(), [](const Slot& slot) { return slot.attempted; });
    };
    // Until a plugin asks, a client tick only notes the phase: it reads no multiplayer state and
    // does no native work.
    test_facts::model_calls = 0;
    tick(f.base, f.client, true);
    check(!test_facts::model_calls && service.eligible && !service.initialization_requested && !service.hook_attempted &&
        test_facts::forbidden_links == 0 && no_allocation(),
        "a client tick with nothing requested read the multiplayer state or did native work");
    Handle out{77, 3};
    check(create(0, definition, out) == "invalid body definition" && !out.generation && !service.initialization_requested,
        "ownerless create queued producer preparation");
    auto invalid = definition;
    invalid.mass = 0;
    check(create(owner, invalid, out) == "invalid body definition" && !service.initialization_requested,
        "invalid definition queued producer preparation");
    std::string wrong_thread;
    std::thread other([&] { Handle ignored; wrong_thread = create(owner, definition, ignored); });
    other.join();
    check(!wrong_thread.empty() && !service.initialization_requested, "wrong thread queued producer preparation");
    test_facts::multiplayer_active = true;
    check(!create(owner, definition, out).empty() && !service.initialization_requested, "multiplayer queued producer preparation");
    test_facts::multiplayer_active = false;
    SetLastError(0x1234);
    check(create(owner, definition, out) == "waiting for a fresh qualified 60 Hz physics step" && !out.generation &&
        service.initialization_requested && !service.hook_attempted && test_facts::forbidden_links == 0 && no_allocation(),
        "cold create initialized hooks or reserved a body rather than only requesting setup");
    check(GetLastError() == 0x1234, "cold preparation request changed LastError");
    check(create(owner + 1, definition, out) == "waiting for a fresh qualified 60 Hz physics step" &&
        service.initialization_requested && test_facts::forbidden_links == 0 && no_allocation(),
        "a second generic preparation request performed native work");
    tick(f.base, f.client, false);
    check(!service.initialization_requested && !service.hook_attempted && test_facts::forbidden_links == 0 && no_allocation(),
        "ineligible tick retained or executed a stale preparation request");
    tick(f.base, f.client, true);
    check(!service.hook_attempted && test_facts::forbidden_links == 0, "eligibility restoration automatically prepared without a new request");
    check(create(owner, definition, out) == "waiting for a fresh qualified 60 Hz physics step", "fresh explicit preparation request refused");
    test_facts::inspect_initializer_lock = true;
    test_facts::initializer_unlocked = false;
    test_facts::initializer_elapsed = 251;
    const auto requested_at = test_clock::now.load();
    SetLastError(0x5678);
    tick(f.base, f.client, true);
    test_facts::inspect_initializer_lock = false;
    test_facts::initializer_elapsed = 0;
    check(GetLastError() == 0x5678, "deferred initializer tick changed LastError");
    check(service.hook_attempted && !service.hooked && !service.initialization_requested &&
        test_facts::forbidden_links == 1 && test_facts::initializer_unlocked && no_allocation(),
        "actual fail-closed initializer did not run exactly once outside the service SRW lock");
    check(service.tick_at == requested_at && test_clock::now == requested_at + 251 && !eligible(),
        "slow producer setup refreshed old readiness instead of failing closed");
    tick(f.base, f.client, true);
    check(create(owner, definition, out) == "native body ABI or producer hook unavailable; another producer may own it" &&
        !service.initialization_requested && test_facts::forbidden_links == 1 && no_allocation(),
        "failed producer initialization was disguised as retryable warmup");
    tick(f.base, f.client, true);
    check(test_facts::forbidden_links == 1 && no_allocation(), "failed initialization automatically repeated");
    // Constructed ready state tests the later create gate, not native hook success.
    service.api = read_api;
    service.hooked = true;
    clock_feed.serial = 0;
    clock_feed.sequence = 0;
    tick(f.base, f.client, true);
    check(create(owner, definition, out) == "waiting for a fresh qualified 60 Hz physics step" && no_allocation() &&
        !service.initialization_requested && test_facts::mutations == 0,
        "constructed producer readiness bypassed the original fresh clock or allocated automatically");
}

#include "body_prepare_source_fixture.inc"

template<class F> void run(const char* name, F function, unsigned expected_links = 0) {
    function();
    check(test_facts::mutations.load() == 0, "source path invoked a native mutator/allocator");
    check(test_facts::forbidden_links.load() == expected_links, "source path entered an unexpected hook/query/constructor dependency");
    ++cases;
    std::cout << "PASS " << name << '\n';
}
} // namespace dingosdk::owned_bodies::source_test

int main() {
    using namespace dingosdk::owned_bodies::source_test;
    try {
        run("service_contact_opt_in_and_read_only", service_contact_opt_in_and_read_only);
        run("service_whole_batch_contact_fences", service_whole_batch_contact_fences);
        run("service_actor_and_nearest_hit_refusals", service_actor_and_nearest_hit_refusals);
        run("service_forwards_query_preset_without_relaxing_refusals", service_forwards_query_preset_without_relaxing_refusals);
        run("service_shape_categories_opt_in_shape_bound_and_fenced", service_shape_categories_are_opt_in_shape_bound_and_fenced);
        run("service_named_actor_categories_reasons_and_hit_owners", service_named_actor_categories_reasons_and_hit_owners);
        run("deferred_producer_initialization_without_allocation", deferred_initialization_stays_outside_create_and_service_lock, 1);
        run("prepare_cold_queues_only_deferred_setup", prepare_cold_queues_only_deferred_setup);
        run("prepare_deferred_failure_is_terminal_without_retry", prepare_deferred_failure_is_terminal_without_retry, 1);
        run("prepare_warm_readiness_needs_fresh_capture_and_clock", prepare_warm_readiness_needs_fresh_capture_and_clock);
        run("prepare_does_not_reconcile_world_or_touch_existing_slots", prepare_does_not_reconcile_world_or_touch_existing_slots);
        std::cout << cases << " actual-Service cases, " << checks << " checks passed; native mutations "
                  << test_facts::mutations.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
