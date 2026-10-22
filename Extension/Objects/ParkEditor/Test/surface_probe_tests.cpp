#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include <cstdio>
#include <limits>

namespace {
using namespace dingosdk::editor;
int failures{};unsigned assertions{};
void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
}
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
        out->world = world; out->data = reinterpret_cast<std::uintptr_t>(values.data()); out->count = 1;
        return out;
    }, nullptr, nullptr, true};
const SurfaceOwner hit_owner = [](std::uintptr_t, std::uint32_t, std::uintptr_t) -> std::uint64_t {
    return currently_self ? 99 : 0;
};
const SurfaceBodyReader hit_body = [](std::uintptr_t world, std::uint32_t, std::uintptr_t, SurfaceBody& out) {
    out = {world, currently_self ? std::uint64_t{42} : std::uint64_t{43}}; return true;
};
}

int main() {
    const Vec3 from{0, 4, 0}, to{0, 0, 0};
    scenario = Scenario::hit;
    const auto hit = cast_surface_probe(api, 1234, from, to);
    check(hit.available && hit.hit && hit.fraction == 0.25f && hit.position == Vec3{0, 3, 0},
        "typed native ray selects the closest valid fraction and reports its world position");
    scenario = Scenario::clear;
    const auto clear = cast_surface_probe(api, 1234, from, to);
    check(clear.available && !clear.hit, "matching-world first0/count0 result is a successful clear query");
    scenario = Scenario::null_empty_array;
    const auto empty = cast_surface_probe(api, 1234, from, to);
    check(empty.available && !empty.hit, "an empty native query does not require an allocated fraction array");
    for (const auto malformed : {Scenario::zeroed, Scenario::wrong_world, Scenario::bad_count,
        Scenario::bad_fraction, Scenario::bad_first}) {
        scenario = malformed;
        const auto result = cast_surface_probe(api, 1234, from, to);
        check(!result.available && !result.hit, "unowned or malformed native result cannot become a clear ray");
    }
    check(releases == calls - 1 && scopes == releases, "hit, clear and malformed results all release native allocation state");
    const auto before = calls;
    auto unavailable = api;
    unavailable.ready = false;
    check(!cast_surface_probe(unavailable, 1234, from, to).available, "unverified native contracts do not execute a ray");
    check(!cast_surface_probe(api, 0, from, to).available, "missing physics world is unavailable");
    check(!cast_surface_probe(api, 1234, from, from).available, "zero-length ray is unavailable");
    check(!cast_surface_probe(api, 1234, {0, std::numeric_limits<float>::infinity(), 0}, to).available,
        "nonfinite query coordinates are rejected before native access");
    check(calls == before, "invalid inputs did not reach native physics");
    const std::array<std::uint64_t, 1> local_entity{99};
    const auto terrain = cast_surface_probe(excluding_api, 1234, from, to, {}, hit_owner, local_entity, hit_body);
    check(terrain.available && terrain.hit && terrain.fraction == 0.75f && terrain.entity == 0 &&
        terrain.body == SurfaceBody{1234, 43} && terrain.excluded_bodies == 1,
        "verified local root learns/excludes its exact native body before reporting the surface behind it");
    exclusion_broken = true;
    check(!cast_surface_probe(excluding_api, 1234, from, to, {}, hit_owner, local_entity, hit_body).available,
        "a native query ignoring exclusions becomes unavailable rather than self-hit or fake clear");
    // Bounded diagnostic counts actual backend callbacks, not wrapper attempts.
    // A chain of local child bodies must never silently spend32 native casts.
    static unsigned bounded_calls{},bounded_releases{},bounded_scopes{},children{};
    static bool malformed{};static std::uint64_t current_body{};
    const NativeSurfaceApi bounded_api{nullptr,
        [](std::uintptr_t world,NativeSurfaceResult* out,const NativeSurfaceRay* ray,const char*)->void* {
            ++bounded_calls;
            check(ray->filter==0x40080303&&ray->mode==0&&ray->tolerance==0&&ray->flags==0,
                "bounded path preserves original verified request filter/mode/tolerance/flags");
            const auto* begin=reinterpret_cast<const SurfaceBody*>(ray->ignored_bodies[0]);
            const auto* end=reinterpret_cast<const SurfaceBody*>(ray->ignored_bodies[1]);
            current_body=42;
            while(current_body<42+children&&begin&&std::find(begin,end,SurfaceBody{world,current_body})!=end)++current_body;
            static float fraction=.5f;static std::array<std::uintptr_t,2> values{};
            values[1]=reinterpret_cast<std::uintptr_t>(&fraction);out->world=world;
            out->data=reinterpret_cast<std::uintptr_t>(values.data());out->count=malformed?9999U:1U;
            out->allocator=1;out->scope[2]=1;return out;
        },
        []()->std::uintptr_t {static std::array<std::uintptr_t,2> table{0,reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t,void*){++bounded_scopes;})};
            static std::uintptr_t value=reinterpret_cast<std::uintptr_t>(table.data());return reinterpret_cast<std::uintptr_t>(&value);},
        [](void*,std::uintptr_t){++bounded_releases;},true};
    const SurfaceOwner bounded_owner=[](std::uintptr_t,std::uint32_t,std::uintptr_t)->std::uint64_t{return current_body<42+children?99:0;};
    const SurfaceBodyReader bounded_body=[](std::uintptr_t world,std::uint32_t,std::uintptr_t,SurfaceBody& body){body={world,current_body};return true;};
    const auto bounded=[&](unsigned budget){return cast_surface_probe_bounded(bounded_api,1234,from,to,budget,{},bounded_owner,local_entity,bounded_body);};
    auto b=bounded(2);check(b.surface.available&&b.surface.hit&&b.native_casts==1&&bounded_calls==1&&!b.budget_exhausted,
        "direct terrain uses one native cast and exports its actual count");
    children=1;bounded_calls=0;b=bounded(2);
    check(b.surface.available&&b.surface.hit&&b.native_casts==2&&bounded_calls==2&&b.surface.excluded_bodies==1&&!b.budget_exhausted,
        "one real local child exclusion reveals terrain within two backend casts");
    children=3;bounded_calls=0;b=bounded(2);
    check(!b.surface.available&&!b.surface.hit&&b.native_casts==2&&bounded_calls==2&&b.budget_exhausted,
        "more local collision children exhaust exact two-cast budget as unavailable");
    children=1;bounded_calls=0;b=bounded(1);
    check(!b.surface.available&&b.native_casts==1&&bounded_calls==1&&b.budget_exhausted,
        "one-cast budget cannot learn an exclusion and secretly query again");
    for(unsigned budget:{0U,3U,32U}) {bounded_calls=0;b=bounded(budget);
        check(!b.surface.available&&b.native_casts==0&&bounded_calls==0,"out-of-range diagnostic budget cannot reach native physics");}
    bounded_calls=0;
    b=cast_surface_probe_bounded(bounded_api,1234,from,from,2);
    check(!b.surface.available&&b.native_casts==0&&bounded_calls==0,"invalid bounded ray executes and counts no native call");
    malformed=true;bounded_calls=0;b=bounded(2);
    check(!b.surface.available&&b.native_casts==1&&bounded_calls==1&&!b.budget_exhausted,"malformed backend result does not use an exclusion retry or fake clear");
    check(bounded_releases==bounded_scopes,"bounded hit/exclusion/malformed results preserve native cleanup");
    malformed=false;children=100;bounded_calls=0;
    check(!cast_surface_probe(bounded_api,1234,from,to,{},bounded_owner,local_entity,bounded_body).available&&bounded_calls==32,
        "existing unbounded diagnostic API retains exact original32-attempt semantics");
    if (failures) return 1;
    std::printf("Surface backend assertions including repeated callbacks: %u\n",assertions);
    std::puts("Typed surface probes: hit/clear/unavailable, empty native arrays, validation and cleanup passed.");
    return 0;
}
