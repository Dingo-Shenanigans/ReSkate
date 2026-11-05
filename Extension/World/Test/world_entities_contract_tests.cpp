// detail::LookupCache (Extension/World/world_entities_contract.h): how often blueprint_bounds may
// search every asset domain for a name.
#include "Extension/World/world_entities_contract.h"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* what) {
    if (!value) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    using Cache = dingosdk::world_entities::detail::LookupCache<int>;
    {
        // A found answer is kept for five seconds, then searched again.
        Cache cache;
        int searches = 0;
        const auto found = [&]() -> std::optional<int> { ++searches; return 7; };
        check(cache.get("deck", 1000, found) == 7, "first ask searches");
        for (std::uint64_t now = 1000; now < 1000 + Cache::found_for; now += 16) (void)cache.get("deck", now, found);
        check(searches == 1, "a found answer is not searched again within five seconds");
        check(cache.get("deck", 1000 + Cache::found_for, found) == 7 && searches == 2, "a found answer is searched again after five seconds");
    }
    {
        // A miss is asked again after a second, and becomes found.
        Cache cache;
        bool loaded = false;
        int searches = 0;
        const auto search = [&]() -> std::optional<int> { ++searches; return loaded ? std::optional<int>{3} : std::nullopt; };
        check(!cache.get("rail", 0, search), "a missing blueprint has no box");
        loaded = true;
        check(!cache.get("rail", 999, search) && searches == 1, "a miss is kept for a second");
        check(cache.get("rail", 1000, search) == 3 && searches == 2, "a miss is searched again after a second");
    }
    {
        // Outside an active level (loading, unloading, quitting) nothing is searched: a name keeps its
        // last answer, even one past its time, or has none.
        Cache cache;
        int searches = 0;
        const auto found = [&]() -> std::optional<int> { ++searches; return 5; };
        check(cache.get("ramp", 0, found) == 5 && searches == 1, "a name is searched in an active level");
        check(cache.get("ramp", 10 * Cache::found_for, found, false) == 5 && searches == 1,
              "outside an active level a name keeps its last answer");
        check(!cache.get("ledge", 10 * Cache::found_for, found, false) && searches == 1,
              "outside an active level a new name is not searched");
        check(cache.get("ledge", 10 * Cache::found_for, found) == 5 && searches == 2, "back in a level it is");
    }
    {
        // A plugin asking for many names every frame: at most eight searches each second.
        Cache cache;
        int searches = 0;
        const auto search = [&]() -> std::optional<int> { ++searches; return std::nullopt; };
        for (std::uint64_t frame = 0; frame < 60; ++frame)
            for (int name = 0; name < 100; ++name) (void)cache.get("blueprint" + std::to_string(name), 5000 + frame * 16, search);
        check(searches == Cache::scans_per_second, "searches are rationed within one second");
        for (int name = 0; name < 100; ++name) (void)cache.get("blueprint" + std::to_string(name), 6000, search);
        check(searches == 2 * Cache::scans_per_second, "the ration starts again the next second");
    }
    {
        // Past the ration a name keeps its last answer.
        Cache cache;
        int value = 1;
        const auto search = [&]() -> std::optional<int> { return value; };
        check(cache.get("a", 0, search) == 1, "a is found");
        for (int name = 0; name < 20; ++name) (void)cache.get("x" + std::to_string(name), 5000, search);
        value = 2;
        check(cache.get("a", 5001, search) == 1, "a stale name keeps its answer while searches are used up");
        check(cache.get("a", 6000, search) == 2, "and is searched again once there is room");
    }
    {
        using dingosdk::world_entities::detail::movement_due;
        check(!movement_due(false, false, false, 2000, 1000) &&
              !movement_due(false, false, true, 2000, 1000) &&
              !movement_due(false, true, false, 2000, 1000) &&
              !movement_due(false, true, true, 2000, 1000),
              "unchanged matrices never move, regardless of solidity or attachment");
        check(movement_due(true, false, false, 1000, 1000),
              "a changed non-solid visual does not wait for the wall clock");
        check(!movement_due(true, true, false, 1015, 1000),
              "an unattached solid object still waits at 15 ms");
        check(movement_due(true, true, false, 1016, 1000),
              "an unattached solid object still moves at 16 ms");
        check(movement_due(true, true, true, 1000, 1000) &&
              movement_due(true, false, true, 1000, 1000),
              "changed joint attachments still move every client tick");
        // Consecutive 15/16 ms client frames must not hold alternate visual updates.
        // Each resident part can have a different last-move time.
        std::uint64_t moved[8]{1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007};
        constexpr std::uint64_t frames[]{1015, 1030, 1046, 1061, 1077};
        unsigned moves{};
        for (const auto now : frames) {
            bool all_moved = true;
            for (auto& previous : moved) {
                if (movement_due(true, false, false, now, previous)) previous = now, ++moves;
                else all_moved = false;
            }
            check(all_moved, "all changed non-solid parts advance on each 15/16 ms client frame");
        }
        check(moves == 40u, "eight changed visual parts move once on each of five client frames");
    }
    {
        // Bodies are plain numbers here: 1-99 another owner's, 100 and up the object's own.
        using dingosdk::world_entities::detail::own_bodies;
        const std::array<std::pair<int, int>, 3> rays{{{0, 0}, {1, 0}, {2, 0}}};
        const auto mine = [](int body) { return body >= 100; };
        unsigned asked{};
        const auto owned = [&](int body) { ++asked; return std::vector<int>{body, body + 1}; };
        const auto meeting = [](std::array<std::optional<int>, 3> met) {
            return [met](int from, int) { return met[static_cast<std::size_t>(from)]; };
        };
        check(own_bodies(rays, meeting({42, 42, 42}), mine, owned).empty() && asked == 0,
              "another owner's bodies in the box are never taken, however many rays meet them");
        check(own_bodies(rays, meeting({std::nullopt, std::nullopt, std::nullopt}), mine, owned).empty(),
              "an object nothing is met in has no bodies");
        check(own_bodies(rays, meeting({42, 200, 300}), mine, owned) == std::vector<int>({200, 201}) && asked == 1,
              "a ray past another owner's body can still find the object's own, and the first found stands");
        check(own_bodies(rays, meeting({std::nullopt, 200, 42}), mine, owned) == std::vector<int>({200, 201}),
              "a ray that meets nothing does not end the search");
        const auto none_for_first = [](int body) { return body == 200 ? std::vector<int>{} : std::vector<int>{body}; };
        check(own_bodies(rays, meeting({200, 300, 42}), mine, none_for_first) == std::vector<int>({300}),
              "an owner whose bodies could not be listed is passed over");
    }
    {
        using dingosdk::world_entities::detail::own_body;
        check(own_body(0x1000, 0x1000), "a body whose owner keeps the entity's own component is the object's");
        check(!own_body(0x1000, 0x2000), "another object's component is not, a second of the same blueprint included");
        check(!own_body(0x1000, 0), "an owner that keeps no component is not, whatever the entity holds");
        check(!own_body(0, 0), "an object with no component owns nothing, even when the owner keeps none either");
    }
    if (failures) return 1;
    std::cout << "world entities contract tests passed\n";
    return 0;
}
