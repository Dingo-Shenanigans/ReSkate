#include "Engine/Game/World/body_contact_read.h"
#include "Engine/Game/World/body_query.h"
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace dingosdk;
using namespace body_contact_read;
namespace {
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
struct Memory {
    std::map<std::uintptr_t, unsigned char> bytes;
    unsigned reads{};
    std::function<void(std::uintptr_t)> after;
    template<class T> void write(std::uintptr_t at, const T& value) {
        std::array<unsigned char, sizeof(T)> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < raw.size(); ++i) bytes[at + i] = raw[i];
    }
    template<class T> bool read(std::uintptr_t at, T& out) {
        ++reads;
        std::array<unsigned char, sizeof(T)> raw{};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const auto p = bytes.find(at + i);
            if (p == bytes.end()) return false;
            raw[i] = p->second;
        }
        std::memcpy(&out, raw.data(), sizeof(T));
        if (after) after(at);
        return true;
    }
};
struct Fixture {
    Memory memory;
    static constexpr std::uintptr_t world = 0x100000, state = 0x110000;
    NativeHandle body{world, 0, 0};
    std::array<Ring, 3> rings{{{0x200000, 4, 1, 2}, {0x210000, 4, 1, 2}, {0x220000, 4, 1, 2}}};
    std::array<V4, 3> values{{{10, 20, 30, 1}, {1, 2, 3, 0}, {0, 2, 0, 0}}};
    Fixture() {
        memory.write(world + 0x28, state);
        memory.write(world + 0x680, std::uintptr_t{0x300000});
        memory.write(0x300000, std::uint32_t{0});
        memory.write(state + 0x9c8, std::uintptr_t{0x310000});
        memory.write(0x310000, std::uint32_t{1});
        constexpr std::array<std::uintptr_t, 3> offsets{0x900, 0x930, 0x940};
        for (std::size_t i = 0; i < 3; ++i) { memory.write(state + offsets[i], rings[i]); memory.write(rings[i].values + 64, values[i]); }
    }
    bool current(const NativeHandle& selected) {
        std::uintptr_t actual{}, generations{}, bits{};
        std::uint32_t capacity{}, allocation{}, generation{};
        return selected.world == world && memory.read(world + 0x28, actual) && actual == state &&
            memory.read(state + 0x908, capacity) && selected.index < capacity &&
            memory.read(state + 0x9c8, bits) && memory.read(bits + std::uintptr_t{selected.index >> 5} * 4, allocation) &&
            (allocation & (1U << (selected.index & 31U))) && memory.read(world + 0x680, generations) &&
            memory.read(generations + std::uintptr_t{selected.index} * 4, generation) && generation == selected.generation;
    }
    bool sample(Motion& out, Vec point = {11, 20, 30}) {
        return motion(world, state, body, point, out, [&](auto at, auto& value) { return memory.read(at, value); },
            [&](const NativeHandle& selected) { return current(selected); });
    }
};
void moving_surface_math_and_witness() {
    Fixture f;
    Motion measured;
    check(f.sample(measured), "current allocated body0/gen0 point sample rejected");
    check(measured.velocity == Vec{1, 2, 1}, "point motion must include world omega cross arm with correct sign");
    check(measured.body == f.body && measured.point == Vec{11, 20, 30}, "motion witness lost original body/point");
    check(motion_current(f.world, f.state, measured, [&](auto at, auto& value) { return f.memory.read(at, value); },
        [&](const NativeHandle& selected) { return f.current(selected); }), "unchanged witness refused");
    // No dynamic/fixed inference: directly measured keyframed-style motion stays nonzero.
    f.memory.write(0x210000 + 64, V4{4, 0, 0, 0});
    check(!motion_current(f.world, f.state, measured, [&](auto at, auto& value) { return f.memory.read(at, value); },
        [&](const NativeHandle& selected) { return f.current(selected); }), "changed motion escaped final witness");
    check(f.sample(measured) && measured.velocity == Vec{4, 0, -2}, "new actual linear motion was replaced with static zero");
}
void native_identity_and_ring_refusals() {
    for (unsigned variant = 0; variant < 13; ++variant) {
        Fixture f;
        Motion measured;
        switch (variant) {
        case 0: f.body.world++; break;
        case 1: f.body.generation++; break;
        case 2: f.memory.write(0x300000, std::uint32_t{1}); break;
        case 3: f.memory.write(0x310000, std::uint32_t{0}); break;
        case 4: f.memory.write(f.world + 0x28, std::uintptr_t{0x110001}); break;
        case 5: f.memory.write(f.state + 0x938, std::uint32_t{0}); break;
        case 6: f.memory.write(f.state + 0x938, std::uint32_t{1048577}); break;
        case 7: f.memory.write(f.state + 0x93c, std::uint16_t{2}); break;
        case 8: f.memory.write(f.state + 0x93e, std::uint16_t{0}); break;
        case 9: f.memory.write(f.state + 0x930, UINTPTR_MAX - std::uintptr_t{4}); break;
        case 10: f.memory.write(0x210000 + 64, V4{NAN, 0, 0, 0}); break;
        case 11: f.memory.bytes.erase(0x210000 + 64); break;
        case 12: f.body.index = 4; break;
        }
        check(!f.sample(measured), "wrong body/world/allocation/ring sample accepted");
        check(measured.body.world == 0 && measured.velocity == Vec{}, "refusal leaked partial motion");
    }
    Fixture f;
    Motion out;
    const auto before = f.memory.reads;
    check(!motion(f.world, UINTPTR_MAX - 8, f.body, Vec{}, out,
        [&](auto at, auto& value) { return f.memory.read(at, value); }, [&](const NativeHandle& selected) { return f.current(selected); }),
        "state fieldspan overflow accepted");
    check(f.memory.reads == before, "overflow performed a callback/read");
}
void closing_changes_and_signed_zero() {
    for (unsigned variant = 0; variant < 6; ++variant) {
        Fixture f;
        Motion out;
        bool changed{};
        f.memory.after = [&](std::uintptr_t at) {
            if (changed || at != 0x220000 + 64) return;
            changed = true;
            if (variant == 0) f.memory.write(f.state + 0x930, std::uintptr_t{0x210100});
            if (variant == 1) f.memory.write(f.state + 0x93c, std::uint16_t{0});
            if (variant == 2) f.memory.write(0x200000 + 64, V4{10, 20, 31, 1});
            if (variant == 3) f.memory.write(0x300000, std::uint32_t{1});
            if (variant == 4) f.memory.write(f.world + 0x28, std::uintptr_t{0});
            if (variant == 5) f.memory.write(0x220000 + 64, V4{-0.0f, 2, 0, 0});
        };
        check(!f.sample(out) && changed, "closing header/value/generation/world/zero-bit change accepted");
        check(out.body.world == 0, "closing failure published partial witness");
    }
}
void borrowed_vectors_not_guessed() {
    Memory m;
    const std::uintptr_t data = 0x400000, points = 0x410000, normals = 0x420000;
    m.write(data + 0x18, points);
    m.write(data + 0x20, normals);
    m.write(points + 16, V4{0, 3, 0, 1});
    m.write(normals + 16, V4{0, 1, 0, 0});
    const auto read = [&](auto at, auto& value) { return m.read(at, value); };
    Vectors v;
    check(vectors(data, 1, {0, 3, 0}, {0, 4, 0}, {0, 0, 0}, v, read) && v.normal == Vec{0, 1, 0}, "actual ordinal vector rejected");
    for (const auto invalid : {V4{0, 0, 0, 0}, V4{0, -1, 0, 0}, V4{0, 2, 0, 0}, V4{0, NAN, 0, 0}}) {
        m.write(normals + 16, invalid);
        check(!vectors(data, 1, {0, 3, 0}, {0, 4, 0}, {0, 0, 0}, v, read), "invalid normal was invented/normalized/flipped");
    }
    m.write(normals + 16, V4{0, 1, 0, 0});
    check(!vectors(data, 1, {0, 2, 0}, {0, 4, 0}, {0, 0, 0}, v, read), "inconsistent hitpoint accepted");
    check(!vectors(data, 2, {0, 3, 0}, {0, 4, 0}, {0, 0, 0}, v, read), "missing actual ordinal became a nearby vector");
    m.write(data + 0x20, UINTPTR_MAX - std::uintptr_t{2});
    check(!vectors(data, 1, {0, 3, 0}, {0, 4, 0}, {0, 0, 0}, v, read), "vector element overflow accepted");
}
void batch_optional_details_and_final_fence() {
    body_query::Request request;
    request.count = 1;
    request.require_contact = true;
    request.observation.epoch = {1, 1, 1, 1, static_cast<float>(owned_body::native_seconds)};
    request.observation.mass = 10;
    request.observation.inertia = {1, 1, 1};
    request.observation.rotation = {0, 0, 0, 1};
    request.rays[0] = {{0, 4, 0}, {0, 0, 0}};
    auto query = [](const body_query::Ray&, unsigned) {
        body_query::Probe p;
        p.native_casts = 1;
        p.hit.status = body_query::Status::hit;
        p.hit.position = {0, 3, 0};
        p.hit.fraction = .25f;
        return p;
    };
    auto r = body_query::execute(request, 2, 10, query, [] { return true; });
    check(!r.available && r.failure == body_query::Failure::contact_measurement_unavailable && r.native_casts == 1,
        "required unknown details became a contact or lost actual cast count");
    check(r.rays[0].status == body_query::Status::unavailable, "failed contact batch leaked geometry");
    request.require_contact = false;
    r = body_query::execute(request, 2, 10, query, [] { return true; });
    check(r.available && !r.rays[0].normal_available && !r.rays[0].surface_velocity_available, "optional unknown details broke geometry");
    request.require_contact = true;
    auto measured = [&](const body_query::Ray& ray, unsigned budget) {
        auto p = query(ray, budget);
        p.hit.normal = {0, 1, 0};
        p.hit.surface_velocity = {2, 0, -1};
        p.hit.normal_available = p.hit.surface_velocity_available = true;
        return p;
    };
    r = body_query::execute(request, 2, 10, measured, [] { return true; });
    check(r.available && r.rays[0].surface_velocity == Vec{2, 0, -1}, "measured moving contact lost velocity");
    auto backfacing = [&](const body_query::Ray& ray, unsigned budget) { auto p = measured(ray, budget); p.hit.normal = {0, -1, 0}; return p; };
    r = body_query::execute(request, 2, 10, backfacing, [] { return true; });
    check(!r.available && r.failure == body_query::Failure::backend, "backfacing backend normal accepted");
    unsigned fences{};
    r = body_query::execute(request, 2, 10, measured, [&] { return ++fences < 2; });
    check(!r.available && r.rays[0].status == body_query::Status::unavailable && r.native_casts == 1, "final fence failure published contact");
}
}
int main() {
    try {
        moving_surface_math_and_witness();
        native_identity_and_ring_refusals();
        closing_changes_and_signed_zero();
        borrowed_vectors_not_guessed();
        batch_optional_details_and_final_fence();
        std::cout << "Body contact read: 5 cases, " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
