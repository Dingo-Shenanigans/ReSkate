#include "Extension/Objects/ParkEditor/editor_math.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_document.h"
#include "Extension/Objects/ParkEditor/park_mods.h"
#include "Extension/Objects/ParkEditor/park_editor_highlight.h"
#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include <Windows.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
bool close_float(float a, float b) {
    return std::abs(a - b) < .001f;
}
template <class F> void rejects(F &&fn) {
    bool rejected = false;
    try {
        fn();
    } catch (...) {
        rejected = true;
    }
    check(rejected, "Invalid input accepted");
}
namespace surface_fixture {
using namespace dingosdk;
using namespace dingosdk::editor;
std::array<float, 2> fractions{};
std::array<std::uintptr_t, 7> data{};
unsigned calls{}, released{}, scopes{};
unsigned expected_bodies{};
bool miss{}, corrupt{};
void pop_scope(std::uintptr_t, void *) {
    ++scopes;
}
std::array<std::uintptr_t, 2> allocator_vtable{0, reinterpret_cast<std::uintptr_t>(&pop_scope)};
std::uintptr_t allocator = reinterpret_cast<std::uintptr_t>(allocator_vtable.data());
NativeSurfaceApi api{
    [](std::uintptr_t) -> std::uintptr_t { return 1234; },
    [](std::uintptr_t world, NativeSurfaceResult *out, const NativeSurfaceRay *ray, const char *) -> void * {
        ++calls;
        check(ray->filter == 0x40080303 &&
                  (ray->ignored_bodies[1] - ray->ignored_bodies[0]) / 16 == expected_bodies,
              "Surface query used the wrong native filter or exclude-vector layout");
        // Independent sloping surface y = .3*x + 2, with unsorted hits.
        const float start = ray->start[1] - .3f * ray->start[0] - 2;
        const float end = ray->end[1] - .3f * ray->end[0] - 2;
        const float t = start / (start - end);
        fractions = {std::min(1.0f, t + .1f), t};
        data[1] = reinterpret_cast<std::uintptr_t>(fractions.data());
        out->world = world;
        out->data = reinterpret_cast<std::uintptr_t>(data.data());
        out->allocator = 1;
        out->count = corrupt ? 9999U : (miss ? 0U : 2U);
        out->scope[2] = 1;
        return out;
    },
    []() -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(&allocator); },
    [](void *, std::uintptr_t) { ++released; }, true};
void test() {
    EditorSurfaceRequest request{3, 7, {1.3f, 10, 2.4f}, {0, -1, 0}, 1};
    const auto result = probe_surface(api, 9, request);
    check(result.available && result.hit && result.id == 7 && close_float(result.position[0], 1) &&
              close_float(result.position[1], 2.3f) && close_float(result.position[2], 2),
          "Surface snapping used a fake plane, rounded terrain height or selected the farther hit");
    check(calls == 2 && released == calls && scopes == calls, "Native ray results leaked allocation scopes");
    const SurfaceOwner owner = [](std::uintptr_t, std::uint32_t index, std::uintptr_t) -> std::uint64_t {
        return index == 1 ? 55 : 77;
    };
    request.grid = 0;
    const auto picked = probe_surface(api, 9, request, owner);
    check(picked.entity == 55, "Collision picking selected a farther body");
    const std::array<std::uint64_t, 1> ignored{55};
    check(probe_surface(api, 9, request, owner, ignored).entity == 77,
          "Placement ray hit the dragged object itself");
    const std::array<std::uint64_t, 2> group{55, 77};
    check(!probe_surface(api, 9, request, owner, group).hit,
          "Group drag failed to exclude every selected body");
    const SurfaceOwner occluded = [](std::uintptr_t, std::uint32_t index, std::uintptr_t) -> std::uint64_t {
        return index == 1 ? 0 : 77;
    };
    check(probe_surface(api, 9, request, occluded).entity == 0,
          "Picking selected through static world collision");
    expected_bodies = 2;
    const std::array<SurfaceBody, 3> bodies{{{1234, 9}, {1234, 10}, {9999, 11}}};
    check(probe_surface(api, 9, request, nullptr, {}, bodies).hit,
          "Native ignore-body array did not survive the surface query");
    expected_bodies = 0;
    miss = true;
    check(!probe_surface(api, 9, request).hit, "A ray miss fell back to a fake ground plane");
    miss = false;
    corrupt = true;
    check(!probe_surface(api, 9, request).hit && released == calls && scopes == calls,
          "Invalid native ray results were used or not released");
    request.direction = {};
    const auto before = calls;
    check(!probe_surface(api, 9, request).hit && before == calls, "Invalid ray reached native physics");
}
} // namespace surface_fixture
namespace closest_surface_fixture {
using namespace dingosdk;
using namespace dingosdk::editor;
unsigned calls{}, released{}, current_body{};
bool ignore_broken{};
constexpr std::uint64_t generation = std::uint64_t{7} << 32;
NativeSurfaceApi api{
    [](std::uintptr_t) -> std::uintptr_t { return 1234; },
    [](std::uintptr_t world, NativeSurfaceResult *out, const NativeSurfaceRay *ray, const char *) -> void * {
        ++calls;
        current_body = 0;
        const auto *begin = reinterpret_cast<const SurfaceBody *>(ray->ignored_bodies[0]);
        const auto *end = reinterpret_cast<const SurfaceBody *>(ray->ignored_bodies[1]);
        if (!ignore_broken)
            while (current_body < 3 && begin &&
                   std::find(begin, end, SurfaceBody{world, generation | current_body}) != end)
                ++current_body;
        // Two children of the dragged object, another selected object, then terrain.
        const float height = 8.0f - static_cast<float>(current_body) * 2;
        static float fraction{};
        fraction = (ray->start[1] - height) / (ray->start[1] - ray->end[1]);
        static std::array<std::uintptr_t, 7> data{0, reinterpret_cast<std::uintptr_t>(&fraction)};
        out->world = world;
        out->data = reinterpret_cast<std::uintptr_t>(data.data());
        out->count = 1; // A closest-only query hides the ground behind self collision.
        out->allocator = 1;
        return out;
    },
    nullptr, [](void *, std::uintptr_t) { ++released; }, true};
const SurfaceOwner owner = [](std::uintptr_t, std::uint32_t, std::uintptr_t) -> std::uint64_t {
    return current_body < 2 ? 55 : (current_body == 2 ? 77 : 0);
};
const SurfaceBodyReader reader = [](std::uintptr_t world, std::uint32_t, std::uintptr_t, SurfaceBody &out) {
    out = {world, generation | current_body};
    return true;
};
void test() {
    EditorSurfaceRequest request{3, 7, {1.3f, 12, 2.4f}, {0, -1, 0}, 1};
    const std::array<std::uint64_t, 2> group{55, 77};
    const auto ground = probe_surface(api, 9, request, owner, group, {}, reader);
    check(ground.hit && ground.entity == 0 && ground.position == Vec3{1, 2, 2} && calls == 8,
          "Closest-only self hits hid the ground or snapping failed to exclude every collision child");
    check(released == calls, "Retried self-collision queries leaked native results");
    request.grid = 0;
    const std::array<std::uint64_t, 1> single{55};
    const auto ramp = probe_surface(api, 9, request, owner, single, {}, reader);
    check(ramp.hit && ramp.entity == 77 && close_float(ramp.position[1], 4),
          "Self exclusion skipped a nonselected ramp");
    const auto picked = probe_surface(api, 9, request, owner, {}, {}, reader);
    check(picked.hit && picked.entity == 55 && close_float(picked.position[1], 8),
          "Picking excluded the object it should select");
    ignore_broken = true;
    const auto before = calls;
    check(!probe_surface(api, 9, request, owner, group, {}, reader).hit && calls == before + 2,
          "A native query ignoring its exclusion list looped or snapped onto self collision");
    check(released == calls, "Failed exclusions leaked native results");
}
} // namespace closest_surface_fixture
namespace highlight_fixture {
using namespace dingosdk::editor;
std::array<std::uint8_t, 24> component{};
unsigned finished{}, notified{};
NativeHighlightApi api{[](const void *, NativeHighlightScope *scope) -> void * {
                           scope->type = 1;
                           scope->component = reinterpret_cast<std::uintptr_t>(component.data());
                           return scope;
                       },
                       [](NativeHighlightScope *scope) {
                           ++finished;
                           notified += scope->dirty ? 1U : 0U;
                       },
                       nullptr, true};
void test() {
    const HighlightValue selected{7, 1};
    const auto old = highlight_value(api, nullptr, selected);
    check(old && *old == HighlightValue{0, 0} && finished == 1 && notified == 1,
          "Selection highlight failed to publish its component update");
    check(highlight_value(api, nullptr) == selected, "Selection highlight did not apply the outline color");
    highlight_value(api, nullptr, old, selected);
    check(highlight_value(api, nullptr) == old, "Deselection did not restore the original highlight");
    highlight_value(api, nullptr, HighlightValue{9, 1});
    highlight_value(api, nullptr, old, selected);
    check(highlight_value(api, nullptr) == HighlightValue{9, 1},
          "Highlight restore overwrote another system's change");
    check(finished == 7 && notified == 3, "Highlight scope notified on reads or leaked a native scope");
}
} // namespace highlight_fixture
} // namespace
int main(int argc, char **argv) {
    try {
        using namespace dingosdk;
        using namespace editor;
        surface_fixture::test();
        closest_surface_fixture::test();
        highlight_fixture::test();
        const Camera camera{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 3, 12, 1}, 60, 1600, 900};
        const auto center = camera.project({10, 3, 0});
        check(center && close_float((*center)[0], 800) && close_float((*center)[1], 450),
              "Camera origin/projection differs");
        check(!camera.project({10, 3, 13}), "Points behind the camera are pickable");
        const Vec3 target{12, 1, 0};
        const auto screen = camera.project(target);
        check(screen.has_value(), "Front point rejected");
        const auto hit =
            plane_hit(camera.origin(), camera.ray((*screen)[0], (*screen)[1]), target, {0, 1, 0});
        check(hit && close_float((*hit)[0], target[0]) && close_float((*hit)[1], target[1]) &&
                  close_float((*hit)[2], target[2]),
              "Projection/ray/plane round trip differs");
        check(!plane_hit({0, 1, 0}, {1, 0, 0}, {0, 0, 0}, {0, 1, 0}), "Parallel ray hit plane");
        check(!plane_hit({0, 1, 0}, {0, 1, 0}, {0, 0, 0}, {0, 1, 0}), "Backward ray hit plane");
        const auto parameter = axis_parameter({0, 1, 5}, normalized({2, -1, -5}), {0, 0, 0}, {1, 0, 0});
        check(parameter && close_float(*parameter, 2), "Axis drag does not follow cursor ray");
        check(!axis_parameter({0, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}), "Parallel axis drag is unstable");
        check(close_float(snapped(-1.26f, .5f), -1.5f) && close_float(snapped(1.26f, .5f), 1.5f),
              "Negative snapping differs");
        const auto q = rotation({25, 41, -73});
        const auto roundtrip = rotation(angles(q));
        float product = 0;
        for (unsigned i = 0; i < 4; ++i)
            product += q[i] * roundtrip[i];
        check(std::abs(product) > .99999f, "Rotation round trip differs");
        const auto turned = rotated({1, 0, 0}, rotation({0, 90, 0}));
        check(close_float(turned[0], 0) && close_float(turned[2], -1), "Local axis rotation differs");
        for (const auto name : {"", "../park", "A/B", "park.json", "CON", "nul", "LPT2", " trailing ", "x:y"})
            check(!valid_park_name(name), "Unsafe park filename accepted");
        check(valid_park_name("Warehouse - Session 2"), "Normal park name rejected");
        ParkDocument park{"bam",
                          {{1, "own_bk_ramp", {2, 3, 4}, q, 2.5f},
                           {2, "own_bk_rail", {5, 6, 7}, {0, 0, 0, 1}}}};
        const auto encoded = encode_park(park);
        const auto decoded = decode_park(encoded);
        check(encoded.find("object_scales") != std::string::npos && decoded.map == park.map &&
                  decoded.objects == park.objects,
               "Park save round trip lost uniform object scale");
        auto applied = park.objects[0];
        applied.id = 1;
        applied.scale = 1;
        check(profile_runtime::placement_matches_applied(park.objects[0], applied) &&
                  applied.id == park.objects[0].id && applied.scale == park.objects[0].scale,
              "A scaled restore acknowledgement did not recover its requested scale/identity");
        auto unrelated = applied;
        unrelated.id = 1;
        unrelated.scale = 1;
        unrelated.position[0] += 1;
        check(!profile_runtime::placement_matches_applied(park.objects[0], unrelated) &&
                  unrelated.id == 1 && unrelated.scale == 1,
              "A nonmatching placement was mutated or claimed as a restore acknowledgement");
        auto bad = park;
        bad.objects[1].id = 1;
        rejects([&] { encode_park(bad); });
        bad = park;
        bad.objects[0].rotation = {0, 0, 0, 0};
        rejects([&] { encode_park(bad); });
        bad = park;
        bad.objects[0].position[0] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { encode_park(bad); });
        bad = park;
        bad.objects[0].scale = 0;
        rejects([&] { encode_park(bad); });
        auto unknown_scale = Json::parse(encoded);
        unknown_scale["object_scales"]["bam"]["999"] = 2;
        rejects([&] { decode_park(unknown_scale.dump()); });
        rejects([&] { decode_park("{\"schema_version\":99}"); });
        rejects([&] { decode_park(std::string(2 * 1024 * 1024 + 1, ' ')); });
        const auto directory = std::filesystem::absolute(argc > 1 ? argv[1] : "build/editor-fixtures");
        const auto name = "Round trip " + std::to_string(GetCurrentProcessId());
        save_park(directory, name, park);
        check(load_park(directory, name).objects == park.objects, "On-disk park save differs");
        bad = park;
        bad.objects[0].rotation = {};
        rejects([&] { save_park(directory, name, bad); });
        check(load_park(directory, name).objects == park.objects, "Rejected save overwrote previous park");
        const auto stray = directory / L"\U0001F6F9 notes.txt"; // No ANSI code page can hold this name.
        std::ofstream(stray).put('x');
        const auto names = list_parks(directory);
        check(std::find(names.begin(), names.end(), name) != names.end(), "Saved park missing from browser");
        std::filesystem::remove(stray);
        std::filesystem::remove(directory / (name + ".park.json"));
        // Park mods: <data>/Mods/<folder>/manifest.json + parks/<map>.park.json.
        {
            const auto data_root = directory / ("mods-" + std::to_string(GetCurrentProcessId()));
            const auto mods_root = data_root / "Mods";
            std::filesystem::remove_all(data_root);
            const auto read_json = [](const std::filesystem::path &path) {
                std::ifstream input(path, std::ios::binary);
                return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
            };
            rejects([&] { create_park_mod(mods_root, {"", "Zee", "1.0", ""}); });
            rejects([&] { create_park_mod(mods_root, {"Park", "", "", std::string(513, 'x')}); });
            check(!std::filesystem::exists(mods_root / "Park"), "A rejected park mod left a folder behind");
            const auto folder = create_park_mod(mods_root, {"Pier Park!", "Zee", "1.0.0", "Lines by the water."});
            check(folder == "Pier Park_", "Park mod folder was not made safe");
            check(create_park_mod(mods_root, {"Pier Park!", "", "", ""}) == "Pier Park_ 2", "Duplicate mod names collided");
            const auto manifest = read_json(mods_root / folder / "manifest.json");
            check(manifest.value("name", std::string{}) == "Pier Park!" && manifest.value("author", std::string{}) == "Zee" &&
                      manifest.value("version_number", std::string{}) == "1.0.0" &&
                      manifest.value("description", std::string{}) == "Lines by the water.",
                  "manifest.json lacks name, author, version_number or description");
            save_mod_park(mods_root, folder, park);
            check(load_mod_park(mods_root, folder, "bam").objects == park.objects, "Park mod round trip differs");
            rejects([&] { load_mod_park(mods_root, folder, "grom"); });
            rejects([&] { load_mod_park(mods_root, folder, "../bam"); });
            rejects([&] { save_mod_park(mods_root, "Not Installed", park); });
            rejects([&] { load_mod_park(mods_root, "..", "bam"); });
            auto extended = manifest;
            extended["website_url"] = "https://example.com";
            std::ofstream(mods_root / folder / "manifest.json", std::ios::binary | std::ios::trunc) << extended.dump();
            save_park_mod_details(mods_root, folder, {"Pier Park", "Zee", "1.1.0", "Updated."});
            check(read_json(mods_root / folder / "manifest.json").value("website_url", std::string{}) ==
                      "https://example.com",
                  "Saving details dropped other manifest fields");
            const auto found_mods = list_park_mods(data_root);
            const auto found = std::find_if(found_mods.begin(), found_mods.end(),
                                            [&](const auto &mod) { return mod.folder == folder; });
            check(found != found_mods.end() && found->details.title == "Pier Park" && found->details.version == "1.1.0" &&
                      found->maps == std::vector<std::string>{"bam"} && found->enabled,
                  "Park mod listing lost its details or maps");
            std::filesystem::remove_all(data_root);
        }
        std::cout << "Park editor math, validation and atomic save tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
