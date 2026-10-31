#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/rigid_bodies.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dingosdk::body_hit_identity {
struct NativeHandle {
    std::uintptr_t world{};
    std::uint32_t index = 0xffffffffU, generation{};
    bool operator==(const NativeHandle&) const = default;
};
static_assert(sizeof(NativeHandle) == 16);
struct Witness { NativeHandle shape{}, body{}; };
struct ShapeState {
    std::uintptr_t allocations{}, generations{}, bodies{};
    std::uint8_t bits{}, generation{};
    std::uint32_t body{};
    bool operator==(const ShapeState&) const = default;
};
template<class Read, class T>
bool element(Read& read, std::uintptr_t array, std::uint32_t index, T& out) {
    constexpr auto last = std::numeric_limits<std::uintptr_t>::max() - (sizeof(T) - 1);
    if (!array || array > last || index > (last - array) / sizeof(T)) return false;
    return read(array + std::uintptr_t{index} * sizeof(T), out);
}
// This boundary accepts only an actual hit from the pinned native ray/result-to-
// shape reader. Native pool validation is not provenance for an arbitrary index.
// world_current guards the caller's bounded, current native world/state anchor;
// the checks below also refuse overflow of the complete field and element spans.
// It qualifies the current association; the ray does not supply a collision-time
// generation or a world read lock.
template<class Read, class WorldCurrent, class ShapeCurrent, class Allocated, class BodyCurrent>
bool resolve(std::uintptr_t world, std::uintptr_t state, const NativeHandle& shape, NativeHandle& body,
    Read&& read, WorldCurrent&& world_current, ShapeCurrent&& shape_current, Allocated&& allocated, BodyCurrent&& body_current) {
    namespace r = addr::rigid_bodies;
    body = {};
    constexpr auto last_pointer = std::numeric_limits<std::uintptr_t>::max() - (sizeof(std::uintptr_t) - 1);
    if (!world || !state || world > last_pointer - r::world_body_generations || state > last_pointer - r::shape_generations ||
        shape.world != world || shape.index == 0xffffffffU || shape.generation > 255 ||
        !world_current() || !shape_current(shape)) return false;
    const auto observe_shape = [&](ShapeState& out) {
        return read(state + r::shape_allocated_bits, out.allocations) &&
            element(read, out.allocations, shape.index >> 3, out.bits) && (out.bits & (1U << (shape.index & 7U))) &&
            read(state + r::shape_generations, out.generations) && element(read, out.generations, shape.index, out.generation) &&
            out.generation == shape.generation && read(state + r::shape_body_indices, out.bodies) &&
            element(read, out.bodies, shape.index, out.body) && out.body != 0xffffffffU;
    };
    ShapeState before{}, after{};
    if (!observe_shape(before) || !allocated(before.body)) return false;
    std::uintptr_t generations{}, again{};
    NativeHandle candidate{world, before.body};
    // The independent body capacity/allocation gate must precede the generation
    // element and the native body validator, which do not bound their own index.
    if (!read(world + r::world_body_generations, generations) || !element(read, generations, candidate.index, candidate.generation) ||
        !body_current(candidate) || !world_current() || !shape_current(shape) || !observe_shape(after) || before != after ||
        !allocated(candidate.index) || !read(world + r::world_body_generations, again) || again != generations) return false;
    std::uint32_t generation{};
    if (!element(read, again, candidate.index, generation) || generation != candidate.generation ||
        !body_current(candidate) || !world_current()) return false;
    body = candidate;
    return true;
}
template<std::size_t Capacity> struct Witnesses {
    std::array<Witness, Capacity> values{};
    std::size_t count{};
    bool add(const Witness& value) {
        if (count == values.size()) return false;
        values[count++] = value;
        return true;
    }
    template<class Current> bool current(Current&& check) const {
        // Different shapes can share a body. Every association must survive.
        for (std::size_t i = 0; i < count; ++i) if (!check(values[i])) return false;
        return true;
    }
};
} // namespace dingosdk::body_hit_identity
