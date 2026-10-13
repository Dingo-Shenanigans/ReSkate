#include "menu_room.h"
#include "style_internal.h"
#include "style_layer.h"
#include "style_layer_editor.h"
#include "trick_pad.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dingosdk::menu_room {
namespace {
using style::notify_player;
namespace menu_data = multiplayer::menu_data;
namespace model = addr::style;

// The menu's tab bar has up to seven tabs; RB goes to the next one.
constexpr int max_tab_presses = 7;
constexpr std::uint64_t tab_press_ms = 400;
constexpr std::uint64_t post_retry_ms = 250, step_deadline_ms = 4000;
constexpr std::uint64_t page_settle_ms = 300, queue_settle_ms = 1500;
constexpr std::uint64_t tab_wait_ms = 1500, skater_wait_ms = 10000;
// An open has this long to find the menu, and then this long for the room's skater.
constexpr std::uint64_t open_wait_ms = 30000, then_wait_ms = 60000;
constexpr int max_scans = 3;
constexpr std::uint64_t scan_gap_ms = 15000, leave_wait_ms = 2000;
constexpr unsigned open_cover_ms = 45000, screen_cover_ms = 900, leave_cover_ms = 3000;
// The way back to the world: the Skaters page's Back closes the menu. ToggleMenu is for a menu that is still open.
constexpr std::array<std::string_view, 2> way_back{"Back", "ToggleMenu"};

enum class Way { none, look, wait_closed, open, wait_open, tab, wait_tab, wait_skater };

struct State {
    std::mutex mutex;
    std::atomic<std::uintptr_t> navigation_type{};
    std::atomic<bool> scanning{};
    std::uint64_t map_changes{};
    std::uint64_t next_scan{};
    int scans{};
    Way way{};
    std::uint64_t way_deadline{}, way_started{};
    std::uint64_t next_post{}; // 0 before the step's first post
    std::string_view post_failure; // literals only, so the view stays valid
    int tab_presses{};
    std::string page_at_press;
    std::string seen_page;
    std::uint64_t page_shown_at{};
    std::atomic<int> leave_steps_left{};
    bool leave_pressed{};
    std::uint64_t leave_step_deadline{};
    std::function<void()> then;
    std::uint64_t then_deadline{};
    std::optional<StagePlace> map;
};
State &state() { static auto *value = new State; return *value; }

std::uintptr_t model_manager(std::uintptr_t base) {
    const auto ui = menu_data::read<std::uintptr_t>(base + addr::engine::ui_manager);
    return ui ? menu_data::read<std::uintptr_t>(ui + model::ui_model_manager) : 0;
}
// Runs `use` on the menu's UI models while they are locked. False when there are none.
template <class Use> bool with_models(std::uintptr_t base, Use &&use) {
    const auto manager = model_manager(base);
    if (!manager) return false;
    const menu_data::Context context(base, manager);
    game::ModelWriteLock lock(manager);
    use(context);
    return true;
}
// The button bar holds only the open page's buttons; other page models stay after their page closes.
std::string menu_page(std::uintptr_t base) noexcept {
    std::string page;
    try {
        (void)with_models(base, [&](const menu_data::Context &context) {
            for (const auto &root : context.roots({model::button_bar})) {
                const auto buttons = context.field(root.model, model::button_bar_buttons);
                unsigned count{}, stride{};
                const auto bytes = context.array(buttons, 64, count, stride);
                for (unsigned i = 0; i < count && stride >= model::button_model_handle + 8; ++i) {
                    try {
                        std::uint64_t handle{};
                        std::memcpy(&handle, bytes.data() + i * stride + model::button_model_handle, sizeof(handle));
                        if (!handle) continue;
                        const menu_data::Value button{handle, context.type_of(handle)};
                        page += context.text(context.path(button, {model::button_action, model::button_navigation}), 64) + ' ';
                    } catch (...) {} // a button without an action
                }
            }
        });
    } catch (...) {}
    return page;
}
// The Skaters page has the button for a new preset.
bool skaters_page(const std::string &page) noexcept { return page.find("CASPresetPurchase ") != std::string::npos; }
bool navigation_pending(std::uintptr_t base) noexcept {
    bool pending{};
    try {
        (void)with_models(base, [&](const menu_data::Context &context) {
            for (const auto &root : context.roots({model::navigation_queue})) {
                unsigned count{}, stride{};
                (void)context.array(context.field(root.model, model::navigation_queued), 4, count, stride);
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
        const bool found = with_models(base, [&](const menu_data::Context &context) {
            const std::uint32_t hash = game::native_name_hash(name);
            for (const auto &root : context.roots({model::navigation_queue})) {
                context.array(context.field(root.model, model::navigation_queued), std::vector<std::uint32_t>{hash});
                context.set(context.field(root.model, model::navigation_current), hash);
                why = {};
                return;
            }
        });
        return found ? why : "no menu model";
    } catch (...) {
        return "the navigation queue could not be written";
    }
}
// The navigation event has no payload.
// A fault in the game's post fails the open instead of the game.
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
    return memory::peek(type, record) && memory::peek(record, hash) && hash == model::navigation_changed_event;
}
std::string_view send_navigation(std::uintptr_t base, State &s, std::string_view name) noexcept {
    const auto type = s.navigation_type.load(std::memory_order_acquire);
    if (!type) return "the event type is not found";
    if (!still_navigation_type(type)) {
        s.navigation_type.store(0, std::memory_order_release);
        s.next_scan = 0;
        return "the event type is not current";
    }
    if (!multiplayer::native_ui_event_code_known(base)) return "the dispatch code changed";
    if (const auto why = queue_navigation(base, name); !why.empty()) return why;
    if (!announce(base, type)) return "the post was refused";
    return {};
}
std::string_view navigate(std::uintptr_t base, State &s, std::string_view name, bool warn = true) noexcept {
    const auto why = send_navigation(base, s, name);
    if (!why.empty() && warn)
        logging::log(logging::Level::warning, logging::Channel::skater, "Style editor: could not send the menu navigation {}: {}.", name,
                     why);
    return why;
}
// The caller holds s.mutex.
void start_scan(State &s) noexcept {
    if (s.scanning.exchange(true)) return;
    try {
        std::thread([map_changes = s.map_changes] {
            auto &l = state();
            std::array<multiplayer::NativeTypeQuery, 1> query{{{model::navigation_changed_event, 0, 2}}};
            (void)multiplayer::find_native_types(query, true);
            {
                // A scan that ran through a map change found the type of the map before.
                std::lock_guard lock(l.mutex);
                if (l.map_changes == map_changes) l.navigation_type.store(query[0].object, std::memory_order_release);
            }
            l.scanning.store(false);
        }).detach();
    } catch (...) {
        s.scanning.store(false);
    }
}
void fail(State &s, std::string_view why) {
    // The text comes first: if it throws, the stop still sees the open and leaves the menu.
    const auto notice = std::format("the editor could not open: {}", why);
    // A menu that this opened does not stay open. Without the event type this open sent nothing.
    if ((s.way != Way::none || s.then) && s.navigation_type.load(std::memory_order_acquire))
        s.leave_steps_left.store(static_cast<int>(way_back.size()), std::memory_order_relaxed);
    s.way = Way::none;
    s.then = nullptr;
    overlay::cover({}, 0);
    notify_player(overlay::NoticeLevel::warning, notice);
}
bool post_step(std::uintptr_t base, State &s, std::uint64_t now, std::string_view name) noexcept {
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
std::string deadline_cause(std::string_view what, const State &s) {
    return s.post_failure.empty() ? std::string(what) : std::format("{} (the last post: {})", what, s.post_failure);
}
void go(State &s, Way way, std::uint64_t now, std::uint64_t deadline_ms = 0) {
    s.way = way;
    s.way_started = now;
    s.way_deadline = now + deadline_ms;
    s.next_post = 0;
    s.post_failure = {};
}
// Walks to the menu's Skaters page from any game state: the menu opens on the page it showed last, and RB goes to the next page.
void step_way(std::uintptr_t base, State &s, std::uint64_t now) {
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    const auto page = in_menu ? menu_page(base) : std::string{};
    const bool skaters = skaters_page(page);
    // A page's bar fills over a few frames: the page counts when its bar has not changed for page_settle_ms.
    if (page != s.seen_page) {
        s.seen_page = page;
        s.page_shown_at = now + page_settle_ms;
    }
    const bool page_shown = !page.empty() && now >= s.page_shown_at;
    // A step waits for the game to run the queued navigation; the time is a fallback.
    const auto settled = [&] { return !navigation_pending(base) || now > s.way_started + queue_settle_ms; };
    if (skaters && style_layer::room_present()) s.way = Way::none;
    else if (!s.navigation_type.load(std::memory_order_acquire)) {
        if (!s.scanning.load() && now > s.way_deadline) fail(s, "the game's menu was not found");
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
            trick_pad::press(XINPUT_GAMEPAD_RIGHT_SHOULDER, trick_pad::tap_ms, trick_pad::tap_end_ms);
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
} // namespace

void open(std::function<void()> then) {
    auto &s = state();
    style_layer::watch_room();
    overlay::close_menus();
    overlay::cover("Opening the style editor", open_cover_ms);
    const auto now = GetTickCount64();
    std::lock_guard lock(s.mutex);
    // A leave that has sent nothing yet stops. A leave that has sent a step goes on, and the open comes after it.
    if (!s.leave_pressed && s.leave_steps_left.load(std::memory_order_relaxed) == static_cast<int>(way_back.size()))
        s.leave_steps_left.store(0, std::memory_order_relaxed);
    s.scans = 0;
    s.then = std::move(then);
    s.then_deadline = now + then_wait_ms;
    if (s.way == Way::none) go(s, Way::look, now, open_wait_ms);
}
void leave() noexcept {
    state().leave_steps_left.store(static_cast<int>(way_back.size()), std::memory_order_relaxed);
    if (!overlay::covered()) overlay::cover("Back to the world", leave_cover_ms);
}
bool idle() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.way == Way::none && !s.then && !s.leave_pressed && !s.leave_steps_left.load(std::memory_order_relaxed);
}
namespace {
// Only for a player who uses the style, and a few times at most: the scan is not cheap.
void start_scan_if_due(State &s, std::uint64_t now) {
    if (!s.navigation_type.load(std::memory_order_acquire) && !s.scanning.load() && now >= s.next_scan && s.scans < max_scans &&
        style_layer::available() && (style_layer::enabled() || s.then)) {
        s.next_scan = now + scan_gap_ms;
        ++s.scans;
        start_scan(s);
    }
}
// The game makes its menu again on each map: the found type and an open or leave of the old map stop. An unknown map changes nothing.
void follow_map(State &s) {
    const bool busy = s.way != Way::none || s.then || s.leave_pressed || s.leave_steps_left.load(std::memory_order_relaxed);
    if (!busy && !s.navigation_type.load(std::memory_order_acquire) && !s.scanning.load()) return s.map.reset();
    const auto map = stage_map();
    if (!map) return;
    const bool changed = s.map && *map != *s.map;
    s.map = map;
    if (!changed) return;
    ++s.map_changes;
    s.navigation_type.store(0, std::memory_order_release);
    s.way = Way::none;
    s.then = nullptr;
    s.leave_steps_left.store(0, std::memory_order_relaxed);
    s.leave_pressed = false;
    if (!busy) return;
    overlay::cover({}, 0);
    logging::log(logging::Level::info, logging::Channel::skater, "Style editor: the map changed, so the menu's open or leave stopped.");
}
// True while a leave runs.
bool leave_step(State &s, std::uintptr_t base, std::uint64_t now) {
    if (!s.leave_steps_left.load(std::memory_order_relaxed) && !s.leave_pressed) return false;
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    const int left = std::clamp(s.leave_steps_left.load(std::memory_order_relaxed), 0, static_cast<int>(way_back.size()));
    if (left == static_cast<int>(way_back.size()) && !s.leave_pressed) {
        // An open that this leave stops takes its long cover with it.
        if (s.way != Way::none || s.then) overlay::cover("Back to the world", leave_cover_ms);
        s.way = Way::none;
        s.then = nullptr;
    }
    if (s.leave_pressed) {
        if (in_menu && now < s.leave_step_deadline) return true;
        s.leave_pressed = false;
        return true;
    }
    if (!in_menu || left == 0) {
        s.leave_steps_left.store(0, std::memory_order_relaxed);
        return true;
    }
    const auto name = way_back[way_back.size() - static_cast<std::size_t>(left)];
    if (!navigate(base, s, name).empty()) {
        s.leave_steps_left.store(0, std::memory_order_relaxed);
        return true;
    }
    s.leave_pressed = true;
    s.leave_steps_left.fetch_sub(1, std::memory_order_relaxed);
    s.leave_step_deadline = now + leave_wait_ms;
    return true;
}
void step(State &s, std::uintptr_t base, bool ready, std::function<void()> &then) {
    if (!ready || !base) return;
    const auto now = GetTickCount64();
    {
        std::lock_guard lock(s.mutex);
        follow_map(s);
        start_scan_if_due(s, now);
        if (leave_step(s, base, now)) return;
        if (s.then || s.way != Way::none) style_layer::watch_room();
        if (s.then && style_layer::room_present() && s.way == Way::none) then = std::exchange(s.then, nullptr);
        else if (s.then && now > s.then_deadline) fail(s, "the menu room's skater did not appear");
        else if (s.way != Way::none) step_way(base, s, now);
    }
    if (!then) return;
    then();
    then = nullptr;
    overlay::cover("Opening the style editor", screen_cover_ms);
}
// A failure that repeats would keep the cover on: the open ends, and an open that was under way leaves the menu.
bool stop_after_error(State &s, bool then_threw) {
    std::lock_guard lock(s.mutex);
    const bool opening = s.way != Way::none || s.then || then_threw;
    const bool busy = opening || s.leave_pressed || s.leave_steps_left.load(std::memory_order_relaxed);
    s.way = Way::none;
    s.then = nullptr;
    s.leave_steps_left.store(opening ? static_cast<int>(way_back.size()) : 0, std::memory_order_relaxed);
    s.leave_pressed = false;
    overlay::cover({}, 0);
    return busy;
}
} // namespace

void tick(std::uintptr_t base, bool ready) noexcept {
    auto &s = state();
    // Declared here, so that the stop sees a `then` that threw.
    std::function<void()> then;
    style::guarded_tick(logging::Channel::skater, "Style editor: the menu room's open or leave", [&] { step(s, base, ready, then); },
                        [&] { return stop_after_error(s, then != nullptr); });
}
} // namespace dingosdk::menu_room
