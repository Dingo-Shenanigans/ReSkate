#pragma once
#include "owned_body.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace dingosdk::body_query {
using Vec = owned_body::Vec;
inline constexpr unsigned native_cast_limit = 64, casts_per_ray = 2;
inline constexpr std::size_t maximum_rays = 32, maximum_exclusions = owned_body::retained_capacity;
struct Ray { Vec from{}, to{}; };
enum class Status { unavailable, clear, hit };
enum class Failure {
    none, invalid, unavailable, observation_changed, budget, ray_limit, backend,
    client_phase_unavailable, local_actor_exclusion_unavailable,
    query_preconditions_unavailable, query_backend_exception,
    body_exclusions_unavailable, contact_measurement_unavailable,
    // Per ray: why one actual nearest hit was refused (Probe::failure).
    owned_body_hit, hit_identity_unqualified,
    // The named-actor-category requirement.
    named_actor_categories_unavailable, hit_categories_unavailable, hit_categories_inconsistent,
    hit_named_actor_category
};
inline const char* name(Failure value) noexcept {
    switch (value) {
    case Failure::none: return "";
    case Failure::invalid: return "invalid_ray_batch";
    case Failure::unavailable: return "query_unavailable";
    case Failure::observation_changed: return "body_epoch_or_observation_changed";
    case Failure::budget: return "native_cast_budget_exhausted";
    case Failure::ray_limit: return "ray_cast_limit_exhausted";
    case Failure::backend: return "invalid_query_backend_result";
    case Failure::client_phase_unavailable: return "client_phase_unavailable";
    case Failure::local_actor_exclusion_unavailable: return "local_actor_exclusion_unavailable";
    case Failure::query_preconditions_unavailable: return "query_preconditions_unavailable";
    case Failure::query_backend_exception: return "query_backend_exception";
    case Failure::body_exclusions_unavailable: return "body_exclusions_unavailable";
    case Failure::contact_measurement_unavailable: return "contact_measurement_unavailable";
    case Failure::owned_body_hit: return "owned_body_hit";
    case Failure::hit_identity_unqualified: return "hit_identity_unqualified";
    case Failure::named_actor_categories_unavailable: return "named_actor_categories_unavailable";
    case Failure::hit_categories_unavailable: return "hit_categories_unavailable";
    case Failure::hit_categories_inconsistent: return "hit_categories_inconsistent";
    case Failure::hit_named_actor_category: return "hit_named_actor_category";
    }
    return "query_unavailable";
}
// The reasons a probe may give for its one ray. Each names an actual nearest hit
// that was refused, so each needs that ray's actual cast. An identity lost while
// reading a hit's optional values has no reason here: that loss also fails the
// fence execute runs next, so it is reported as an observation change.
constexpr bool per_ray(Failure value) noexcept {
    return value == Failure::owned_body_hit || value == Failure::hit_identity_unqualified;
}
// A fixed native query filter, chosen by name. The Build layer owns each
// preset's native value. Choosing one selects what the native ray tests; it
// does not classify what a hit belongs to and excludes no actor. all is a
// diagnostic filter that tests every shape with a nonzero category.
enum class QueryPreset : std::uint8_t { camera, terrain, all };
inline const char* name(QueryPreset value) noexcept {
    switch (value) {
    case QueryPreset::camera: return "camera";
    case QueryPreset::terrain: return "terrain";
    case QueryPreset::all: return "all";
    }
    return nullptr; // Not a preset; callers refuse it.
}
struct Hit {
    Status status{};
    Vec position{};
    float fraction{};
    Vec normal{}, surface_velocity{};
    bool normal_available{}, surface_velocity_available{};
    // Opt-in: the hit shape's InCategories DWORD as read after the hit qualified.
    // Only a hit can carry it, and only once the batch's closing fence passed.
    // It says nothing about an actor, a floor or collision-time filtering.
    std::uint32_t in_categories{};
    bool in_categories_available{};
    // Opt-in: the hit shape's owner slot, read and fenced like in_categories.
    // owner_assigned is false for the build's unassigned slot. The slot itself
    // never leaves the SDK; a binding may only compare two of them. It says
    // nothing about whose owner it is.
    std::uint32_t owner{};
    bool owner_available{}, owner_assigned{};
};
struct Request {
    owned_body::Sample observation{};
    std::array<Ray, maximum_rays> rays{};
    std::size_t count{};
    std::array<owned_body::Handle, maximum_exclusions> excluded{};
    std::size_t exclusions{};
    bool exclude_local_actor{};
    bool require_contact{}; // Every hit needs qualified normal and point motion; clear stays clear.
    QueryPreset preset{QueryPreset::camera}; // Camera is the filter every earlier caller used.
    bool include_shape_categories{}; // False reads no category and adds no field.
    // Every hit's read-time categories must overlap the preset's filter and carry
    // no named actor category. A filter that has one is refused before any cast.
    // This is a check on the returned hits, not actor exclusion.
    bool require_named_actor_categories_absent{};
    bool include_hit_owner{}; // False reads no owner slot and adds no field.
};
// Compare each published numeric lane exactly, including signed zero. Padding is not state.
inline bool same_sample(const owned_body::Sample& a, const owned_body::Sample& b) noexcept {
    const auto same = [](float x, float y) { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); };
    if (a.epoch.world != b.epoch.world || a.epoch.body != b.epoch.body || a.epoch.serial != b.epoch.serial ||
        std::bit_cast<std::uint64_t>(a.epoch.time) != std::bit_cast<std::uint64_t>(b.epoch.time) || !same(a.epoch.seconds, b.epoch.seconds) ||
        !same(a.mass, b.mass)) return false;
    for (std::size_t i = 0; i < a.position.size(); ++i)
        if (!same(a.position[i], b.position[i]) || !same(a.velocity[i], b.velocity[i]) ||
            !same(a.angular_velocity[i], b.angular_velocity[i]) || !same(a.inertia[i], b.inertia[i])) return false;
    for (std::size_t i = 0; i < a.rotation.size(); ++i) if (!same(a.rotation[i], b.rotation[i])) return false;
    return true;
}
struct Result {
    std::array<Hit, maximum_rays> rays{};
    std::size_t count{};
    unsigned native_casts{};
    bool available{}, budget_exhausted{};
    bool accounting_valid{true};
    Failure failure{Failure::unavailable};
    // Facts only a backend sets, and only on an available result for a request
    // that asked for them. A binding reports these, never the request's flags.
    bool local_actor_excluded{}, named_actor_categories_absent{};
};
// Turn a result into a refusal: no row, value or fact survives, and the actual
// cast count stays. execute refuses this way, and so does an adapter's own check
// after execute's closing fence.
inline Result& refuse(Result& out, Failure failure) noexcept {
    out.rays = {};
    out.available = false;
    out.failure = failure;
    out.budget_exhausted = failure == Failure::budget || failure == Failure::ray_limit;
    out.local_actor_excluded = out.named_actor_categories_absent = false;
    return out;
}
inline bool valid(const Ray& ray, float maximum_distance) noexcept {
    if (!owned_body::finite(ray.from, 100000) || !owned_body::finite(ray.to, 100000) ||
        !std::isfinite(maximum_distance) || maximum_distance <= 0) return false;
    Vec delta{};
    for (std::size_t i = 0; i < delta.size(); ++i) delta[i] = ray.to[i] - ray.from[i];
    const double distance = owned_body::length(delta);
    return distance >= .00001 && distance <= maximum_distance;
}
inline bool valid(const Request& request, float maximum_distance) noexcept {
    const auto& sample = request.observation;
    const auto& epoch = sample.epoch;
    if (!request.count || request.count > maximum_rays || request.exclusions > maximum_exclusions || !name(request.preset) ||
        !epoch.world || !epoch.body || !epoch.serial || !std::isfinite(epoch.time) || epoch.time < 0 || !owned_body::calibrated(epoch.seconds) ||
        !owned_body::finite(sample.position, 1000000) || !owned_body::finite(sample.velocity, 100) || !owned_body::finite(sample.angular_velocity, 100) ||
        !owned_body::finite(sample.inertia, 1000000) || !std::isfinite(sample.mass) || sample.mass <= 0) return false;
    for (float value : sample.inertia) if (value <= 0) return false;
    double norm{};
    for (float value : sample.rotation) { if (!std::isfinite(value)) return false; norm += static_cast<double>(value) * value; }
    if (norm < .9 || norm > 1.1) return false;
    for (std::size_t i = 0; i < request.count; ++i) if (!valid(request.rays[i], maximum_distance)) return false;
    return true;
}
inline bool consistent(const Ray& ray, const Hit& hit) noexcept {
    // A clear ray hit no shape, so it has no category or owner either.
    if (hit.status == Status::clear) return !hit.normal_available && !hit.surface_velocity_available && !hit.in_categories_available &&
        !hit.owner_available && !hit.owner_assigned;
    if (hit.owner_assigned && !hit.owner_available) return false;
    if (hit.normal_available) {
        if (!owned_body::finite(hit.normal, 1.01f)) return false;
        double norm{};
        for (float lane : hit.normal) norm += double(lane) * lane;
        if (norm < .98 || norm > 1.02) return false;
        double facing{}, distance{};
        for (std::size_t i = 0; i < ray.from.size(); ++i) {
            const double delta = double(ray.to[i]) - ray.from[i];
            facing += delta * hit.normal[i];
            distance += delta * delta;
        }
        if (facing > 1e-4 * std::sqrt(distance)) return false;
    }
    if (hit.surface_velocity_available && !owned_body::finite(hit.surface_velocity, 100)) return false;
    if (hit.status != Status::hit || !std::isfinite(hit.fraction) || hit.fraction < 0 || hit.fraction > 1 ||
        !owned_body::finite(hit.position, 100000)) return false;
    for (std::size_t i = 0; i < ray.from.size(); ++i)
        if (std::abs(hit.position[i] - (ray.from[i] + (ray.to[i] - ray.from[i]) * hit.fraction)) > .025f) return false;
    return true;
}
// failure, on an unavailable probe, names why its actual hit was refused
// (per_ray). Budget exhaustion still outranks it.
struct Probe { Hit hit{}; unsigned native_casts{}; bool budget_exhausted{}; Failure failure{Failure::none}; };
// Query is the counted native adapter. Fence requalifies the same owned body,
// exclusions, actor and local observation epoch. A partial batch never escapes.
template<class Query, class Fence>
Result execute(const Request& request, unsigned allowance, float maximum_distance, Query&& query, Fence&& fence) {
    Result out;
    out.count = request.count <= maximum_rays ? request.count : 0;
    const auto fail = [&](Failure failure) { return refuse(out, failure); };
    if (!valid(request, maximum_distance)) return fail(Failure::invalid);
    if (allowance > native_cast_limit) allowance = native_cast_limit;
    if (!allowance) return fail(Failure::budget);
    if (!fence()) return fail(Failure::observation_changed);
    const auto finish_failure = [&](Failure failure) {
        return fail(fence() ? failure : Failure::observation_changed);
    };
    for (std::size_t i = 0; i < request.count; ++i) {
        const auto remaining = allowance - out.native_casts;
        if (!remaining) return finish_failure(Failure::budget);
        const auto permitted = remaining < casts_per_ray ? remaining : casts_per_ray;
        const auto probe = query(request.rays[i], permitted);
        if (probe.native_casts > permitted) {
            // An invalid adapter cannot authorize any more calls this tick.
            out.accounting_valid = false;
            return finish_failure(Failure::backend);
        }
        out.native_casts += probe.native_casts;
        if (probe.hit.status == Status::unavailable) {
            if (probe.budget_exhausted) return finish_failure(out.native_casts == allowance ? Failure::budget : Failure::ray_limit);
            // A named reason takes the generic one's place. Only a per-ray reason,
            // with the actual cast that found the refused hit, is kept.
            if (probe.failure != Failure::none)
                return finish_failure(per_ray(probe.failure) && probe.native_casts ? probe.failure : Failure::backend);
            return finish_failure(out.native_casts ? Failure::unavailable : Failure::query_preconditions_unavailable);
        }
        if (probe.failure != Failure::none || !probe.native_casts || !consistent(request.rays[i], probe.hit))
            return finish_failure(Failure::backend);
        // A probe never carries a category or owner. The adapter copies them in only
        // after the closing fence below, so a later downgrade cannot leave a stale value.
        if (probe.hit.in_categories_available || probe.hit.owner_available) return finish_failure(Failure::backend);
        if (request.require_contact && probe.hit.status == Status::hit &&
            (!probe.hit.normal_available || !probe.hit.surface_velocity_available))
            return finish_failure(Failure::contact_measurement_unavailable);
        out.rays[i] = probe.hit;
    }
    if (!fence()) return fail(Failure::observation_changed);
    out.available = true;
    out.failure = Failure::none;
    return out;
}
// One instance belongs to a VM tick. Every backend receives only remaining(),
// and reports actual native calls even when its query is unavailable.
struct Budget {
    unsigned used{};
    void poison() noexcept { used = native_cast_limit; }
    unsigned remaining(unsigned configured = native_cast_limit) const noexcept {
        const auto limit = configured < native_cast_limit ? configured : native_cast_limit;
        return used < limit ? limit - used : 0;
    }
    bool consume(unsigned actual, unsigned allowed) noexcept {
        if (actual > allowed || actual > remaining()) { used = native_cast_limit; return false; }
        used += actual;
        return true;
    }
};
} // namespace dingosdk::body_query
