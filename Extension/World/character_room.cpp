#include "character_room.h"
#include "Extension/Skater/skater_animation.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dingosdk::character_room {
namespace {
namespace menu_data = multiplayer::menu_data;
using logging::Channel;
using logging::Level;

// UI models by schema hash (TypeNameHash of the game's type) and field hash, from UI model dumps on
// 2026-10-04 and 2026-10-10 (PR #230, Engine/Game/Build/20260929/style.h).
// The menu's button bar: the buttons of the page that shows now. Each 16-byte element holds its button
// model's handle at +8, and the model's action has a navigation.
constexpr std::uint32_t button_bar = 0xe6ab1489, button_bar_buttons = 0x5fc6738b;
constexpr std::uintptr_t button_model_handle = 8;
constexpr std::uint32_t button_action = 0xa2b71c93, button_navigation = 0xb1e14cd6;
// UI/Legacy/Flow/State/StateNavigationModel: the menu's current and queued named navigations. The event is
// UI/Legacy/Flow/State/StateNavigationChanged.
constexpr std::uint32_t navigation_queue = 0xcc4776b5, navigation_current = 0xf3ba9cf8, navigation_queued = 0xf437b255;
constexpr std::uint32_t navigation_changed_event = 0xcb03d3ec;
// UI manager -> model manager.
constexpr std::uintptr_t ui_model_manager = 0x140;

// The menu's tab bar has up to seven tabs; RB goes to the next one.
constexpr int max_tab_presses = 7;
constexpr std::uint64_t tab_press_ms = 400, tap_ms = 150, tap_end_ms = 400;
constexpr std::uint64_t post_retry_ms = 250, step_deadline_ms = 4000;
constexpr std::uint64_t page_settle_ms = 300, queue_settle_ms = 1500;
constexpr std::uint64_t tab_wait_ms = 1500, skater_wait_ms = 10000;
constexpr std::uint64_t open_wait_ms = 30000, leave_wait_ms = 2000;
// The walk back ends by then whatever happens: without a skater ready (a bail's respawn, a load) it
// cannot step, and the room is left behind rather than waited on.
constexpr std::uint64_t leave_give_up_ms = 8000;
constexpr int max_scans = 3;
constexpr std::uint64_t scan_gap_ms = 15000;
// The way back to the world: the Skaters page's Back closes the menu. ToggleMenu is for a menu that is still open.
constexpr std::array<std::string_view, 2> way_back{"Back", "ToggleMenu"};

enum class Way { none, look, wait_closed, open, wait_open, tab, wait_tab, wait_skater };

struct State {
    std::mutex mutex;
    std::atomic<std::uintptr_t> navigation_type{};
    std::atomic<bool> scanning{};
    std::atomic<Phase> phase{Phase::idle};
    std::uint64_t generation{}; // a new skater (a respawn, a new map) makes the found event type stale
    std::uintptr_t skater{};
    std::uint64_t skater_since{}; // the scan never runs while a map loads
    std::uint64_t next_scan{};
    int scans{};
    Way way{};
    std::uint64_t way_deadline{}, way_started{}, open_deadline{};
    std::uint64_t next_post{};
    std::string_view post_failure;
    int tab_presses{};
    std::string page_at_press, seen_page;
    std::uint64_t page_shown_at{};
    int leave_steps_left{};
    bool leave_pressed{};
    std::uint64_t leave_step_deadline{}, leave_deadline{};
    // Once there: when the room was last seen on screen (the menu on its Skaters page), and checked.
    std::uint64_t room_seen_at{}, next_room_check{};
    std::string failure;
};
State& state() { static auto* value = new State; return *value; }

std::uintptr_t model_manager(std::uintptr_t base) {
    const auto ui = menu_data::read<std::uintptr_t>(base + game::build::v20260929::engine::ui_manager);
    return ui ? menu_data::read<std::uintptr_t>(ui + ui_model_manager) : 0;
}
// Runs `use` on the menu's UI models while they are locked. False when there are none.
template <class Use> bool with_models(std::uintptr_t base, Use&& use) {
    const auto manager = model_manager(base);
    if (!manager) return false;
    const menu_data::Context context(base, manager);
    game::ModelWriteLock lock(manager);
    use(context);
    return true;
}
// The navigations of the open page's buttons, in one line.
std::string menu_page(std::uintptr_t base) noexcept {
    std::string page;
    try {
        (void)with_models(base, [&](const menu_data::Context& context) {
            for (const auto& root : context.roots({button_bar})) {
                const auto buttons = context.field(root.model, button_bar_buttons);
                unsigned count{}, stride{};
                const auto bytes = context.array(buttons, 64, count, stride);
                for (unsigned i = 0; i < count && stride >= button_model_handle + 8; ++i) {
                    try {
                        std::uint64_t handle{};
                        std::memcpy(&handle, bytes.data() + i * stride + button_model_handle, sizeof(handle));
                        if (!handle) continue;
                        const menu_data::Value button{handle, context.type_of(handle)};
                        page += context.text(context.path(button, {button_action, button_navigation}), 64) + ' ';
                    } catch (...) {} // a button without an action
                }
            }
        });
    } catch (...) {}
    return page;
}
// The Skaters page has the button for a new preset.
bool skaters_page(const std::string& page) noexcept { return page.find("CASPresetPurchase ") != std::string::npos; }
bool navigation_pending(std::uintptr_t base) noexcept {
    bool pending{};
    try {
        (void)with_models(base, [&](const menu_data::Context& context) {
            for (const auto& root : context.roots({navigation_queue})) {
                unsigned count{}, stride{};
                (void)context.array(context.field(root.model, navigation_queued), 4, count, stride);
                pending = count > 0;
                return;
            }
        });
    } catch (...) {}
    return pending;
}
// As the menu's own buttons do. Returns why it was not queued, or an empty text.
std::string_view queue_navigation(std::uintptr_t base, std::string_view name) noexcept {
    try {
        std::string_view why = "no navigation queue model";
        const bool found = with_models(base, [&](const menu_data::Context& context) {
            const std::uint32_t hash = game::native_name_hash(name);
            for (const auto& root : context.roots({navigation_queue})) {
                context.array(context.field(root.model, navigation_queued), std::vector<std::uint32_t>{hash});
                context.set(context.field(root.model, navigation_current), hash);
                why = {};
                return;
            }
        });
        return found ? why : "no menu model";
    } catch (...) {
        return "the navigation queue could not be written";
    }
}
// The navigation event has no payload. A fault in the game's post fails the walk instead of the game.
bool announce(std::uintptr_t base, std::uintptr_t type) noexcept {
    alignas(16) const std::array<std::byte, 64> payload{};
    __try {
        return multiplayer::post_native_ui_event(base, type, payload.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool still_navigation_type(std::uintptr_t type) noexcept {
    std::uintptr_t record{};
    std::uint32_t hash{};
    return memory::peek(type, record) && memory::peek(record, hash) && hash == navigation_changed_event;
}
std::string_view navigate(std::uintptr_t base, State& s, std::string_view name, bool warn = true) noexcept {
    std::string_view why;
    const auto type = s.navigation_type.load(std::memory_order_acquire);
    if (!type) why = "the event type is not found";
    else if (!still_navigation_type(type)) {
        s.navigation_type.store(0, std::memory_order_release);
        s.next_scan = 0;
        why = "the event type is not current";
    } else if (!multiplayer::native_ui_event_code_known(base)) why = "the dispatch code changed";
    else if (const auto queued = queue_navigation(base, name); !queued.empty()) why = queued;
    else if (!announce(base, type)) why = "the post was refused";
    if (!why.empty() && warn) logging::log(Level::warning, Channel::skater, "Character room: could not send the menu navigation {}: {}.", name, why);
    return why;
}
// The caller holds s.mutex. The scan runs on its own thread (about 0.15 s).
void start_scan(State& s) noexcept {
    if (s.scanning.exchange(true)) return;
    try {
        std::thread([generation = s.generation] {
            auto& l = state();
            std::array<multiplayer::NativeTypeQuery, 1> query{{{navigation_changed_event, 0, 2}}};
            (void)multiplayer::find_native_types(query, true);
            {
                // A scan that ran through a new map found the type of the map before.
                std::lock_guard lock(l.mutex);
                if (l.generation == generation) l.navigation_type.store(query[0].object, std::memory_order_release);
            }
            l.scanning.store(false);
        }).detach();
    } catch (...) {
        s.scanning.store(false);
    }
}
void press_rb() {
    XINPUT_GAMEPAD pressed{}, neutral{};
    pressed.wButtons = XINPUT_GAMEPAD_RIGHT_SHOULDER;
    const auto ticks = [](std::uint64_t ms) { return static_cast<std::uint32_t>((ms * 60 + 999) / 1000); };
    overlay::play_pad({{0, pressed}, {ticks(tap_ms), neutral}, {ticks(tap_end_ms), neutral}});
}
// The caller holds s.mutex.
void fail(State& s, std::string_view why) {
    s.failure = std::format("The character room did not open: {}.", why);
    logging::log(Level::warning, Channel::skater, "Character room: {}", s.failure);
    // A menu that this walk opened does not stay open. Without the event type the walk sent nothing.
    if (s.way != Way::none && s.navigation_type.load(std::memory_order_acquire)) s.leave_steps_left = static_cast<int>(way_back.size());
    s.way = Way::none;
    s.phase.store(Phase::failed, std::memory_order_release);
}
bool post_step(std::uintptr_t base, State& s, std::uint64_t now, std::string_view name) noexcept {
    if (s.next_post && now < s.next_post) return false;
    const bool first = !s.next_post;
    if (first) {
        s.way_started = now;
        s.way_deadline = now + step_deadline_ms;
    }
    s.next_post = now + post_retry_ms;
    const auto why = navigate(base, s, name, first);
    if (!why.empty()) s.post_failure = why;
    return why.empty();
}
std::string deadline_cause(std::string_view what, const State& s) {
    return s.post_failure.empty() ? std::string(what) : std::format("{} (the last post: {})", what, s.post_failure);
}
void go(State& s, Way way, std::uint64_t now, std::uint64_t deadline_ms = 0) {
    s.way = way;
    s.way_started = now;
    s.way_deadline = now + deadline_ms;
    s.next_post = 0;
    s.post_failure = {};
}
// Walks to the menu's Skaters page from any game state: the menu opens on the page it showed last, and RB goes to the next page.
void step_way(std::uintptr_t base, State& s, std::uint64_t now) {
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    const auto page = in_menu ? menu_page(base) : std::string{};
    const bool skaters = skaters_page(page);
    // A page's bar fills over a few frames: the page counts when its bar has not changed for page_settle_ms.
    if (page != s.seen_page) {
        s.seen_page = page;
        s.page_shown_at = now + page_settle_ms;
    }
    const bool page_shown = !page.empty() && now >= s.page_shown_at;
    const auto settled = [&] { return !navigation_pending(base) || now > s.way_started + queue_settle_ms; };
    if (skaters && skater_animation::room_skater_seen()) {
        s.way = Way::none;
        s.room_seen_at = now;
        s.phase.store(Phase::arrived, std::memory_order_release);
        logging::log(Level::info, Channel::skater, "Character room: the room is open (the menu's Skaters page).");
    } else if (now > s.open_deadline) fail(s, "it took too long");
    else if (!s.navigation_type.load(std::memory_order_acquire)) {
        if (!s.scanning.load() && s.scans >= max_scans) fail(s, "the game's menu was not found");
    } else if (s.way == Way::look) {
        if (skaters) go(s, Way::wait_skater, now, skater_wait_ms);
        else if (in_menu) {
            if (post_step(base, s, now, "ToggleMenu")) go(s, Way::wait_closed, now, step_deadline_ms);
            else if (now > s.way_deadline) fail(s, deadline_cause("the game's menu did not close", s));
        } else go(s, Way::open, now);
    } else if (s.way == Way::wait_closed) {
        if (!in_menu && settled()) go(s, Way::open, now);
        else if (now > s.way_deadline) fail(s, "the game's menu did not close");
    } else if (s.way == Way::open) {
        if (post_step(base, s, now, "ToggleMenu")) go(s, Way::wait_open, now, step_deadline_ms);
        else if (now > s.way_deadline) fail(s, deadline_cause("the menu did not open", s));
    } else if (s.way == Way::wait_open) {
        if (in_menu && settled()) {
            s.tab_presses = 0;
            go(s, Way::tab, now, tab_wait_ms);
        } else if (now > s.way_deadline) fail(s, "the menu did not open");
    } else if (s.way == Way::tab) {
        if (skaters) go(s, Way::wait_skater, now, skater_wait_ms);
        else if (!in_menu) fail(s, "the menu closed");
        else if (!page_shown && now < s.way_deadline) {
        } else if (s.tab_presses >= max_tab_presses) fail(s, "the Skaters page did not open");
        else {
            press_rb();
            ++s.tab_presses;
            s.page_at_press = page;
            go(s, Way::wait_tab, now, tab_wait_ms);
        }
    } else if (s.way == Way::wait_tab) {
        if (!in_menu) fail(s, "the menu closed");
        else if ((now > s.way_started + tab_press_ms && page != s.page_at_press) || now > s.way_deadline) go(s, Way::tab, now, tab_wait_ms);
    } else if (s.way == Way::wait_skater) {
        // No press here: it would leave the page.
        if (!in_menu) fail(s, "the menu closed");
        else if (now > s.way_deadline) fail(s, "the menu's skater did not appear");
    }
}
// True while a leave runs.
bool leave_step(State& s, std::uintptr_t base, std::uint64_t now) {
    if (!s.leave_steps_left && !s.leave_pressed) return false;
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    if (s.leave_pressed) {
        if (in_menu && now < s.leave_step_deadline) return true;
        s.leave_pressed = false;
        return true;
    }
    const int left = std::clamp(s.leave_steps_left, 0, static_cast<int>(way_back.size()));
    if (!in_menu || left == 0) {
        s.leave_steps_left = 0;
        return true;
    }
    const auto name = way_back[way_back.size() - static_cast<std::size_t>(left)];
    if (!navigate(base, s, name).empty()) {
        s.leave_steps_left = 0;
        return true;
    }
    s.leave_pressed = true;
    --s.leave_steps_left;
    s.leave_step_deadline = now + leave_wait_ms;
    return true;
}
} // namespace

void open() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    skater_animation::watch_room(true);
    const auto now = GetTickCount64();
    s.leave_steps_left = 0;
    s.leave_pressed = false;
    s.scans = 0;
    s.next_scan = 0;
    s.failure.clear();
    s.open_deadline = now + open_wait_ms;
    s.phase.store(Phase::opening, std::memory_order_release);
    go(s, Way::look, now, open_wait_ms);
}
void leave() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.way = Way::none;
    skater_animation::watch_room(false);
    s.leave_steps_left = static_cast<int>(way_back.size());
    s.leave_pressed = false;
    s.leave_deadline = GetTickCount64() + leave_give_up_ms;
    s.phase.store(Phase::leaving, std::memory_order_release);
}
Phase phase() noexcept { return state().phase.load(std::memory_order_acquire); }
std::string failure() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.failure;
}
void tick(std::uintptr_t base, std::uintptr_t skater, bool ready, bool prepare) noexcept {
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        const auto now = GetTickCount64();
        // The game makes its menu again on each map: a new skater (a respawn, a new map) makes the found type stale.
        if (skater && skater != s.skater) {
            if (s.skater) {
                ++s.generation;
                s.navigation_type.store(0, std::memory_order_release);
                s.scans = 0;
                if (s.way != Way::none) fail(s, "the map changed");
                s.leave_steps_left = 0;
                s.leave_pressed = false;
            }
            s.skater = skater;
            s.skater_since = GetTickCount64();
        }
        if (s.phase.load(std::memory_order_acquire) == Phase::leaving && now >= s.leave_deadline) {
            s.leave_steps_left = 0;
            s.leave_pressed = false;
            s.phase.store(Phase::idle, std::memory_order_release);
            return;
        }
        if (!ready || !base) return;
        const auto p = s.phase.load(std::memory_order_acquire);
        // The scan is not cheap: for a walk, or once a map ahead of one, a few times at most.
        const bool ahead = prepare && p == Phase::idle && s.scans == 0 && now > s.skater_since + 10000;
        if ((ahead || p == Phase::opening || p == Phase::leaving) && !s.navigation_type.load(std::memory_order_acquire) && !s.scanning.load() &&
            now >= s.next_scan && s.scans < max_scans) {
            s.next_scan = now + scan_gap_ms;
            ++s.scans;
            start_scan(s);
        }
        if (leave_step(s, base, now)) return;
        // There, the player can still close the game's menu (Esc) or go to another of its pages: the room
        // is gone then, and the skater must not be drawn where it was.
        if (p == Phase::arrived && now >= s.next_room_check) {
            s.next_room_check = now + 250;
            if (multiplayer::sample_game_ui_state(base).in_menu && skaters_page(menu_page(base))) s.room_seen_at = now;
            else if (now > s.room_seen_at + 750) {
                skater_animation::watch_room(false);
                s.failure = "The character room closed: the game's menu left the Skaters page.";
                s.phase.store(Phase::failed, std::memory_order_release);
                logging::log(Level::info, Channel::skater, "Character room: {}", s.failure);
                return;
            }
        }
        if (p == Phase::leaving) {
            s.phase.store(Phase::idle, std::memory_order_release);
            return;
        }
        if (s.way != Way::none) step_way(base, s, now);
    } catch (...) {
        std::lock_guard lock(s.mutex);
        if (s.way != Way::none) fail(s, "an error in the walk");
    }
}
std::string menu_buttons(std::uintptr_t base) noexcept { return menu_page(base); }
bool is_skaters_page(const std::string& buttons) noexcept { return skaters_page(buttons); }
} // namespace dingosdk::character_room
