#include "custom_nametags.h"
#include "game_ui_state.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/UI/game_view.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dingosdk::multiplayer {
namespace {
using Clock = std::chrono::steady_clock;
using Address = std::uintptr_t;

struct State {
    std::atomic<bool> enabled{};
    std::atomic<Address> base{};
    std::mutex mutex;
    // The latest client frame: its camera (published earlier in the same tick) and who
    // stood where, so the two always match.
    GameView view;
    std::vector<overlay::Nametag> tags;
    Clock::time_point published;
    bool hidden{}; // the game hides its own nametags (menus, hidden UI; game_ui_state.h)
    bool show_names{true}, show_bubbles{};
    float bubble_distance{40.f};
};
State &state() {
    static auto *value = new State;
    return *value;
}
} // namespace

void publish_custom_nametags(std::uintptr_t base, std::vector<NametagPlayer> players,
                             std::optional<std::array<float, 3>> local, bool show_names,
                             bool show_bubbles, float bubble_distance) noexcept {
    auto &s = state();
    if (!s.enabled.load(std::memory_order_acquire)) return;
    s.base.store(base, std::memory_order_release);
    try {
        // This frame's camera, published earlier in the same client tick.
        const auto view = latest_game_view();
        const auto ui = sample_game_ui_state(base);
        const bool hidden = ui.in_menu || ui.ui_hidden || ui.nametags_hidden || ui.indicators_hidden;
        // Distances from the local skater, or from the camera without one.
        if (!local && view) local = std::array<float, 3>{view->world[12], view->world[13], view->world[14]};
        std::vector<overlay::Nametag> tags;
        tags.reserve(players.size());
        for (auto &player : players) {
            overlay::Nametag tag;
            tag.position = player.head;
            tag.name = std::move(player.name);
            tag.color = player.color;
            tag.tag = std::move(player.tag);
            tag.talking = player.talking;
            tag.bubbles.reserve(player.bubbles.size());
            for (auto &line : player.bubbles)
                tag.bubbles.push_back({std::move(line.text), std::move(line.raw), line.appear, line.fade});
            tag.self = player.self;
            if (local) {
                const float dx = player.head[0] - (*local)[0], dy = player.head[1] - (*local)[1], dz = player.head[2] - (*local)[2];
                tag.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            tags.push_back(std::move(tag));
        }
        std::lock_guard lock(s.mutex);
        s.hidden = hidden;
        s.show_names = show_names;
        s.show_bubbles = show_bubbles;
        s.bubble_distance = bubble_distance;
        if (!view) {
            s.tags.clear();
            return;
        }
        s.view = *view;
        s.tags = std::move(tags);
        s.published = Clock::now();
    } catch (...) {}
}

void set_custom_nametags_enabled(bool enabled) noexcept {
    auto &s = state();
    s.enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard lock(s.mutex);
        s.tags.clear();
    }
}

bool custom_nametags_enabled() noexcept { return state().enabled.load(std::memory_order_acquire); }

namespace {
// The camera as it is now: the game updates its own camera after the client update the view
// was taken in, so a nametag placed with that one trails the picture while the camera turns.
// Only the same camera object, still of a camera type, with a sound matrix.
void read_live_view(Address base, GameView &view) noexcept {
    if (!base || !view.camera || (view.camera & 7)) return;
    Address vtable{};
    if (!memory::peek(view.camera, vtable) ||
        (vtable != base + addr::engine::camera_vtable && vtable != base + addr::client_source_spawn::free_camera_vtable))
        return;
    std::array<float, 16> world{};
    float fov{};
    if (!memory::peek(view.camera + 0x50, world) || !memory::peek(view.camera + 0xac, fov)) return;
    if (!std::isfinite(fov) || fov <= 1 || fov >= 175) return;
    for (const auto value : world)
        if (!std::isfinite(value) || std::abs(value) > 1e7f) return;
    view.world = world;
    view.vertical_fov = fov;
}
} // namespace

overlay::Nametags custom_nametags() {
    auto &s = state();
    if (!s.enabled.load(std::memory_order_acquire)) return {};
    overlay::Nametags result;
    GameView view;
    {
        std::lock_guard lock(s.mutex);
        if (s.hidden || s.tags.empty() || Clock::now() - s.published > std::chrono::milliseconds(250)) return {};
        result.tags = s.tags;
        result.show_names = s.show_names;
        result.show_bubbles = s.show_bubbles;
        result.bubble_distance = s.bubble_distance;
        view = s.view;
    }
    read_live_view(s.base.load(std::memory_order_acquire), view);
    result.camera = view.world;
    result.vertical_fov = view.vertical_fov;
    return result;
}
} // namespace dingosdk::multiplayer
