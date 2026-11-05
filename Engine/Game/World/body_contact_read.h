#pragma once
#include "body_hit_identity.h"
#include "owned_body.h"
#include "Engine/Game/Build/20260929/body_contact.h"
#include <bit>
#include <cmath>

namespace dingosdk::body_contact_read {
using Vec = owned_body::Vec;
using V4 = std::array<float, 4>;
using NativeHandle = body_hit_identity::NativeHandle;
struct Ring {
    std::uintptr_t values{};
    std::uint32_t elements{};
    std::uint16_t current{}, planes{};
    bool operator==(const Ring&) const = default;
};
static_assert(sizeof(Ring) == 16);
struct Motion {
    NativeHandle body{};
    std::array<Ring, 3> rings{};
    std::array<V4, 3> values{};
    Vec point{}, velocity{};
};
inline bool same(const V4& a, const V4& b) noexcept {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i])) return false;
    return true;
}
inline bool field_span(std::uintptr_t base, std::uintptr_t offset, std::size_t size) noexcept {
    return base && size && base <= UINTPTR_MAX - (size - 1) && offset <= UINTPTR_MAX - (size - 1) - base;
}
template<class Read>
bool ring_value(Read& read, std::uintptr_t state, std::uintptr_t field, std::uint32_t index, Ring& ring, V4& value) {
    if (!field_span(state, field, sizeof(Ring)) || !read(state + field, ring) || !ring.values ||
        !ring.elements || ring.elements > 1048576 || index >= ring.elements ||
        !ring.planes || ring.planes > 64 || ring.current >= ring.planes) return false;
    // A plane is elements 16-byte lanes padded to 64 bytes, as the game sizes it. The game does
    // this in 32 bits with the count masked to 27 bits; within the bound above both agree.
    const auto stride = ((std::uint64_t{ring.elements} * 16 + 60) & ~std::uint64_t{63});
    const auto displacement = stride * ring.current + std::uint64_t{index} * sizeof(V4);
    if (displacement > UINTPTR_MAX || !field_span(ring.values, static_cast<std::uintptr_t>(displacement), sizeof(V4)) ||
        !read(ring.values + static_cast<std::uintptr_t>(displacement), value)) return false;
    for (float lane : value) if (!std::isfinite(lane) || std::abs(lane) > 1000000) return false;
    return true;
}
// Current must qualify the same current world/state and allocated body generation.
// It must not accept an arbitrary numeric index as trusted native-hit provenance.
// These are repeated endpoint reads, not a native allocation pin or a world mutex.
template<class Read, class Current>
bool motion(std::uintptr_t world, std::uintptr_t state, NativeHandle body, const Vec& point,
    Motion& out, Read&& read, Current&& current) {
    namespace r = addr::rigid_bodies;
    out = {};
    if (!world || body.world != world || body.index == UINT32_MAX || !owned_body::finite(point, 100000) ||
        !field_span(state, r::spin_ring, sizeof(Ring)) || !current(body)) return false;
    constexpr std::array<std::uintptr_t, 3> offsets{r::position_ring, r::velocity_ring, r::spin_ring};
    Motion candidate;
    candidate.body = body;
    candidate.point = point;
    for (std::size_t i = 0; i < offsets.size(); ++i)
        if (!ring_value(read, state, offsets[i], body.index, candidate.rings[i], candidate.values[i])) return false;
    if (!current(body)) return false;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        Ring header;
        V4 value;
        if (!ring_value(read, state, offsets[i], body.index, header, value) || header != candidate.rings[i] ||
            !same(value, candidate.values[i])) return false;
    }
    if (!current(body)) return false;
    const auto& p = candidate.values[0];
    const auto& v = candidate.values[1];
    const auto& w = candidate.values[2];
    const std::array<double, 3> arm{double(point[0]) - p[0], double(point[1]) - p[1], double(point[2]) - p[2]};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        const double lane = double(v[i]) + double(w[j]) * arm[k] - double(w[k]) * arm[j];
        if (!std::isfinite(lane) || std::abs(lane) > 100) return false;
        candidate.velocity[i] = static_cast<float>(lane);
    }
    out = candidate;
    return true;
}
template<class Read, class Current>
bool motion_current(std::uintptr_t world, std::uintptr_t state, const Motion& value, Read&& read, Current&& current) {
    Motion again;
    if (!motion(world, state, value.body, value.point, again, read, current) || again.rings != value.rings) return false;
    for (std::size_t i = 0; i < again.values.size(); ++i) if (!same(again.values[i], value.values[i])) return false;
    return true;
}
struct Vectors { Vec position{}, normal{}; };
// Borrowed result arrays are read only while the native result's Release scope
// is alive. The caller supplies the actual native first+ordinal and closest ray.
template<class Read>
bool vectors(std::uintptr_t data, std::uint32_t ordinal, const Vec& expected, const Vec& from,
    const Vec& to, Vectors& out, Read&& read) {
    namespace c = addr::body_contact;
    out = {};
    if (!field_span(data, c::result_normals, sizeof(std::uintptr_t)) || !owned_body::finite(expected, 100000) ||
        !owned_body::finite(from, 100000) || !owned_body::finite(to, 100000)) return false;
    std::uintptr_t positions{}, normals{}, positions_after{}, normals_after{};
    V4 p{}, n{}, pa{}, na{};
    if (!read(data + c::result_positions, positions) || !read(data + c::result_normals, normals) ||
        !body_hit_identity::element(read, positions, ordinal, p) || !body_hit_identity::element(read, normals, ordinal, n) ||
        !read(data + c::result_positions, positions_after) || positions_after != positions ||
        !read(data + c::result_normals, normals_after) || normals_after != normals ||
        !body_hit_identity::element(read, positions, ordinal, pa) || !body_hit_identity::element(read, normals, ordinal, na) ||
        !same(p, pa) || !same(n, na)) return false;
    double norm{}, residual{}, facing{}, distance{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(p[i]) || !std::isfinite(n[i])) return false;
        const double delta = double(to[i]) - from[i];
        distance += delta * delta;
        norm += double(n[i]) * n[i];
        facing += delta * n[i];
        const double error = double(p[i]) - expected[i];
        residual += error * error;
    }
    // Refuse malformed/ambiguous orientation; never synthesize or flip a normal.
    if (distance < 1e-10 || norm < .98 || norm > 1.02 || residual > .0004 || facing > 1e-4 * std::sqrt(distance)) return false;
    out = {{p[0], p[1], p[2]}, {n[0], n[1], n[2]}};
    return true;
}
} // namespace dingosdk::body_contact_read
