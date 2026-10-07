#include "Extension/Music/local_music_shelf_lifetime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace md = dingosdk::multiplayer::menu_data;
struct Ref { std::uint64_t record, handle; bool operator==(const Ref&) const = default; };
struct Model { std::uint64_t handle, type; };
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void shelf_transition(unsigned leaving, unsigned active) {
    md::MenuLifetime lifetime;
    md::OwnedMenuModels<Model> models;
    lifetime.before_transition(13, [] {});
    const std::vector<Ref> native{{11, 100}, {22, 200}, {33, 300}};
    auto refs = native;
    refs.insert(refs.begin() + 1, Ref{22, 401}); // Tagged anchor, same record as native Liked.
    models.track({400, 1}); models.track({500, 2});
    bool child_bound = true, assets_live = true;
    unsigned cleanups{};
    std::vector<std::uint64_t> destroyed;
    const auto cleanup = [&] {
        ++cleanups;
        check(lifetime.blocked(), "Block shelf creation and polling before detach callbacks");
        models.release([&] {
            refs = dingosdk::profile_runtime::music_shelf_detached(std::span<const Ref>{refs}, 400);
            child_bound = false;
        }, [&](Model model) {
            check(assets_live, "Destroy widget-bearing rows before unloading their assets");
            check(refs == native && !child_bound, "Detach shelf and child bindings before destruction");
            destroyed.push_back(model.handle);
        });
    };
    lifetime.before_transition(leaving, cleanup);
    assets_live = false;
    for (unsigned next : {leaving, 3U, 4U, 5U, 12U}) {
        lifetime.before_transition(next, cleanup);
        check(lifetime.blocked(), "Loading ticks must not recreate or republish the shelf");
    }
    check(cleanups == 1 && models.empty() && destroyed == std::vector<std::uint64_t>{500, 400},
        "Release both roots exactly once, including the two-row tile list");
    assets_live = true;
    lifetime.before_transition(active, cleanup);
    check(!lifetime.blocked(), "Allow rebuilding in the next active world");
    models.track({600, 1});
    lifetime.before_transition(24, cleanup);
    assets_live = false;
    lifetime.before_transition(24, cleanup);
    lifetime.before_transition(active, cleanup);
    check(lifetime.blocked() && models.empty() && destroyed.back() == 600 && cleanups == 2,
        "Shutdown must release a rebuilt shelf and permanently suppress recreation");
}

void partial_generation() {
    md::OwnedMenuModels<Model> models;
    models.track({400, 1}); // Anchor created, tile-template preparation failed.
    unsigned destroys{};
    try { models.release([] { throw std::runtime_error("detach failed"); }, [&](Model) { ++destroys; }); }
    catch (const std::runtime_error&) {}
    check(!models.empty() && !destroys, "Keep failed cleanup ownership for a retry");
    models.release([] {}, [&](Model) { ++destroys; });
    check(models.empty() && destroys == 1, "Release partial builds as well as published shelves");
    models.track({400, 1});
    models.manager_replaced();
    models.release([] {}, [](Model) { throw std::runtime_error("expired manager dereferenced"); });
}

int main() {
    try {
        for (unsigned leaving : {14U, 22U, 3U}) shelf_transition(leaving, leaving == 22 ? 21 : 13);
        partial_generation();
        const std::array<Ref, 1> native{{{1, 100}}};
        check(dingosdk::profile_runtime::music_shelf_detached(std::span<const Ref>{native}, 400) ==
            std::vector<Ref>{native.begin(), native.end()}, "Expired/missing shelf removal preserves native entries");
        std::puts("Music shelf lifetime tests passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
