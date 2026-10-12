#include "Server/object_check.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using dingosdk::NetworkObject;
using dingosdk::server::ObjectCheck;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
NetworkObject object(std::uint64_t id, float x, float scale = 1.f) {
    NetworkObject o;
    o.id = id;
    o.item = "own_bk_bell";
    o.position = {x, 0, 0};
    o.scale = scale;
    return o;
}
std::map<std::uint64_t, NetworkObject> share(const std::vector<NetworkObject> &layout) {
    std::map<std::uint64_t, NetworkObject> out;
    for (const auto &o : layout) out[o.id] = o;
    return out;
}
constexpr std::uint64_t step_us = 100000;
}

int main() {
    const ObjectCheck::Rules rules;
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{100, 0, 0}};
        auto result = guard.filter(step_us, {object(1, 0, 40.f)}, {}, others, rules);
        check(result.layout.size() == 1 && result.layout[0].scale == rules.max_scale, "An oversized object was not held to the scale limit");
        check(!result.on_players && !result.retry, "A far object was held back");
    }
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{0.5f, 0, 0}};
        std::vector<NetworkObject> bells;
        for (std::uint64_t id = 1; id <= 20; ++id) bells.push_back(object(id, 0, 3.f));
        auto result = guard.filter(step_us, bells, {}, others, rules);
        check(result.layout.empty() && result.on_players == 20 && result.newly_on_players == 20 && result.retry,
              "Bells spawned on a player were shared");
        result = guard.filter(2 * step_us, bells, {}, others, rules);
        check(!result.newly_on_players && !result.chasing && result.on_players == 20, "Bells left where they were counted as chasing");
        for (auto &bell : bells) bell.position[1] = 0.3f;
        result = guard.filter(3 * step_us, bells, {}, others, rules);
        check(result.chasing == 20, "Bells moved around on a player were not counted as chasing");
    }
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{30, 0, 0}};
        auto layout = std::vector{object(1, 0)};
        auto shared = share(guard.filter(step_us, layout, {}, others, rules).layout);
        layout[0].position = {29.5f, 0, 0};
        auto result = guard.filter(2 * step_us, layout, shared, others, rules);
        check(result.layout.size() == 1 && result.layout[0].position[0] == 0.f, "An object moved onto a player did not stay where it was");
        check(result.newly_on_players == 1 && !result.chasing, "One object moved next to a player counted as chasing");
        const std::vector<ObjectCheck::Position> gone{{80, 0, 0}};
        result = guard.filter(3 * step_us, layout, shared, gone, rules);
        check(result.layout[0].position[0] == 29.5f && !result.retry, "A held-back object did not go through once the player left");
    }
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{500, 0, 0}};
        auto layout = std::vector{object(1, 0)};
        auto shared = share(guard.filter(0, layout, {}, others, rules).layout);
        for (unsigned i = 1; i <= 600; ++i) {
            layout[0].position = {static_cast<float>(i % 40) * 0.1f, 0, 0};
            const auto result = guard.filter(i * step_us, layout, shared, others, rules);
            check(!result.throttled && !result.throttled_us, "Dragging one object was throttled");
            shared = share(result.layout);
        }
    }
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{500, 0, 0}};
        std::vector<NetworkObject> bells;
        for (std::uint64_t id = 1; id <= 20; ++id) bells.push_back(object(id, static_cast<float>(id) * 5));
        auto shared = share(guard.filter(0, bells, {}, others, rules).layout);
        std::uint64_t throttled{};
        for (unsigned i = 1; i <= 100; ++i) {
            for (auto &bell : bells) bell.rotation = {0, std::sin(i * 0.1f), 0, std::cos(i * 0.1f)};
            const auto result = guard.filter(i * step_us, bells, shared, others, rules);
            throttled += result.throttled_us;
            shared = share(result.layout);
        }
        check(throttled >= 8000000, "Twenty bells spun for ten seconds were not throttled");
    }
    {
        ObjectCheck guard;
        const std::vector<ObjectCheck::Position> others{{0, 0, 0}};
        auto result = guard.filter(step_us, {object(1, 0)}, {}, others, {rules.max_scale, 0.f});
        check(result.layout.size() == 1, "A clearance of 0 still held objects back");
    }
    return 0;
}
