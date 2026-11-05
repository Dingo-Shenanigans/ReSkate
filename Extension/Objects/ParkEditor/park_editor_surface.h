#pragma once
#include "Engine/Core/Platform/memory.h"
#include "editor_math.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/park_editor.h"
#include "Engine/Game/World/park_editor.h"
#include "Engine/Game/World/body_contact_read.h"
#include "Engine/Game/World/body_query.h"
#include <algorithm>
#include <optional>
#include <span>

namespace dingosdk::editor {
using SurfaceOwner = std::uint64_t (*)(std::uintptr_t, std::uint32_t, std::uintptr_t);
using SurfaceBody = std::array<std::uint64_t, 2>;
using SurfaceBodyReader = bool (*)(std::uintptr_t, std::uint32_t, std::uintptr_t, SurfaceBody &);
// addr::park_editor::surface_contracts[1] is the native camera ray query;
// the dropper's sphere query uses the same implementation and collision filter.
struct alignas(16) NativeSurfaceRay {
    std::array<float, 4> start{}, end{};
    std::uint32_t filter{addr::park_editor::camera_query_filter}, mode{};
    float tolerance{};
    std::uint32_t flags{};
    std::uintptr_t allocator_vtable{}, allocator{};
    std::array<std::uintptr_t, 5> ignored_bodies{};
    std::uint64_t padding{};
};
struct alignas(16) NativeSurfaceResult {
    std::uintptr_t world{}, data{}, allocator_vtable{}, allocator{};
    std::uint32_t first{}, count{};
    std::uint64_t padding{};
    std::array<std::uintptr_t, 4> scope{};
};
static_assert(offsetof(NativeSurfaceRay, filter) == 0x20 && offsetof(NativeSurfaceRay, ignored_bodies) == 0x40);
static_assert(offsetof(NativeSurfaceResult, scope) == 0x30 && sizeof(NativeSurfaceResult) == 0x50);
// Only the supported presets have a native filter. Anything else is not a
// request this build can make, so callers refuse it before any native call.
inline std::optional<std::uint32_t> surface_query_filter(body_query::QueryPreset preset) noexcept {
    switch (preset) {
    case body_query::QueryPreset::camera: return addr::park_editor::camera_query_filter;
    case body_query::QueryPreset::terrain: return addr::park_editor::terrain_query_filter;
    case body_query::QueryPreset::all: return addr::park_editor::all_query_filter;
    }
    return {};
}
struct NativeSurfaceApi {
    std::uintptr_t (*world)(std::uintptr_t){};
    void *(*ray)(std::uintptr_t, NativeSurfaceResult *, const NativeSurfaceRay *, const char *){};
    std::uintptr_t (*scope_allocator)(){};
    void (*release)(void *, std::uintptr_t){};
    bool ready{}, contact_ready{};
};
// What one cast found, for a caller that needs more than a position: an unavailable or
// malformed native result and a successful clear ray are told apart. This establishes that
// the query ran, not that collision for a whole region has streamed.
struct SurfaceCastProbe {
    bool available{}, hit{};
    Vec3 position{};
    float fraction{};
    SurfaceBody body{};
    Vec3 normal{};
    bool normal_available{};
};
inline NativeSurfaceApi surface_api(std::uintptr_t base, bool measure_contact = false) {
    const auto matches = [&](const auto &contract) {
        std::array<unsigned char, 32> bytes{};
        return memory::read_bytes(base + contract.rva, bytes.data(), bytes.size()) && bytes == contract.bytes;
    };
    const auto &surface_contracts = addr::park_editor::surface_contracts;
    NativeSurfaceApi api;
    api.ready = std::all_of(surface_contracts.begin(), surface_contracts.end(), matches);
    if (api.ready) {
        api.world = reinterpret_cast<decltype(api.world)>(base + surface_contracts[0].rva);
        api.ray = reinterpret_cast<decltype(api.ray)>(base + surface_contracts[1].rva);
        api.scope_allocator =
            reinterpret_cast<decltype(api.scope_allocator)>(base + surface_contracts[2].rva);
        api.release = reinterpret_cast<decltype(api.release)>(base + surface_contracts[3].rva);
    }
    // The contact readers are compared only for a caller that asks to measure contact.
    const auto &contact_contracts = addr::body_contact::contracts;
    api.contact_ready = measure_contact && api.ready &&
                        std::all_of(contact_contracts.begin(), contact_contracts.end(), matches);
    return api;
}
// What a caller adds to one cast. Without a probe the cast keeps the park editor's own rules,
// which its placement, picking, world rays and ground checks rely on: a malformed fraction is
// skipped and the request is not checked before the native call. probe, when given, makes it
// the counted cast instead: an unready API, a missing world, a non-finite or out-of-map endpoint
// and a zero-length segment are refused before the native call, a malformed fraction makes the
// whole result unknown, and the probe receives the typed result: the nearest hit's body as
// body_reader read it (read for that hit only, and kept only when the reader succeeds) and, with
// measure_contact, the hit's point and normal, copied while the native result is still alive.
// native_casts, when given, is counted just before the native call, so a call that fails or
// throws is counted too. The preset changes only the request's filter lane, and one with no
// native filter makes no call.
struct SurfaceCastOptions {
    SurfaceCastProbe *probe{};
    unsigned *native_casts{};
    bool measure_contact{};
    body_query::QueryPreset preset{body_query::QueryPreset::camera};
};
inline std::optional<Vec3> cast_surface_once(const NativeSurfaceApi &api, std::uintptr_t world, Vec3 start,
                                             Vec3 end, SurfaceOwner owner,
                                             std::span<const std::uint64_t> ignored, std::uint64_t *entity,
                                             std::span<const SurfaceBody> ignored_bodies,
                                             std::vector<SurfaceBody> &discovered,
                                             SurfaceBodyReader body_reader,
                                             const SurfaceCastOptions &options = {}) {
    const bool counted = options.probe != nullptr;
    if (counted) {
        *options.probe = {};
        if (!api.ready || !api.ray || !world)
            return {};
        for (const auto &point : {start, end})
            for (const float value : point)
                if (!std::isfinite(value) || std::abs(value) > 100000)
                    return {};
        if (dot(sub(end, start), sub(end, start)) < 1e-10f)
            return {};
    }
    const auto filter = surface_query_filter(options.preset);
    if (!filter)
        return {};
    NativeSurfaceRay request;
    // Mode, tolerance, flags and the allocator lanes stay as the camera ray sends them.
    request.filter = *filter;
    std::vector<SurfaceBody> bodies;
    for (const auto &body : ignored_bodies)
        if (body[0] == world)
            bodies.push_back(body);
    if (!bodies.empty()) {
        // The native ray implementation reads begin/end and each 16-byte body's index at +8.
        request.ignored_bodies[0] = reinterpret_cast<std::uintptr_t>(bodies.data());
        request.ignored_bodies[1] = reinterpret_cast<std::uintptr_t>(bodies.data() + bodies.size());
        request.ignored_bodies[2] = request.ignored_bodies[1];
    }
    std::copy(start.begin(), start.end(), request.start.begin());
    std::copy(end.begin(), end.end(), request.end.begin());
    NativeSurfaceResult result;
    // Before the call, so a call that unwinds still releases what it wrote. A call that returns
    // is released where it always was, when the cast returns.
    struct Release {
        const NativeSurfaceApi &api;
        NativeSurfaceResult &result;
        ~Release() {
            // Same result cleanup as the native ray callers. Pop the temporary
            // native allocation scope before releasing an explicit allocator.
            if (result.scope[2]) {
                const auto allocator = api.scope_allocator();
                std::uintptr_t vtable{}, function{};
                if (memory::read(allocator, vtable) && memory::read(vtable + 8, function) && function)
                    reinterpret_cast<void (*)(std::uintptr_t, void *)>(function)(allocator,
                                                                                 result.scope.data());
            }
            if (result.allocator)
                api.release(&result.allocator_vtable, result.data);
        }
    } release{api, result};
    if (options.native_casts)
        ++*options.native_casts;
    api.ray(world, &result, &request, "ReSkate_ParkSurface");
    if (result.world != world || !result.data || result.count > 4096 ||
        result.first > UINT32_MAX - result.count)
        return {};
    std::uintptr_t fractions{};
    if (!memory::read(result.data + 8, fractions))
        return {};
    // Supported-build ray wrapper 0x26afdc0 -> 0x2664c00: the implementation writes
    // world/data, first=0, count=number of hits (0x26651fb..0x2665216). An empty
    // query can have no fraction array. A zeroed/unowned result is not a clear ray.
    if (!result.count) {
        if (counted && !result.first)
            options.probe->available = true;
        return {};
    }
    if (!fractions)
        return {};
    float closest = 2;
    std::uint32_t closest_index{};
    for (std::uint32_t n = 0; n < result.count; ++n) {
        float fraction{};
        if (!memory::read(fractions + static_cast<std::uintptr_t>(result.first + n) * 4, fraction))
            return {};
        if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) {
            // The counted cast must not skip it: that could report a farther surface than the
            // one the ray met, or a clear ray.
            if (counted)
                return {};
            continue;
        }
        if (fraction < closest) {
            const auto hit_entity = owner ? owner(world, result.first + n, result.data) : 0;
            if (hit_entity && std::find(ignored.begin(), ignored.end(), hit_entity) != ignored.end()) {
                SurfaceBody body{};
                if (body_reader && body_reader(world, result.first + n, result.data, body) &&
                    body[0] == world)
                    discovered.push_back(body);
                continue;
            }
            closest = fraction;
            closest_index = result.first + n;
            if (entity)
                *entity = hit_entity;
        }
    }
    if (closest > 1)
        return {};
    const auto position = add(start, mul(sub(end, start), closest));
    if (counted) {
        auto &probe = *options.probe;
        probe.available = probe.hit = true;
        probe.position = position;
        probe.fraction = closest;
        SurfaceBody body{};
        if (body_reader && body_reader(world, closest_index, result.data, body))
            probe.body = body;
        body_contact_read::Vectors vectors;
        const auto read = [](std::uintptr_t at, auto &value) { return memory::read(at, value); };
        if (options.measure_contact && api.contact_ready &&
            body_contact_read::vectors(result.data, closest_index, position, start, end, vectors, read)) {
            probe.position = vectors.position;
            probe.normal = vectors.normal;
            probe.normal_available = true;
        }
    }
    return position;
}
// One counted cast, for the plugins' counted world queries (reskate.world.probe and
// world.physics.probe_batch). native_casts is 1 when the native ray was called, whatever it
// returned, and 0 when the request was refused first. No owner is asked about any hit, nothing
// is ignored and nothing is cast again: a caller may refuse the nearest hit, never cast through
// it. body_reader is asked only about the nearest hit.
struct CountedSurfaceCast {
    SurfaceCastProbe surface;
    unsigned native_casts{};
};
inline CountedSurfaceCast cast_surface_counted(const NativeSurfaceApi &api, std::uintptr_t world, Vec3 start,
                                               Vec3 end, SurfaceBodyReader body_reader = nullptr,
                                               bool measure_contact = false,
                                               body_query::QueryPreset preset = body_query::QueryPreset::camera) {
    CountedSurfaceCast out;
    std::vector<SurfaceBody> discovered; // stays empty: nothing is ignored
    try {
        cast_surface_once(api, world, start, end, nullptr, {}, nullptr, {}, discovered, body_reader,
                          {&out.surface, &out.native_casts, measure_contact, preset});
    } catch (...) {
        // A query that throws keeps its count and loses its result.
        out.surface = {};
    }
    return out;
}
inline std::optional<Vec3> cast_surface(const NativeSurfaceApi &api, std::uintptr_t world, Vec3 start,
                                        Vec3 end, SurfaceOwner owner = nullptr,
                                        std::span<const std::uint64_t> ignored = {},
                                        std::uint64_t *entity = nullptr,
                                        std::span<const SurfaceBody> ignored_bodies = {},
                                        SurfaceBodyReader body_reader = nullptr) {
    std::vector<SurfaceBody> bodies(ignored_bodies.begin(), ignored_bodies.end());
    // A closest-hit query may initially return only the selected object's body.
    // Learn that exact body from the hit, exclude it natively, then query again.
    // This also covers multiple collision children and newly replicated previews.
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        if (entity)
            *entity = 0;
        std::vector<SurfaceBody> discovered;
        if (const auto hit = cast_surface_once(api, world, start, end, owner, ignored, entity, bodies,
                                               discovered, body_reader))
            return hit;
        const auto count = bodies.size();
        for (const auto &body : discovered)
            if (std::find(bodies.begin(), bodies.end(), body) == bodies.end())
                bodies.push_back(body);
        if (bodies.size() == count || bodies.size() > 4096)
            break;
    }
    return {}; // Hold the preview when exclusions cannot establish a real surface.
}
inline EditorSurfaceHit probe_surface(const NativeSurfaceApi &api, std::uintptr_t context,
                                      const EditorSurfaceRequest &request, SurfaceOwner owner = nullptr,
                                      std::span<const std::uint64_t> ignored = {},
                                      std::span<const SurfaceBody> ignored_bodies = {},
                                      SurfaceBodyReader body_reader = nullptr) {
    EditorSurfaceHit result{request.generation, request.id};
    if (!api.ready || !context || !request.id || !std::isfinite(request.grid) || request.grid < 0 ||
        request.grid > 100)
        return result;
    for (float value : request.origin)
        if (!std::isfinite(value) || std::abs(value) > 100000)
            return result;
    const auto direction = normalized(request.direction);
    if (!std::isfinite(dot(direction, direction)) || dot(direction, direction) < .9f)
        return result;
    const auto world = api.world(context);
    if (!world)
        return result;
    result.available = true;
    auto hit = cast_surface(api, world, request.origin, add(request.origin, mul(direction, 500)), owner,
                            ignored, &result.entity, ignored_bodies, body_reader);
    if (hit && request.grid > 0) {
        const float x = snapped((*hit)[0], request.grid), z = snapped((*hit)[2], request.grid);
        // Snap horizontal coordinates, then query terrain again. Rounding Y
        // would float objects above slopes or bury them below the real ground.
        hit = cast_surface(api, world, {x, (*hit)[1] + 10, z}, {x, (*hit)[1] - 10, z}, owner, ignored,
                           nullptr, ignored_bodies, body_reader);
    }
    if (hit) {
        result.hit = true;
        result.position = *hit;
    }
    return result;
}
} // namespace dingosdk::editor
