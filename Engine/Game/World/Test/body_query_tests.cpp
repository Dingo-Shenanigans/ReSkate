#include "Engine/Game/World/body_query.h"
#include "Engine/Game/World/body_hit_identity.h"
#include "Engine/Game/World/body_shape_categories.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dingosdk::body_query;
namespace {
static_assert(static_cast<unsigned>(Failure::none) == 0 && static_cast<unsigned>(Failure::backend) == 6);
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
Request request(std::size_t count = 1) {
    Request out;
    out.observation.epoch = {7, 11, 100, 3, static_cast<float>(dingosdk::owned_body::native_seconds)};
    out.count = count;
    out.observation.mass = 12;
    out.observation.inertia = {2, 3, 4};
    out.observation.rotation = {0, 0, 0, 1};
    for (auto& ray : out.rays) ray = {{1, 2, 3}, {1, -2, 3}};
    return out;
}
void empty(const Result& out) {
    check(!out.available, "partial batch was available");
    for (const auto& hit : out.rays) check(hit.status == Status::unavailable && hit.position == Vec{} && !hit.fraction, "partial geometry escaped");
}
void bounds_and_backend_geometry() {
    auto r = request();
    check(valid(r, 1000), "bounded request rejected");
    auto bad = r;
    bad.count = 0;
    check(!valid(bad, 1000), "empty batch");
    bad = r;
    bad.count = maximum_rays + 1;
    check(!valid(bad, 1000), "ray quota");
    bad = r;
    bad.exclusions = maximum_exclusions + 1;
    check(!valid(bad, 1000), "exclusion quota");
    bad = r;
    bad.observation.epoch.serial = 0;
    check(!valid(bad, 1000), "missing epoch");
    bad = r;
    bad.observation.epoch.seconds = .02f;
    check(!valid(bad, 1000), "uncalibrated step");
    bad = r;
    bad.observation.epoch.time = std::numeric_limits<double>::quiet_NaN();
    check(!valid(bad, 1000), "nonfinite epoch");
    bad = r;
    bad.rays[0].to = bad.rays[0].from;
    check(!valid(bad, 1000), "zero direction");
    bad = r;
    bad.rays[0].to[0] = 100001;
    check(!valid(bad, 1000), "map bounds");
    bad = r;
    bad.rays[0].to[0] = 1002;
    check(!valid(bad, 1000), "distance limit");
    bad = r;
    bad.rays[0].to[0] = std::numeric_limits<float>::infinity();
    check(!valid(bad, 1000), "infinite coordinate");
    Hit hit{Status::hit, {1, 0, 3}, .5f};
    check(consistent(r.rays[0], hit), "actual segment hit rejected");
    hit.position[0] = 2;
    check(!consistent(r.rays[0], hit), "off-segment hit");
    hit = {Status::hit, {1, 0, 3}, -.5f};
    check(!consistent(r.rays[0], hit), "negative fraction");
}
void counts_and_discard_are_bounded() {
    auto r = request(32);
    unsigned calls{}, fences{};
    const auto fence = [&] { ++fences; return true; };
    const auto clear = [&](const Ray&, unsigned allowance) { ++calls; check(allowance == 2, "per-ray cast allowance"); return Probe{{Status::clear}, 2}; };
    auto out = execute(r, 64, 1000, clear, fence);
    check(out.available && out.native_casts == 64 && calls == 32 && fences == 2, "all-clear maximum batch");
    for (const auto& hit : out.rays) check(hit.status == Status::clear && hit.position == Vec{}, "clear ray invented geometry");
    calls = fences = 0;
    out = execute(r, 63, 1000, [&](const Ray&, unsigned allowance) {
        ++calls;
        return allowance == 2 ? Probe{{Status::hit, {1, 0, 3}, .5f}, 2} : Probe{{}, allowance, true};
    }, fence);
    check(out.native_casts == 63 && calls == 32 && fences == 2 && out.failure == Failure::budget && out.budget_exhausted, "last retry escaped shared allowance");
    empty(out);
    calls = fences = 0;
    out = execute(request(), 64, 1000, [&](const Ray&, unsigned allowance) { ++calls; return Probe{{}, allowance, true}; }, fence);
    check(out.native_casts == 2 && calls == 1 && fences == 2 && out.failure == Failure::ray_limit, "unresolved exclusion silently retried or called clear");
    empty(out);
    out = execute(request(), 1, 1000, [](const Ray&, unsigned allowance) { return Probe{{}, allowance, true}; }, fence);
    check(out.native_casts == 1 && out.failure == Failure::budget, "shared exhaustion mislabeled");
    empty(out);
    calls = fences = 0;
    out = execute(r, 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{}; }, fence);
    check(!out.native_casts && calls == 1 && fences == 2 && out.failure == Failure::query_preconditions_unavailable, "zero-cast preconditions were not distinguished");
    empty(out);
    out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{{}, 1}; }, fence);
    check(out.native_casts == 1 && out.failure == Failure::unavailable, "after-cast unavailable was called a precondition refusal");
    empty(out);
    calls = 0;
    out = execute(request(2), 64, 1000, [&](const Ray&, unsigned) {
        return ++calls == 1 ? Probe{{Status::clear}, 1} : Probe{};
    }, fence);
    check(out.native_casts == 1 && calls == 2 && out.failure == Failure::unavailable, "later zero-cast refusal hid earlier native calls");
    empty(out);
    calls = 0;
    out = execute(r, 0, 1000, clear, fence);
    check(!calls && !out.native_casts && out.failure == Failure::budget, "zero budget called backend");
    out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{{Status::clear}, 3}; }, fence);
    check(!out.accounting_valid && out.failure == Failure::backend && !out.native_casts, "malformed count invented a valid actual count");
    empty(out);
    out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{{Status::clear}, 0}; }, fence);
    check(out.failure == Failure::backend, "zero-cast clear admitted");
    empty(out);
    out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{{}, 0, true}; }, fence);
    check(out.failure == Failure::ray_limit && out.budget_exhausted, "precondition detail overrode exhausted retry allowance");
    empty(out);
}
// A preset names a fixed native filter and nothing else. The shared core
// refuses an unknown value before its first fence or query and otherwise keeps
// every accounting, contact and whole-batch rule exactly as for camera.
void query_presets_are_fixed_and_default_to_camera() {
    static_assert(static_cast<unsigned>(QueryPreset::camera) == 0, "a zeroed request must mean camera");
    check(Request{}.preset == QueryPreset::camera && request().preset == QueryPreset::camera, "default preset is not camera");
    check(std::string_view(name(QueryPreset::camera)) == "camera" && std::string_view(name(QueryPreset::terrain)) == "terrain" &&
        std::string_view(name(QueryPreset::all)) == "all", "preset names changed");
    for (const auto unknown : {static_cast<QueryPreset>(3), static_cast<QueryPreset>(255)}) {
        check(!name(unknown), "an unknown preset value was given a name");
        auto bad = request(2);
        bad.preset = unknown;
        check(!valid(bad, 1000), "an unknown preset value passed validation");
        unsigned calls{}, fences{};
        const auto out = execute(bad, 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{{Status::clear}, 1}; },
            [&] { ++fences; return true; });
        check(!calls && !fences && !out.native_casts && out.failure == Failure::invalid && out.accounting_valid,
            "an unknown preset reached a fence or the query");
        empty(out);
    }
    const auto run = [](QueryPreset preset, std::size_t count, auto query) {
        auto r = request(count);
        r.preset = preset;
        r.require_contact = true;
        check(valid(r, 1000), "a fixed preset was rejected");
        return execute(r, 64, 1000, query, [] { return true; });
    };
    const auto measured = [](const Ray&, unsigned) {
        Probe out{{Status::hit, {1, 0, 3}, .5f}, 1};
        out.hit.normal = {0, 1, 0};
        out.hit.surface_velocity = {2, 0, -1};
        out.hit.normal_available = out.hit.surface_velocity_available = true;
        return out;
    };
    for (const auto preset : {QueryPreset::camera, QueryPreset::terrain, QueryPreset::all}) {
        auto out = run(preset, 2, measured);
        check(out.available && out.native_casts == 2 && out.rays[1].surface_velocity == Vec{2, 0, -1},
            "a preset changed accepted measured hits or their motion");
        out = run(preset, 2, [](const Ray&, unsigned) { return Probe{{Status::hit, {1, 0, 3}, .5f}, 1}; });
        check(out.failure == Failure::contact_measurement_unavailable && out.native_casts == 1,
            "a preset admitted a hit without measurements");
        empty(out);
        unsigned calls{};
        out = run(preset, 3, [&](const Ray& ray, unsigned allowance) { return ++calls == 2 ? Probe{{}, 1} : measured(ray, allowance); });
        check(out.failure == Failure::unavailable && out.native_casts == 2 && calls == 2, "a preset let a partial batch escape");
        empty(out);
        out = run(preset, 1, [](const Ray&, unsigned) { return Probe{{Status::clear}, 3}; });
        check(!out.accounting_valid && out.failure == Failure::backend, "a preset relaxed native call accounting");
        empty(out);
    }
}
void typed_refusal_names_are_stable() {
    check(std::string_view(name(Failure::client_phase_unavailable)) == "client_phase_unavailable", "client phase reason");
    check(std::string_view(name(Failure::local_actor_exclusion_unavailable)) == "local_actor_exclusion_unavailable", "actor exclusion reason");
    check(std::string_view(name(Failure::query_preconditions_unavailable)) == "query_preconditions_unavailable", "query preconditions reason");
    check(std::string_view(name(Failure::query_backend_exception)) == "query_backend_exception", "query exception reason");
    check(std::string_view(name(Failure::body_exclusions_unavailable)) == "body_exclusions_unavailable", "owned exclusion reason");
}
void every_observation_fence_invalidates_the_whole_batch() {
    // Each field represents a native adapter fence: context, world generation/state, actor,
    // listener clock, owned-body generation and exclusion lifetime. None is a world mutex.
    for (unsigned changed = 0; changed < 7; ++changed) {
        std::array<unsigned, 7> observation{1, 2, 3, 4, 5, 6, 7};
        const auto before = observation;
        unsigned calls{};
        const auto out = execute(request(3), 64, 1000, [&](const Ray&, unsigned) {
            if (++calls == 2) ++observation[changed];
            return Probe{{Status::hit, {1, 0, 3}, .5f}, 1};
        }, [&] { return observation == before; });
        check(out.failure == Failure::observation_changed && out.native_casts == 3, "changed observation escaped final fence");
        empty(out);
    }
    unsigned calls{}, fences{};
    auto out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{}; }, [&] { return ++fences == 1; });
    check(calls == 1 && fences == 2 && out.failure == Failure::observation_changed, "failed query omitted after fence");
    out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{}; }, [] { return false; });
    check(calls == 1 && !out.native_casts && out.failure == Failure::observation_changed, "stale epoch reached backend");
}
void one_budget_charges_actual_retries() {
    Budget budget;
    check(budget.remaining() == 64, "fresh shared budget");
    for (unsigned i = 0; i < 31; ++i) check(budget.consume(2, 2), "two-call query charge");
    check(budget.remaining() == 2 && budget.consume(1, 2) && budget.remaining() == 1, "mixed one/two cast consumption");
    check(budget.consume(0, 1) && budget.remaining() == 1, "unavailable fabricated a cast");
    check(budget.consume(1, 1) && !budget.remaining(), "final actual call");
    check(budget.consume(0, 0) && !budget.consume(1, 0) && !budget.remaining(), "exhaustion escaped");
    budget = {};
    check(!budget.consume(3, 2) && !budget.remaining(), "bad backend failed to poison tick");
    budget = {};
    check(budget.remaining(4) == 4 && budget.remaining(1000) == 64, "configured budget raised hard cap");
}
void a_listener_epoch_does_not_freeze_canonical_body_state() {
    const auto r = request(2);
    const auto canonical = r.observation;
    for (unsigned field = 0; field < 7; ++field) {
        auto latest = canonical, native = canonical;
        unsigned calls{};
        const auto mutate = [&](dingosdk::owned_body::Sample& sample) {
            switch (field) {
            case 0: sample.position[0] += .001f; break;
            case 1: sample.rotation[2] += .001f; break;
            case 2: sample.velocity[1] += .001f; break;
            case 3: sample.angular_velocity[2] += .001f; break;
            case 4: sample.inertia[0] += .001f; break;
            case 5: sample.mass += .001f; break;
            case 6: sample.velocity[0] = -0.f; break;
            }
        };
        const auto fence = [&] { return same_sample(canonical, latest) && same_sample(canonical, native); };
        auto out = execute(r, 64, 1000, [&](const Ray&, unsigned) {
            ++calls;
            mutate(native);
            return Probe{{Status::hit, {1, 0, 3}, .5f}, 1};
        }, fence);
        check(native.epoch == canonical.epoch && out.failure == Failure::observation_changed && out.native_casts == 2,
            "same-listener-epoch native state drift escaped post fence");
        empty(out);
        native = canonical;
        mutate(native);
        calls = 0;
        out = execute(r, 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{{Status::clear}, 1}; }, fence);
        check(!calls && !out.native_casts && out.failure == Failure::observation_changed, "same-epoch native drift reached query");
        // A later read/receipt can publish new latest in the same epoch. Its immutable older
        // token must not become paired with that replacement, even if native now matches it.
        latest = native;
        out = execute(r, 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{{Status::clear}, 1}; }, fence);
        check(!calls && !out.native_casts && !same_sample(canonical, latest) && out.failure == Failure::observation_changed,
            "same-epoch latest replacement was mistaken for canonical sample");
        check(same_sample(r.observation, canonical), "query overwrote immutable canonical state");
    }
}
namespace identity = dingosdk::body_hit_identity;
using NativeHitHandle = identity::NativeHandle;

struct SparseHitMemory {
    std::map<std::uintptr_t, unsigned char> bytes;
    std::vector<std::uintptr_t> reads;
    std::function<void(std::uintptr_t, std::size_t)> before_read;
    template<class T> void put(std::uintptr_t at, const T& value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[at + i] = data[i];
    }
    template<class T> bool operator()(std::uintptr_t at, T& value) {
        reads.push_back(at);
        if (before_read) before_read(at, sizeof(T));
        std::array<unsigned char, sizeof(T)> data{};
        for (std::size_t i = 0; i < data.size(); ++i) {
            const auto found = bytes.find(at + i);
            if (found == bytes.end()) return false;
            data[i] = found->second;
        }
        std::memcpy(&value, data.data(), sizeof(T));
        return true;
    }
    bool touched(std::uintptr_t at) const { return std::find(reads.begin(), reads.end(), at) != reads.end(); }
};

struct HitFixture {
    // Literal layout is independent of the production Build constants. A wrong
    // field or the old state/world generation-base confusion must break the test.
    static constexpr std::uintptr_t world = 0x10000, state = 0x20000;
    static constexpr std::uintptr_t shape_bits = 0x30000, shape_generations = 0x40000, mappings = 0x50000;
    static constexpr std::uintptr_t body_generations = 0x60000, other_body_generations = 0x68000;
    SparseHitMemory memory;
    std::array<bool, 64> allocated_bodies{};
    std::uint32_t body_capacity = 64, gate_index{};
    bool allow_world{true}, allow_shape{true}, allow_body{true}, gate_passed{};
    unsigned world_calls{}, shape_calls{}, allocation_calls{}, body_calls{}, generation_reads{};
    std::function<void()> after_first_body;
    HitFixture() {
        memory.put(state + 0xf58, shape_bits);
        memory.put(state + 0xff8, shape_generations);
        memory.put(state + 0xf98, mappings);
        memory.put(world + 0x680, body_generations);
        // An attractive but wrong generation array at state+680 has a different value.
        memory.put(state + 0x680, std::uintptr_t{0x70000});
        for (const std::uint32_t body : {0U, 9U, 57U}) {
            allocated_bodies[body] = true;
            memory.put(body_generations + body * sizeof(std::uint32_t), std::uint32_t{4});
            memory.put(other_body_generations + body * sizeof(std::uint32_t), std::uint32_t{4});
            memory.put(0x70000 + body * sizeof(std::uint32_t), std::uint32_t{99});
        }
        seed_shape(9, 4, 57);
        memory.before_read = [&](std::uintptr_t at, std::size_t size) {
            for (const auto array : {body_generations, other_body_generations}) {
                if (size != sizeof(std::uint32_t) || at < array || at >= array + allocated_bodies.size() * sizeof(std::uint32_t)) continue;
                const auto index = (at - array) / sizeof(std::uint32_t);
                check(gate_passed && gate_index == index && index < body_capacity && allocated_bodies[index],
                    "body generation was read before the independent capacity/allocation gate");
                ++generation_reads;
            }
        };
    }
    HitFixture(const HitFixture&) = delete;
    HitFixture& operator=(const HitFixture&) = delete;
    void seed_shape(std::uint32_t index, std::uint8_t generation, std::uint32_t body) {
        auto& bits = memory.bytes[shape_bits + (index >> 3)];
        bits = static_cast<unsigned char>(bits | (1U << (index & 7U)));
        memory.put(shape_generations + index, generation);
        memory.put(mappings + index * sizeof(std::uint32_t), body);
    }
    bool resolve(NativeHitHandle shape, NativeHitHandle& body) {
        gate_passed = false;
        const auto reads_before = generation_reads, validations_before = body_calls;
        // These callbacks are native/world/allocation oracles, not another
        // implementation of the production shape-to-body resolver.
        return identity::resolve(world, state, shape, body, memory,
            [&] { ++world_calls; return allow_world; },
            [&](const NativeHitHandle&) { ++shape_calls; return allow_shape; },
            [&](std::uint32_t index) {
                ++allocation_calls;
                gate_index = index;
                gate_passed = index < body_capacity && index < allocated_bodies.size() && allocated_bodies[index];
                return gate_passed;
            },
            [&](const NativeHitHandle& candidate) {
                ++body_calls;
                check(gate_passed && candidate.index == gate_index &&
                    generation_reads - reads_before >= body_calls - validations_before,
                    "native body validator preceded the allocated body generation read");
                const bool accepted = allow_body;
                if (body_calls == 1 && after_first_body) after_first_body();
                return accepted;
            });
    }
};

void hit_association_uses_shape_and_body_domains() {
    HitFixture fixture;
    NativeHitHandle body;
    const NativeHitHandle shape{HitFixture::world, 9, 4}, excluded{HitFixture::world, 9, 4};
    check(fixture.resolve(shape, body) && body == NativeHitHandle{HitFixture::world, 57, 4},
        "shape identity was mistaken for its mapped rigid body");
    check(body != excluded, "coincident shape/body numbers falsely excluded another body");
    check(fixture.memory.touched(HitFixture::world + 0x680) && !fixture.memory.touched(HitFixture::state + 0x680),
        "body generation came from the state instead of the world");
    check(fixture.shape_calls >= 2 && fixture.body_calls >= 2 && fixture.generation_reads >= 2,
        "association was not checked before and after resolution");
    fixture.seed_shape(9, 4, 9);
    check(fixture.resolve(shape, body) && body == excluded, "mapped excluded body was hidden by a different handle domain");
    HitFixture zero;
    zero.seed_shape(0, 0, 0);
    zero.memory.put(HitFixture::body_generations, std::uint32_t{0});
    check(zero.resolve({HitFixture::world, 0, 0}, body) && body == NativeHitHandle{HitFixture::world, 0, 0},
        "valid index-zero/generation-zero association was rejected");
}

void unknown_hit_identity_stops_before_unsafe_body_reads() {
    for (unsigned missing = 0; missing < 11; ++missing) {
        HitFixture fixture;
        NativeHitHandle body{11, 12, 13};
        NativeHitHandle shape{HitFixture::world, 9, 4};
        switch (missing) {
        case 0: fixture.allow_world = false; break;
        case 1: fixture.allow_shape = false; break;
        case 2: fixture.memory.put(HitFixture::shape_bits + 1, std::uint8_t{0}); break;
        case 3: fixture.memory.put(HitFixture::shape_generations + 9, std::uint8_t{5}); break;
        case 4: fixture.memory.put(HitFixture::state + 0xf98, std::uintptr_t{0}); break;
        case 5: fixture.memory.bytes.erase(HitFixture::mappings + 9 * sizeof(std::uint32_t)); break;
        case 6: fixture.seed_shape(9, 4, 0xffffffffU); break;
        case 7: fixture.seed_shape(9, 4, fixture.body_capacity); break;
        case 8: fixture.allocated_bodies[57] = false; break;
        case 9: fixture.memory.put(HitFixture::state + 0xff8, std::uintptr_t{0}); break;
        case 10: fixture.memory.put(HitFixture::state + 0xf58, std::uintptr_t{0}); break;
        }
        check(!fixture.resolve(shape, body) && body == NativeHitHandle{}, "unknown hit association published a partial body");
        check(!fixture.generation_reads && !fixture.body_calls && !fixture.memory.touched(HitFixture::world + 0x680),
            "unqualified shape or unallocated body reached the generation array/validator");
    }
    for (const NativeHitHandle shape : {NativeHitHandle{HitFixture::world + 1, 9, 4},
             NativeHitHandle{HitFixture::world, 0xffffffffU, 4}, NativeHitHandle{HitFixture::world, 9, 256}}) {
        HitFixture fixture;
        NativeHitHandle body{11, 12, 13};
        check(!fixture.resolve(shape, body) && body == NativeHitHandle{} && fixture.memory.reads.empty() && !fixture.body_calls,
            "invalid hit handle reached pool reads");
    }
    for (const bool missing_element : {false, true}) {
        HitFixture fixture;
        NativeHitHandle body{11, 12, 13};
        if (missing_element) fixture.memory.bytes.erase(HitFixture::body_generations + 57 * sizeof(std::uint32_t));
        else fixture.memory.put(HitFixture::world + 0x680, std::uintptr_t{0});
        check(!fixture.resolve({HitFixture::world, 9, 4}, body) && body == NativeHitHandle{} && !fixture.body_calls,
            "unreadable body generation published a partial body or reached its validator");
    }
    HitFixture rejected;
    rejected.allow_body = false;
    NativeHitHandle body{11, 12, 13};
    check(!rejected.resolve({HitFixture::world, 9, 4}, body) && body == NativeHitHandle{} && rejected.generation_reads == 1,
        "native body validator refusal escaped");
}

void hit_association_rechecks_mapping_generations_and_array_heads() {
    for (unsigned change = 0; change < 12; ++change) {
        HitFixture fixture;
        NativeHitHandle body{11, 12, 13};
        fixture.after_first_body = [&] {
            switch (change) {
            case 0: fixture.seed_shape(9, 4, 9); break;
            case 1: fixture.memory.put(HitFixture::shape_generations + 9, std::uint8_t{5}); break;
            case 2: fixture.memory.put(HitFixture::shape_bits + 1, std::uint8_t{0}); break;
            case 3:
                fixture.memory.put(std::uintptr_t{0x31001}, std::uint8_t{2});
                fixture.memory.put(HitFixture::state + 0xf58, std::uintptr_t{0x31000});
                break;
            case 4:
                fixture.memory.put(std::uintptr_t{0x41009}, std::uint8_t{4});
                fixture.memory.put(HitFixture::state + 0xff8, std::uintptr_t{0x41000});
                break;
            case 5:
                fixture.memory.put(std::uintptr_t{0x51000} + 9 * sizeof(std::uint32_t), std::uint32_t{57});
                fixture.memory.put(HitFixture::state + 0xf98, std::uintptr_t{0x51000});
                break;
            case 6: fixture.memory.put(HitFixture::world + 0x680, HitFixture::other_body_generations); break;
            case 7: fixture.memory.put(HitFixture::body_generations + 57 * sizeof(std::uint32_t), std::uint32_t{5}); break;
            case 8: fixture.allocated_bodies[57] = false; break;
            case 9: fixture.body_capacity = 57; break;
            case 10: fixture.allow_world = false; break;
            case 11: fixture.allow_shape = false; break;
            }
        };
        check(!fixture.resolve({HitFixture::world, 9, 4}, body) && body == NativeHitHandle{},
            "association drift or pool relocation escaped repeated qualification");
    }
    HitFixture changed_validator;
    NativeHitHandle body;
    changed_validator.after_first_body = [&] { changed_validator.allow_body = false; };
    check(!changed_validator.resolve({HitFixture::world, 9, 4}, body) && body == NativeHitHandle{},
        "final native body validation was omitted");
}

void every_returned_shape_body_pair_reaches_the_final_batch_fence() {
    for (const bool mutate_second : {false, true}) {
        HitFixture fixture;
        fixture.seed_shape(10, 4, 57);
        identity::Witnesses<2> witnesses;
        unsigned calls{};
        std::vector<std::uint32_t> final_shapes;
        const auto out = execute(request(2), 64, 1000, [&](const Ray&, unsigned) {
            const NativeHitHandle shape{HitFixture::world, 9 + calls++, 4};
            NativeHitHandle body;
            check(fixture.resolve(shape, body), "initial association fixture was unavailable");
            check(witnesses.add({shape, body}), "bounded witness storage rejected a returned hit");
            if (calls == 2 && mutate_second) fixture.seed_shape(10, 4, 9);
            return Probe{{Status::hit, {1, 0, 3}, .5f}, 1};
        }, [&] {
            return witnesses.current([&](const identity::Witness& witness) {
                final_shapes.push_back(witness.shape.index);
                NativeHitHandle current;
                return fixture.resolve(witness.shape, current) && current == witness.body;
            });
        });
        check(witnesses.count == 2 && witnesses.values[0].body == witnesses.values[1].body &&
            witnesses.values[0].shape != witnesses.values[1].shape, "two shapes sharing a body were deduplicated");
        check(final_shapes == std::vector<std::uint32_t>{9, 10}, "final fence omitted a returned shape/body pair");
        check(out.native_casts == 2, "association refusal lost actual cast accounting");
        if (mutate_second) {
            check(out.failure == Failure::observation_changed, "changed second shape escaped the batch fence");
            empty(out);
        } else {
            check(out.available && out.rays[0].status == Status::hit && out.rays[1].status == Status::hit,
                "current pairs failed");
        }
        check(!witnesses.add({{HitFixture::world, 11, 4}, {HitFixture::world, 57, 4}}) && witnesses.count == 2,
            "witness capacity overflow overwrote an earlier returned pair");
    }
}

// Shape categories are opt-in batch metadata. They never ride a probe, attach
// to a clear or unavailable row, or survive a refused batch. They are copied in
// after execute's closing fence, so a downgrade there is what the result shows,
// and asking for them changes no accounting, contact or nearest-hit rule.
void shape_categories_attach_only_to_hits_after_the_closing_fence() {
    using Sidecars = dingosdk::body_shape_categories::Sidecars<maximum_rays>;
    check(!Request{}.include_shape_categories && !request().include_shape_categories && !Hit{}.in_categories_available,
        "categories are not opt-in or a default row claims one");
    const auto base = request();
    Hit row{Status::clear};
    row.in_categories_available = true;
    check(!consistent(base.rays[0], row), "a clear row carried a category");
    row = {};
    row.in_categories_available = true;
    check(!consistent(base.rays[0], row), "an unavailable row carried a category");
    row = {Status::hit, {1, 0, 3}, .5f};
    row.in_categories = 0xffffffffU;
    row.in_categories_available = true;
    check(consistent(base.rays[0], row), "a hit row cannot hold its category");
    for (const auto status : {Status::hit, Status::clear}) {
        auto selected = request(2);
        selected.include_shape_categories = true;
        auto out = execute(selected, 64, 1000, [&](const Ray&, unsigned) {
            Probe probe{{status, {1, 0, 3}, .5f}, 1};
            probe.hit.in_categories_available = true;
            return probe;
        }, [] { return true; });
        check(out.failure == Failure::backend && out.native_casts == 1, "a probe that carried a category was accepted");
        empty(out);
    }
    for (unsigned variant = 0; variant < 6; ++variant) {
        // Shapes 9 and 10 share body 57; element 57 is a decoy for a body-index read.
        HitFixture fixture;
        fixture.seed_shape(10, 4, 57);
        constexpr std::uintptr_t categories = 0x80000;
        fixture.memory.put(HitFixture::state + 0xfe8, categories);
        fixture.memory.put(categories + 9 * sizeof(std::uint32_t), std::uint32_t{0x80000000U});
        fixture.memory.put(categories + 10 * sizeof(std::uint32_t), std::uint32_t{1});
        fixture.memory.put(categories + 57 * sizeof(std::uint32_t), std::uint32_t{0xdeadbeefU});
        auto selected = request(2);
        selected.include_shape_categories = variant != 5;
        Sidecars sidecars;
        unsigned calls{};
        const auto qualified = [&](const NativeHitHandle& shape, const NativeHitHandle& body) {
            NativeHitHandle current;
            return fixture.resolve(shape, current) && current == body;
        };
        auto out = execute(selected, 64, 1000, [&](const Ray&, unsigned) {
            const auto ordinal = calls++;
            const NativeHitHandle shape{HitFixture::world, 9 + ordinal, 4};
            NativeHitHandle body;
            if (variant == 4 && ordinal == 1) return Probe{{}, 1};
            check(fixture.resolve(shape, body), "fixture hit was unavailable");
            if (selected.include_shape_categories &&
                !sidecars.capture(ordinal, HitFixture::world, HitFixture::state, shape, body, fixture.memory, qualified)) return Probe{{}, 1};
            return Probe{{Status::hit, {1, 0, 3}, .5f}, 1};
        }, [&] {
            if (calls == 2 && variant == 1) fixture.memory.put(categories + 9 * sizeof(std::uint32_t), std::uint32_t{2});
            if (calls == 2 && variant == 2) fixture.seed_shape(10, 4, 9);
            return !(calls == 2 && variant == 3) && sidecars.current(fixture.memory, qualified);
        });
        sidecars.publish(selected, out);
        switch (variant) {
        case 0:
            check(out.available && out.native_casts == 2 && out.rays[0].in_categories_available && out.rays[0].in_categories == 0x80000000U &&
                out.rays[1].in_categories_available && out.rays[1].in_categories == 1, "shape-bound categories were lost or read by body index");
            break;
        case 1:
            check(out.available && !out.rays[0].in_categories_available && out.rays[1].in_categories_available,
                "publication did not reflect the closing sidecar downgrade");
            break;
        case 2: case 3: case 4:
            check(!out.available && out.native_casts == 2 && (variant == 4 ? out.failure == Failure::unavailable : out.failure == Failure::observation_changed),
                "a failed closing fence or partial batch kept its reason or casts wrong");
        empty(out);
            for (const auto& hit : out.rays) check(!hit.in_categories_available, "a refused batch published a category");
            break;
        case 5:
            check(out.available && !out.rays[0].in_categories_available && !out.rays[1].in_categories_available &&
                !fixture.memory.touched(HitFixture::state + 0xfe8), "a batch without the option read or carried categories");
            break;
        }
    }
    // Asking for categories leaves accounting, contact and nearest-hit rules alone.
    for (const bool categories : {false, true}) {
        auto selected = request(3);
        selected.include_shape_categories = categories;
        selected.require_contact = true;
        unsigned calls{};
        auto out = execute(selected, 64, 1000, [&](const Ray&, unsigned) { ++calls; return Probe{{Status::hit, {1, 0, 3}, .5f}, 1}; },
            [] { return true; });
        check(out.failure == Failure::contact_measurement_unavailable && out.native_casts == 1 && calls == 1,
            "the option changed the contact requirement");
        empty(out);
        selected.require_contact = false;
        out = execute(selected, 4, 1000, [](const Ray&, unsigned allowance) { return Probe{{Status::clear}, allowance}; }, [] { return true; });
        check(out.failure == Failure::budget && out.native_casts == 4 && out.budget_exhausted, "the option changed shared cast accounting");
        empty(out);
        out = execute(selected, 64, 1000, [](const Ray&, unsigned) { return Probe{{Status::hit, {1, 0, 3}, .5f}, 2}; }, [] { return true; });
        check(out.available && out.native_casts == 6 && out.rays[2].position == Vec{1, 0, 3} && out.rays[2].fraction == .5f &&
            !out.rays[2].in_categories_available, "the option changed the nearest hit or invented a category");
    }
}

// A probe may name why its one actual hit was refused. Only a per-ray reason, with
// the cast that found the hit, replaces the generic one; budget and ray-limit
// exhaustion and a changed closing observation still outrank it.
void per_ray_reasons_name_the_refused_hit() {
    check(std::string_view(name(Failure::owned_body_hit)) == "owned_body_hit" &&
        std::string_view(name(Failure::hit_identity_unqualified)) == "hit_identity_unqualified", "per-ray reason names");
    check(std::string_view(name(Failure::named_actor_categories_unavailable)) == "named_actor_categories_unavailable" &&
        std::string_view(name(Failure::hit_categories_unavailable)) == "hit_categories_unavailable" &&
        std::string_view(name(Failure::hit_categories_inconsistent)) == "hit_categories_inconsistent" &&
        std::string_view(name(Failure::hit_named_actor_category)) == "hit_named_actor_category", "category requirement names");
    check(Probe{}.failure == Failure::none, "a probe names a reason by default");
    const auto fence = [] { return true; };
    for (const auto reason : {Failure::owned_body_hit, Failure::hit_identity_unqualified}) {
        check(per_ray(reason), "a per-ray reason was not recognised");
        unsigned calls{};
        auto out = execute(request(3), 64, 1000, [&](const Ray&, unsigned) {
            if (++calls == 2) return Probe{{}, 1, false, reason};
            return Probe{{Status::hit, {1, 0, 3}, .5f}, 1};
        }, fence);
        check(out.failure == reason && out.native_casts == 2 && calls == 2 && !out.budget_exhausted,
            "a per-ray reason was lost or relabelled");
        empty(out);
        // Exhaustion keeps its own mapping, whatever the probe names.
        out = execute(request(), 64, 1000, [&](const Ray&, unsigned allowance) { return Probe{{}, allowance, true, reason}; }, fence);
        check(out.failure == Failure::ray_limit && out.budget_exhausted && out.native_casts == 2, "a reason outranked the ray limit");
        empty(out);
        out = execute(request(), 1, 1000, [&](const Ray&, unsigned allowance) { return Probe{{}, allowance, true, reason}; }, fence);
        check(out.failure == Failure::budget && out.budget_exhausted && out.native_casts == 1, "a reason outranked the shared budget");
        empty(out);
        // A refused hit needs the actual cast that found it.
        out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { return Probe{{}, 0, false, reason}; }, fence);
        check(out.failure == Failure::backend && !out.native_casts, "a castless per-ray reason was accepted");
        empty(out);
        // A reason belongs only on a refused ray.
        for (const auto status : {Status::hit, Status::clear}) {
            out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { return Probe{{status, {1, 0, 3}, .5f}, 1, false, reason}; }, fence);
            check(out.failure == Failure::backend && out.native_casts == 1, "a reason rode an accepted row");
            empty(out);
        }
        unsigned fences{};
        out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { return Probe{{}, 1, false, reason}; }, [&] { return ++fences == 1; });
        check(out.failure == Failure::observation_changed && fences == 2, "a per-ray reason hid a changed observation");
        empty(out);
    }
    // No other reason can ride a probe.
    for (const auto reason : {Failure::invalid, Failure::unavailable, Failure::observation_changed, Failure::budget, Failure::ray_limit,
             Failure::local_actor_exclusion_unavailable, Failure::contact_measurement_unavailable,
             Failure::named_actor_categories_unavailable, Failure::hit_categories_unavailable,
             Failure::hit_categories_inconsistent, Failure::hit_named_actor_category}) {
        check(!per_ray(reason), "a batch reason was treated as per ray");
        const auto out = execute(request(), 64, 1000, [&](const Ray&, unsigned) { return Probe{{}, 1, false, reason}; }, fence);
        check(out.failure == Failure::backend && out.native_casts == 1, "a probe forged a batch reason");
        empty(out);
    }
    // Without a reason the generic mapping is unchanged.
    auto out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{{}, 1}; }, fence);
    check(out.failure == Failure::unavailable && out.native_casts == 1, "the generic after-cast reason changed");
    empty(out);
    out = execute(request(), 64, 1000, [](const Ray&, unsigned) { return Probe{}; }, fence);
    check(out.failure == Failure::query_preconditions_unavailable && !out.native_casts, "the generic zero-cast reason changed");
    empty(out);
}
// Exclusion facts are the backend's. The core never sets one, a request flag
// alone produces none, and every refusal, a failed closing fence included,
// clears them with the rows while keeping the actual cast count.
void exclusion_facts_are_backend_results() {
    check(!Result{}.local_actor_excluded && !Result{}.named_actor_categories_absent, "a default result claims a fact");
    auto r = request(2);
    r.exclude_local_actor = r.require_named_actor_categories_absent = true;
    const auto hit = [](const Ray&, unsigned) { return Probe{{Status::hit, {1, 0, 3}, .5f}, 1}; };
    auto out = execute(r, 64, 1000, hit, [] { return true; });
    check(out.available && !out.local_actor_excluded && !out.named_actor_categories_absent, "a request flag became a fact");
    unsigned fences{};
    out = execute(r, 64, 1000, hit, [&] { return ++fences < 2; });
    check(out.failure == Failure::observation_changed && out.native_casts == 2 && fences == 2 &&
        !out.local_actor_excluded && !out.named_actor_categories_absent, "a failed closing fence kept a fact or lost its casts");
    empty(out);
    Result claimed;
    claimed.available = true;
    claimed.count = 2;
    claimed.native_casts = 3;
    claimed.failure = Failure::none;
    claimed.local_actor_excluded = claimed.named_actor_categories_absent = true;
    claimed.rays[0] = {Status::hit, {1, 0, 3}, .5f};
    claimed.rays[0].in_categories_available = claimed.rays[0].owner_available = claimed.rays[0].owner_assigned = true;
    refuse(claimed, Failure::hit_named_actor_category);
    check(claimed.failure == Failure::hit_named_actor_category && claimed.native_casts == 3 && claimed.count == 2 &&
        !claimed.budget_exhausted && !claimed.local_actor_excluded && !claimed.named_actor_categories_absent,
        "a refusal kept a fact or lost its casts");
    empty(claimed);
    for (const auto& row : claimed.rays) check(!row.in_categories_available && !row.owner_available && !row.owner_assigned, "a refusal kept a value");
    refuse(claimed, Failure::ray_limit);
    check(claimed.budget_exhausted && claimed.failure == Failure::ray_limit, "a refusal lost its exhaustion flag");
}
// Owner slots follow the category rules: only a hit row may hold one, an
// assigned slot needs its value, and a probe never carries one.
void owner_slots_attach_only_to_hits() {
    check(!Request{}.include_hit_owner && !Request{}.require_named_actor_categories_absent && !Hit{}.owner_available &&
        !Hit{}.owner_assigned, "owner slots or the category requirement are not opt-in");
    const auto base = request();
    Hit row{Status::clear};
    row.owner_available = true;
    check(!consistent(base.rays[0], row), "a clear row carried an owner slot");
    row = {Status::clear};
    row.owner_assigned = true;
    check(!consistent(base.rays[0], row), "a clear row claimed an assigned owner");
    row = {Status::hit, {1, 0, 3}, .5f};
    row.owner_assigned = true;
    check(!consistent(base.rays[0], row), "an assigned owner without its slot was accepted");
    row.owner_available = true;
    row.owner = 0x2a;
    check(consistent(base.rays[0], row), "a hit row cannot hold its owner slot");
    row.owner_assigned = false;
    row.owner = 0xffffffffU;
    check(consistent(base.rays[0], row), "a hit row cannot hold an unassigned slot");
    for (const auto status : {Status::hit, Status::clear}) {
        auto selected = request(2);
        selected.include_hit_owner = true;
        auto out = execute(selected, 64, 1000, [&](const Ray&, unsigned) {
            Probe probe{{status, {1, 0, 3}, .5f}, 1};
            probe.hit.owner_available = true;
            return probe;
        }, [] { return true; });
        check(out.failure == Failure::backend && out.native_casts == 1, "a probe that carried an owner slot was accepted");
        empty(out);
    }
}

void identity_address_ranges_refuse_overflow_before_callbacks() {
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    unsigned reads{}, callbacks{};
    std::uint32_t value{};
    const auto permissive = [&](std::uintptr_t, auto& out) { ++reads; out = 7; return true; };
    check(!identity::element(permissive, 0, 0, value) && !reads, "null array reached the reader");
    check(!identity::element(permissive, maximum - 7, 2, value) && !reads, "element stride overflow reached the reader");
    check(!identity::element(permissive, maximum, 0, value) && !reads, "typed element end-span overflow reached the reader");
    check(identity::element(permissive, maximum - 3, 0, value) && reads == 1 && value == 7,
        "last representable complete element was rejected by range arithmetic");
    for (const auto bases : {std::array<std::uintptr_t, 2>{maximum - 0x680 + 1, HitFixture::state},
             std::array<std::uintptr_t, 2>{maximum - 0x680, HitFixture::state},
             std::array<std::uintptr_t, 2>{HitFixture::world, maximum - 0xff8 + 1},
             std::array<std::uintptr_t, 2>{HitFixture::world, maximum - 0xff8}}) {
        reads = callbacks = 0;
        NativeHitHandle body{11, 12, 13};
        const auto read = [&](std::uintptr_t, auto&) { ++reads; return true; };
        check(!identity::resolve(bases[0], bases[1], NativeHitHandle{bases[0], 9, 4}, body, read,
            [&] { ++callbacks; return true; }, [&](const auto&) { ++callbacks; return true; },
            [&](std::uint32_t) { ++callbacks; return true; }, [&](const auto&) { ++callbacks; return true; }) &&
            body == NativeHitHandle{} && !reads && !callbacks, "world/state field span overflow reached callbacks or reads");
    }
}
}
int main() {
    try {
        bounds_and_backend_geometry();
        counts_and_discard_are_bounded();
        every_observation_fence_invalidates_the_whole_batch();
        one_budget_charges_actual_retries();
        a_listener_epoch_does_not_freeze_canonical_body_state();
        typed_refusal_names_are_stable();
        query_presets_are_fixed_and_default_to_camera();
        hit_association_uses_shape_and_body_domains();
        unknown_hit_identity_stops_before_unsafe_body_reads();
        hit_association_rechecks_mapping_generations_and_array_heads();
        every_returned_shape_body_pair_reaches_the_final_batch_fence();
        identity_address_ranges_refuse_overflow_before_callbacks();
        shape_categories_attach_only_to_hits_after_the_closing_fence();
        per_ray_reasons_name_the_refused_hit();
        exclusion_facts_are_backend_results();
        owner_slots_attach_only_to_hits();
        std::cout << "body query tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
