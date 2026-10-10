#include "game_ui_state.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <chrono>
#include <mutex>

namespace dingosdk::multiplayer {
namespace {
using menu_data::Context;
using Clock = std::chrono::steady_clock;
using Address = std::uintptr_t;

// UI/Foundations/State/DelMar_UI_ScreenData: the game's own menu and HUD visibility state.
constexpr std::uint32_t screen_data_hash = 0x2c5645a1;
constexpr std::uint32_t in_menu_offset = 20, hide_indicators_offset = 40, hide_nametags_offset = 56, hide_ui_offset = 80,
                        menu_focus_offset = 84;

struct State {
    std::mutex mutex;
    menu_data::Value screen; // DelMar_UI_ScreenData's root, once found
    Address screen_manager{};
    Clock::time_point next_scan, next_sample;
    bool logged{};
    GameUiState latest;
};
State &state() {
    static auto *value = new State;
    return *value;
}

GameUiState read(Address base, State &s) {
    const auto ui = menu_data::read<Address>(base + addr::engine::ui_manager);
    if (!ui) return {};
    const auto manager = menu_data::read<Address>(ui + 0x140);
    if (!manager) return {};
    const Context context(base, manager);
    game::ModelWriteLock lock(manager);
    if (s.screen_manager != manager || !s.screen.handle || context.type_of(s.screen.handle) != s.screen.type) {
        s.screen = {};
        s.screen_manager = manager;
        const auto now = Clock::now();
        if (now < s.next_scan) return {};
        s.next_scan = now + std::chrono::seconds(2);
        for (const auto &root : context.roots({screen_data_hash})) {
            s.screen = root.model;
            break;
        }
        if (!s.logged) {
            s.logged = true;
            logging::log(logging::Level::info, logging::Channel::ui, "Game UI state: {}.",
                         s.screen.handle ? "found" : "not found yet (nametags and chat show in menus until it is)");
        }
        if (!s.screen.handle) return {};
    }
    const auto at = context.address(s.screen);
    if (!at) return {};
    const auto flag = [&](std::uint32_t offset) { return menu_data::read<std::uint8_t>(at + offset) != 0; };
    return {flag(in_menu_offset), flag(hide_ui_offset), flag(hide_nametags_offset), flag(hide_indicators_offset),
            flag(menu_focus_offset)};
}
} // namespace

GameUiState sample_game_ui_state(std::uintptr_t base) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!base) return s.latest = {};
    if (const auto now = Clock::now(); now >= s.next_sample) {
        s.next_sample = now + std::chrono::milliseconds(100);
        try { s.latest = read(base, s); } catch (...) { s.screen = {}; s.latest = {}; }
    }
    return s.latest;
}
} // namespace dingosdk::multiplayer
