#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace dingosdk::editor;
using dingosdk::body_query::QueryPreset;
int failures{};
unsigned assertions{};
void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}
// A constructed native ray: two hits at fractions 0.8 and 0.25 of the segment, or one of the
// malformed results a real call must never turn into a clear ray.
unsigned calls{}, releases{}, scopes{};
enum class Scenario { hit, clear, null_empty_array, zeroed, wrong_world, bad_count, bad_fraction, bad_first };
Scenario scenario{};
std::array<std::uintptr_t, 2> data{};
std::array<float, 2> fractions{0.8f, 0.25f};
void pop_scope(std::uintptr_t, void*) { ++scopes; }
std::array<std::uintptr_t, 2> allocator_table{0, reinterpret_cast<std::uintptr_t>(&pop_scope)};
std::uintptr_t allocator = reinterpret_cast<std::uintptr_t>(allocator_table.data());
NativeSurfaceApi api{
    [](std::uintptr_t) -> std::uintptr_t { return 1234; },
    [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay*, const char*) -> void* {
        ++calls;
        if (scenario == Scenario::zeroed) return out;
        out->world = scenario == Scenario::wrong_world ? world + 1 : world;
        out->data = reinterpret_cast<std::uintptr_t>(data.data());
        out->allocator = 1;
        out->scope[2] = 1;
        out->count = scenario == Scenario::hit || scenario == Scenario::bad_fraction ? 2U : 0U;
        out->first = scenario == Scenario::bad_first ? 1U : 0U;
        if (scenario == Scenario::bad_count) out->count = 9999;
        fractions = {0.8f, scenario == Scenario::bad_fraction ? std::numeric_limits<float>::quiet_NaN() : 0.25f};
        data[1] = scenario == Scenario::null_empty_array ? 0 : reinterpret_cast<std::uintptr_t>(fractions.data());
        return out;
    },
    []() -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(&allocator); },
    [](void*, std::uintptr_t) { ++releases; }, true};
// The park editor's own ray: the nearest hit is the selected object's body (42) until the
// request ignores that body, and then a surface behind it (43).
bool exclusion_broken{}, currently_self{};
NativeSurfaceApi excluding_api{
    nullptr,
    [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay* ray, const char*) -> void* {
        const auto* begin = reinterpret_cast<const SurfaceBody*>(ray->ignored_bodies[0]);
        const auto* end = reinterpret_cast<const SurfaceBody*>(ray->ignored_bodies[1]);
        currently_self = exclusion_broken || !begin || std::find(begin, end, SurfaceBody{world, 42}) == end;
        static float fraction;
        fraction = currently_self ? 0.0f : 0.75f;
        static std::array<std::uintptr_t, 2> values{};
        values[1] = reinterpret_cast<std::uintptr_t>(&fraction);
        out->world = world;
        out->data = reinterpret_cast<std::uintptr_t>(values.data());
        out->count = 1;
        return out;
    },
    nullptr, nullptr, true};
const SurfaceOwner hit_owner = [](std::uintptr_t, std::uint32_t, std::uintptr_t) -> std::uint64_t {
    return currently_self ? 99 : 0;
};
const SurfaceBodyReader hit_body = [](std::uintptr_t world, std::uint32_t, std::uintptr_t, SurfaceBody& out) {
    out = {world, currently_self ? std::uint64_t{42} : std::uint64_t{43}};
    return true;
};
}

int main() {
    const Vec3 from{0, 4, 0}, to{0, 0, 0};

    // The counted cast tells a hit, a clear ray and a malformed result apart.
    scenario = Scenario::hit;
    auto counted = cast_surface_counted(api, 1234, from, to);
    check(counted.surface.available && counted.surface.hit && counted.surface.fraction == 0.25f &&
              counted.surface.position == Vec3{0, 3, 0} && counted.native_casts == 1,
          "the counted ray selects the closest fraction and reports its world position and one cast");
    scenario = Scenario::clear;
    counted = cast_surface_counted(api, 1234, from, to);
    check(counted.surface.available && !counted.surface.hit && counted.native_casts == 1,
          "a matching-world first 0, count 0 result is a successful clear query");
    scenario = Scenario::null_empty_array;
    counted = cast_surface_counted(api, 1234, from, to);
    check(counted.surface.available && !counted.surface.hit, "an empty native query does not need a fraction array");
    for (const auto malformed : {Scenario::zeroed, Scenario::wrong_world, Scenario::bad_count, Scenario::bad_fraction,
                                 Scenario::bad_first}) {
        scenario = malformed;
        counted = cast_surface_counted(api, 1234, from, to);
        check(!counted.surface.available && !counted.surface.hit && counted.native_casts == 1,
              "an unowned or malformed native result is counted and never becomes a clear ray");
    }
    check(releases == calls - 1 && scopes == releases, "hit, clear and malformed results all release native allocation state");

    // Refused requests make no native call and count none.
    const auto before = calls;
    auto unavailable = api;
    unavailable.ready = false;
    check(!cast_surface_counted(unavailable, 1234, from, to).surface.available, "unverified native contracts do not execute a ray");
    check(!cast_surface_counted(api, 0, from, to).surface.available, "a missing physics world is unavailable");
    check(!cast_surface_counted(api, 1234, from, from).native_casts, "a zero-length ray makes no native call");
    check(!cast_surface_counted(api, 1234, {0, std::numeric_limits<float>::infinity(), 0}, to).native_casts,
          "non-finite query coordinates are refused before native access");
    check(calls == before, "invalid inputs did not reach native physics");

    // The park editor's cast_surface still learns the selected object's body from the hit and
    // ignores it natively, through the same cast_surface_once.
    const std::array<std::uint64_t, 1> local_entity{99};
    std::uint64_t entity{};
    const auto behind = cast_surface(excluding_api, 1234, from, to, hit_owner, local_entity, &entity, {}, hit_body);
    check(behind && *behind == Vec3{0, 1, 0} && entity == 0,
          "the selected object's exact body is learnt and ignored before the surface behind it is reported");
    exclusion_broken = true;
    check(!cast_surface(excluding_api, 1234, from, to, hit_owner, local_entity, &entity, {}, hit_body),
          "a native query that ignores the exclusions gives no surface rather than the selected object");
    exclusion_broken = false;

    // The park editor's own casts keep the rules they always had, which its placement, picking,
    // world rays and ground checks rely on: a malformed fraction is skipped rather than making
    // the result unknown, and the request is not refused before the native call.
    scenario = Scenario::bad_fraction;
    auto upstream_calls = calls;
    const auto skipped = cast_surface(api, 1234, from, to);
    check(skipped && std::abs((*skipped)[1] - 0.8f) < 1e-5f && calls == upstream_calls + 1,
          "the park editor's cast no longer skips a malformed fraction for the next hit");
    scenario = Scenario::hit;
    upstream_calls = calls;
    const auto zero_length = cast_surface(api, 1234, from, from);
    check(zero_length && *zero_length == from && calls == upstream_calls + 1,
          "the park editor's cast refused a zero-length segment before the native call");
    const auto beyond_map = cast_surface(api, 1234, Vec3{200000, 4, 0}, Vec3{200000, 0, 0});
    check(beyond_map && calls == upstream_calls + 2, "the park editor's cast refused an endpoint beyond 100 km before the native call");

    // The counted cast asks no owner, ignores nothing and casts once.
    auto geometry_api = api;
    geometry_api.ray = [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay* request, const char* label) -> void* {
        check(request->ignored_bodies == std::array<std::uintptr_t, 5>{}, "the counted ray passes no native owner IDs");
        return api.ray(world, out, request, label);
    };
    scenario = Scenario::hit;
    auto geometry_calls = calls;
    counted = cast_surface_counted(geometry_api, 1234, from, to);
    check(counted.surface.available && counted.surface.hit && counted.surface.fraction == .25f && counted.native_casts == 1 &&
              calls == geometry_calls + 1,
          "the counted cast is one native call that reports the nearest hit");

    // Only the nearest hit's shape is read, and only a reader's success keeps it.
    static unsigned shape_reads{};
    static bool shape_fails{};
    const SurfaceBodyReader zero_shape = [](std::uintptr_t world, std::uint32_t index, std::uintptr_t, SurfaceBody& shape) {
        ++shape_reads;
        check(index == 1, "the shape read was not the nearest hit's");
        shape = {world, 0};
        return !shape_fails;
    };
    geometry_calls = calls;
    counted = cast_surface_counted(geometry_api, 1234, from, to, zero_shape);
    check(counted.surface.available && counted.surface.hit && counted.surface.fraction == .25f &&
              counted.surface.body == SurfaceBody{1234, 0} && counted.native_casts == 1 && calls == geometry_calls + 1 &&
              shape_reads == 1,
          "the nearest hit keeps its zero index and generation from one shape read and one cast");
    shape_fails = true;
    counted = cast_surface_counted(geometry_api, 1234, from, to, zero_shape);
    check(counted.surface.available && counted.surface.hit && counted.surface.body == SurfaceBody{} && shape_reads == 2,
          "a failed shape read leaves the hit without a shape, never with what the reader wrote");
    shape_fails = false;
    counted = cast_surface_counted(geometry_api, 1234, from, to);
    check(counted.surface.available && counted.surface.hit && counted.surface.body == SurfaceBody{} && counted.native_casts == 1,
          "a missing shape reader stays a hit for the caller to refuse, never a clear or farther ray");
    scenario = Scenario::null_empty_array;
    shape_reads = 0;
    counted = cast_surface_counted(geometry_api, 1234, from, to, zero_shape);
    check(counted.surface.available && !counted.surface.hit && counted.native_casts == 1 && !shape_reads,
          "only a validated empty native result is clear, and it reads no shape");
    scenario = Scenario::bad_fraction;
    counted = cast_surface_counted(geometry_api, 1234, from, to, zero_shape);
    check(!counted.surface.available && counted.native_casts == 1, "a malformed result is discarded without casting again");
    scenario = Scenario::hit;
    geometry_api.ray = [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay* request, const char* label) -> void* {
        api.ray(world, out, request, label);
        throw std::runtime_error("test query exception after a partial native result");
    };
    counted = cast_surface_counted(geometry_api, 1234, from, to, zero_shape);
    check(!counted.surface.available && counted.native_casts == 1,
          "a query that throws keeps its one native call and loses its result");
    check(releases == calls - 1 && scopes == releases, "a query that throws still releases its native result");

    // A named preset changes only the filter lane of the request the native ray receives. The
    // expected masks are literals, not the production constants.
    check(surface_query_filter(QueryPreset::camera) == 0x40080303u && surface_query_filter(QueryPreset::terrain) == 0x00000001u &&
              surface_query_filter(QueryPreset::all) == 0xffffffffu && !surface_query_filter(static_cast<QueryPreset>(3)) &&
              !surface_query_filter(static_cast<QueryPreset>(255)),
          "only the three fixed presets map to a native filter");
    // Only a filter that shares no bit with the named actor categories can back a claim that
    // no returned hit carried one; all is refused for that claim.
    static_assert((dingosdk::addr::park_editor::named_actor_categories & 0x40080303u) == 0 &&
                      (dingosdk::addr::park_editor::named_actor_categories & 0x00000001u) == 0 &&
                      dingosdk::addr::park_editor::named_actor_categories == 0xB620000Cu,
                  "a preset filter shares a named actor category");
    check((*surface_query_filter(QueryPreset::all) & dingosdk::addr::park_editor::named_actor_categories) != 0,
          "the all filter no longer covers the named actor categories");
    static NativeSurfaceRay seen{};
    static unsigned seen_calls{};
    auto preset_api = api;
    preset_api.ray = [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay* request, const char* label) -> void* {
        seen = *request;
        ++seen_calls;
        return api.ray(world, out, request, label);
    };
    const auto same_lanes = [](const NativeSurfaceRay& a, const NativeSurfaceRay& b) {
        return a.start == b.start && a.end == b.end && a.mode == b.mode && a.tolerance == b.tolerance && a.flags == b.flags &&
               a.allocator_vtable == b.allocator_vtable && a.allocator == b.allocator && a.ignored_bodies == b.ignored_bodies &&
               a.padding == b.padding;
    };
    scenario = Scenario::hit;
    const auto preset_calls = calls, preset_releases = releases, preset_scopes = scopes;
    auto preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape);
    const auto camera_request = seen;
    check(preset.surface.available && preset.surface.hit && preset.native_casts == 1 && seen_calls == 1 &&
              camera_request.filter == 0x40080303u && camera_request.mode == 0 && camera_request.tolerance == 0 &&
              camera_request.flags == 0 && camera_request.ignored_bodies == std::array<std::uintptr_t, 5>{} &&
              camera_request.start == std::array<float, 4>{0, 4, 0, 0},
          "the default request is not the camera request");
    preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, false, QueryPreset::camera);
    check(seen_calls == 2 && seen.filter == 0x40080303u && same_lanes(seen, camera_request), "an explicit camera differs from the default");
    std::vector<SurfaceBody> discovered;
    check(cast_surface_once(preset_api, 1234, from, to, nullptr, {}, nullptr, {}, discovered, nullptr) && seen_calls == 3 &&
              seen.filter == 0x40080303u && same_lanes(seen, camera_request),
          "the park editor's uncounted cast no longer sends the camera request");
    preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, false, QueryPreset::terrain);
    check(preset.surface.available && preset.surface.hit && preset.surface.fraction == .25f && preset.surface.position == Vec3{0, 3, 0} &&
              preset.surface.body == SurfaceBody{1234, 0} && preset.native_casts == 1 && seen_calls == 4,
          "terrain lost the nearest hit or spent another cast");
    check(seen.filter == 0x00000001u && same_lanes(seen, camera_request), "terrain sent anything but mask 1 or changed another lane");
    preset = cast_surface_counted(preset_api, 1234, from, to, nullptr, false, QueryPreset::terrain);
    check(preset.surface.available && preset.surface.hit && preset.surface.body == SurfaceBody{} && preset.native_casts == 1 &&
              seen_calls == 5,
          "a terrain hit with an unknown shape became clear or was cast through");
    scenario = Scenario::null_empty_array;
    preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, true, QueryPreset::terrain);
    check(preset.surface.available && !preset.surface.hit && !preset.surface.normal_available && preset.surface.normal == Vec3{} &&
              preset.native_casts == 1 && seen_calls == 6 && seen.filter == 0x00000001u,
          "a terrain clear carried a plane or a shape");
    scenario = Scenario::bad_fraction;
    preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, false, QueryPreset::terrain);
    check(!preset.surface.available && preset.native_casts == 1 && seen_calls == 7, "a malformed terrain result became clear");
    scenario = Scenario::hit;
    preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, false, QueryPreset::all);
    check(preset.surface.available && preset.surface.hit && preset.surface.fraction == .25f && preset.native_casts == 1 &&
              seen_calls == 8,
          "all lost the nearest hit or spent another cast");
    check(seen.filter == 0xffffffffu && same_lanes(seen, camera_request), "all sent anything but every bit or changed another lane");
    for (const auto unknown : {static_cast<QueryPreset>(3), static_cast<QueryPreset>(255)}) {
        preset = cast_surface_counted(preset_api, 1234, from, to, zero_shape, true, unknown);
        check(!preset.surface.available && !preset.surface.hit && !preset.native_casts,
              "an unknown preset reported a result or a native call");
        unsigned once_casts{};
        SurfaceCastProbe once{true, true};
        discovered.clear();
        check(!cast_surface_once(preset_api, 1234, from, to, nullptr, {}, nullptr, {}, discovered, zero_shape,
                                 {&once, &once_casts, false, unknown}) &&
                  !once_casts && !once.available && !once.hit && discovered.empty(),
              "an unknown preset reached the counted native call");
    }
    check(seen_calls == 8 && calls - preset_calls == 8, "an unknown preset reached the native ray");
    check(releases - preset_releases == calls - preset_calls && scopes - preset_scopes == releases - preset_releases,
          "preset requests lost native result cleanup");

    // Contact: the nearest hit's actual point and normal, read inside the result's scope.
    static unsigned contact_calls{}, contact_releases{};
    static bool invalid_normal{};
    static std::array<std::array<float, 4>, 2> contact_points{}, contact_normals{};
    const NativeSurfaceApi contact_api{
        nullptr,
        [](std::uintptr_t world, NativeSurfaceResult* out, const NativeSurfaceRay* request, const char*) -> void* {
            ++contact_calls;
            check(request->ignored_bodies == std::array<std::uintptr_t, 5>{}, "contact details must not add native owner IDs");
            static std::array<float, 2> hits{.8f, .25f};
            static std::array<std::uintptr_t, 5> fields{};
            contact_points = {{{0, .8f, 0, 1}, {.01f, 3, 0, 1}}};
            contact_normals = {{{1, 0, 0, 0}, {0, invalid_normal ? NAN : 1.f, 0, 0}}};
            fields[1] = reinterpret_cast<std::uintptr_t>(hits.data());
            fields[3] = reinterpret_cast<std::uintptr_t>(contact_points.data());
            fields[4] = reinterpret_cast<std::uintptr_t>(contact_normals.data());
            out->world = world;
            out->data = reinterpret_cast<std::uintptr_t>(fields.data());
            out->count = 2;
            out->allocator = 1;
            return out;
        },
        nullptr,
        [](void*, std::uintptr_t) {
            ++contact_releases;
            contact_normals = {};
        },
        true, true};
    auto contact = cast_surface_counted(contact_api, 1234, from, to, zero_shape, true);
    check(contact.surface.available && contact.surface.hit && contact.surface.normal_available &&
              contact.surface.position == Vec3{.01f, 3, 0} && contact.surface.normal == Vec3{0, 1, 0} && contact.native_casts == 1 &&
              contact_calls == 1 && contact_releases == 1,
          "contact copies the nearest actual point and normal inside the result's scope and keeps them after release");
    const auto without_contact = cast_surface_counted(contact_api, 1234, from, to, zero_shape);
    check(without_contact.surface.available && without_contact.surface.hit && !without_contact.surface.normal_available,
          "a cast that did not ask for contact reads none");
    invalid_normal = true;
    contact = cast_surface_counted(contact_api, 1234, from, to, zero_shape, true);
    check(contact.surface.available && contact.surface.hit && !contact.surface.normal_available && contact.native_casts == 1,
          "an invalid normal stays unknown without faking a clear ray or casting again");
    check(contact_calls == 3 && contact_releases == 3, "contact details keep one cast and its cleanup");
    if (failures) return 1;
    std::printf("Surface ray assertions, callbacks included: %u\n", assertions);
    std::puts("Surface rays: the park editor's own rules, and the counted cast's hit, clear and unavailable, empty native arrays, refusals, presets, contact and cleanup passed.");
    return 0;
}
