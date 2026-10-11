#include "style_stage.h"
#include "style_internal.h"
#include "style_layer.h"
#include "trick_pad.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dingosdk::style_stage {
namespace {
namespace menu_data = multiplayer::menu_data;
namespace model = addr::style;

// The menu's tab bar has up to seven tabs. RB goes to the next one. The press lasts 400 ms.
constexpr int max_tab_presses = 7;
constexpr std::uint64_t tab_press_ms = 400;
// The way back to the world: the Skaters page's Back closes the menu. ToggleMenu is for a menu that is still open.
constexpr std::array<std::string_view, 2> way_back{"Back", "ToggleMenu"};

enum class Way { none, look, wait_closed, open, wait_open, tab, wait_tab, wait_skater };

struct State {
    std::mutex mutex;
    // The navigation event's type object, found in memory one time.
    std::atomic<std::uintptr_t> navigation_type{};
    std::atomic<bool> scanning{};
    std::uint64_t next_scan{};
    int scans{}; // attempts started: the scan costs a core for a long time
    Way way{};
    std::uint64_t way_at{}, way_deadline{}, way_started{};
    int tab_presses{};
    std::string tab_page; // the page before the last press
    std::string seen_page;
    std::uint64_t seen_at{}; // the time the bar last changed
    std::atomic<int> leave{}; // the steps of way_back still to send
    bool leave_pressed{};
    std::uint64_t leave_wait{}; // the fallback time of the current step
    std::function<void()> then;
    std::uint64_t then_deadline{};
};
State &state() { static auto *value = new State; return *value; }

std::uintptr_t model_manager(std::uintptr_t base) {
    const auto ui = menu_data::read<std::uintptr_t>(base + addr::engine::ui_manager);
    return ui ? menu_data::read<std::uintptr_t>(ui + model::ui_model_manager) : 0;
}
// The navigations of the menu's button bar: these show which page is open. Empty without a bar. The bar holds only the buttons of
// the page that shows, but the page's other models stay after the page closes.
std::string menu_page(std::uintptr_t base) noexcept {
    std::string page;
    try {
        const auto manager = model_manager(base);
        if (!manager) return page;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
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
    } catch (...) {}
    return page;
}
// The Skaters page has the button for a new preset.
bool skaters_page(const std::string &page) noexcept { return page.find("CASPresetPurchase ") != std::string::npos; }
// True while the menu has a queued navigation that it has not run yet.
bool navigation_pending(std::uintptr_t base) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return false;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        for (const auto &root : context.roots({model::navigation_queue})) {
            unsigned count{}, stride{};
            (void)context.array(context.field(root.model, model::navigation_queued), 4, count, stride);
            return count > 0;
        }
    } catch (...) {}
    return false;
}
// Queues a named navigation as the menu's own buttons do.
bool queue_navigation(std::uintptr_t base, std::string_view name) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return false;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        const std::uint32_t hash = game::native_name_hash(name);
        for (const auto &root : context.roots({model::navigation_queue})) {
            context.array(context.field(root.model, model::navigation_queued), std::vector<std::uint32_t>{hash});
            context.set(context.field(root.model, model::navigation_current), hash);
            return true;
        }
    } catch (...) {}
    return false;
}
bool code_is(std::uintptr_t base, std::uintptr_t rva, const std::array<unsigned char, 32> &bytes) noexcept {
    std::array<unsigned char, 32> actual{};
    return memory::peek(base + rva, actual) && actual == bytes;
}
// True if the three natives that raise an event match their fingerprints.
bool dispatch_known(std::uintptr_t base) noexcept {
    namespace party = addr::native_party;
    unsigned matched{};
    for (const auto &contract : party::party_request_contracts)
        if (contract.rva == party::current_context || contract.rva == party::event_dispatcher || contract.rva == party::event_post)
            matched += code_is(base, contract.rva, contract.bytes);
    return matched == 3;
}
// Announces the queued navigation on the UI's event dispatcher. The event has no payload.
bool announce(std::uintptr_t base, std::uintptr_t type) noexcept {
    alignas(16) const std::array<std::byte, 64> payload{};
    return multiplayer::post_native_ui_event(base, type, payload.data());
}
bool navigate(std::uintptr_t base, State &s, std::string_view name) noexcept {
    const auto type = s.navigation_type.load(std::memory_order_acquire);
    const bool sent = type && dispatch_known(base) && queue_navigation(base, name) && announce(base, type);
    if (!sent) logging::log(logging::Level::warning, logging::Channel::skater, "Style editor: could not send the menu navigation {}.", name);
    return sent;
}
// A background thread finds the navigation event's type on the heap.
void start_scan(State &s) noexcept {
    if (s.scanning.exchange(true)) return;
    try {
        std::thread([] {
            auto &l = state();
            std::array<multiplayer::NativeTypeQuery, 1> query{{{model::navigation_changed_event, 0, 2}}};
            (void)multiplayer::find_native_types(query, true);
            l.navigation_type.store(query[0].object, std::memory_order_release);
            l.scanning.store(false);
        }).detach();
    } catch (...) {
        s.scanning.store(false);
    }
}
void fail(State &s, std::string_view why) {
    // A menu that this opened does not stay open.
    if (s.way != Way::none || s.then) s.leave.store(static_cast<int>(way_back.size()), std::memory_order_relaxed);
    s.way = Way::none;
    s.then = nullptr;
    overlay::cover({}, 0);
    logging::log(logging::Level::warning, logging::Channel::skater, "Style editor: {}.", why);
    overlay::notify(overlay::NoticeLevel::warning, "Style editor", std::string(why) + ".");
}
// Opens the menu's Skaters page from any game state: the menu opens on the page it showed last, and RB goes to the next page.
void step_way(std::uintptr_t base, State &s, std::uint64_t now) {
    const auto next = [&](Way way, std::uint64_t wait, std::uint64_t deadline = 0) {
        s.way = way;
        s.way_at = now + wait;
        s.way_started = now;
        s.way_deadline = now + deadline;
    };
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    const auto page = in_menu ? menu_page(base) : std::string{};
    const bool skaters = skaters_page(page);
    // A page's bar fills over a few frames: the page counts when its bar has not changed for 300 ms.
    if (page != s.seen_page) {
        s.seen_page = page;
        s.seen_at = now;
    }
    const bool page_shown = !page.empty() && now >= s.seen_at + 300;
    // Each step waits until the game has run the queued navigation. The fallback time applies only when the queue does not empty.
    const auto settled = [&] { return !navigation_pending(base) || now > s.way_started + 1500; };
    if (skaters && style_layer::stage_present()) s.way = Way::none;
    else if (!s.navigation_type.load(std::memory_order_acquire)) {
        if (!s.scanning.load() && now > s.way_deadline) fail(s, "The editor could not open: the game's menu was not found");
    } else if (now < s.way_at) {
    } else if (s.way == Way::look) {
        // On the Skaters page, the room's skater comes. Another menu closes first.
        if (skaters) next(Way::wait_skater, 0, 10000);
        else if (in_menu) {
            (void)navigate(base, s, "ToggleMenu");
            next(Way::wait_closed, 0, 4000);
        } else next(Way::open, 0);
    } else if (s.way == Way::wait_closed) {
        if (!in_menu && settled()) next(Way::open, 0);
        else if (now > s.way_deadline) fail(s, "The editor could not open: the game's menu did not close");
    } else if (s.way == Way::open) {
        (void)navigate(base, s, "ToggleMenu");
        next(Way::wait_open, 0, 4000);
    } else if (s.way == Way::wait_open) {
        if (in_menu && settled()) {
            s.tab_presses = 0;
            next(Way::tab, 0, 1500);
        } else if (now > s.way_deadline) fail(s, "The editor could not open: the menu did not open");
    } else if (s.way == Way::tab) {
        // The menu opens on the page it showed last, so it can be the Skaters page.
        if (skaters) next(Way::wait_skater, 0, 10000);
        else if (!in_menu) fail(s, "The editor could not open: the menu closed");
        else if (!page_shown && now < s.way_deadline) {
        } else if (s.tab_presses >= max_tab_presses) fail(s, "The editor could not open: the Skaters page did not open");
        else {
            std::string error;
            if (!trick_pad::play("steps", "0:0,0,200 150:0,0 400:0,0", error)) return fail(s, "The editor could not open: " + error);
            ++s.tab_presses;
            s.tab_page = page;
            next(Way::wait_tab, 0, 1500);
        }
    } else if (s.way == Way::wait_tab) {
        // The press ends when the bar shows another page. The time is the fallback.
        if (!in_menu) fail(s, "The editor could not open: the menu closed");
        else if ((now > s.way_started + tab_press_ms && page != s.tab_page) || now > s.way_deadline) next(Way::tab, 0, 1500);
    } else if (s.way == Way::wait_skater) {
        // The room's skater comes after the page. No press here: it would leave the page.
        if (!in_menu) fail(s, "The editor could not open: the menu closed");
        else if (now > s.way_deadline) fail(s, "The editor could not open: the menu's skater did not appear");
    }
}
} // namespace

void open(std::function<void()> then) {
    auto &s = state();
    style_layer::watch_stage();
    overlay::close_menus();
    // The game's menus go past behind a cover.
    overlay::cover("Opening the style editor", 45000);
    const auto now = GetTickCount64();
    std::lock_guard lock(s.mutex);
    // A leave that has sent nothing yet stops. A leave that has sent a step goes on, and the open comes after it.
    if (!s.leave_pressed && s.leave.load(std::memory_order_relaxed) == static_cast<int>(way_back.size())) s.leave.store(0, std::memory_order_relaxed);
    s.scans = 0;
    s.then = std::move(then);
    s.then_deadline = now + 60000;
    if (s.way == Way::none) {
        s.way = Way::look;
        s.way_started = now;
        s.way_at = now;
        s.way_deadline = now + 30000;
    }
}
void leave() noexcept {
    state().leave.store(static_cast<int>(way_back.size()), std::memory_order_relaxed);
    // A cover that shows already stays as it is.
    if (!overlay::covered()) overlay::cover("Back to the world", 3000);
}
bool idle() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.way == Way::none && !s.then && !s.leave_pressed && !s.leave.load(std::memory_order_relaxed);
}
void tick(std::uintptr_t base, bool ready) noexcept {
    auto &s = state();
    try {
        if (!ready || !base) return;
        const auto now = GetTickCount64();
        std::function<void()> then;
        {
            std::lock_guard lock(s.mutex);
            // Only for a player who uses the style, and a few times at most: the scan is not cheap.
            if (!s.navigation_type.load(std::memory_order_acquire) && !s.scanning.load() && now >= s.next_scan && s.scans < 3 &&
                (style_layer::enabled() || s.then)) {
                s.next_scan = now + 15000;
                ++s.scans;
                start_scan(s);
            }
            if (s.leave.load(std::memory_order_relaxed) || s.leave_pressed) {
                const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
                const int left = std::clamp(s.leave.load(std::memory_order_relaxed), 0, static_cast<int>(way_back.size()));
                // The leave stops an open that is in progress.
                if (left == static_cast<int>(way_back.size()) && !s.leave_pressed) {
                    s.way = Way::none;
                    s.then = nullptr;
                }
                if (s.leave_pressed) {
                    // The step ends when the menu closes. The time is the fallback.
                    if (in_menu && now < s.leave_wait) return;
                    s.leave_pressed = false;
                    return;
                }
                if (!in_menu || left == 0) {
                    s.leave.store(0, std::memory_order_relaxed);
                    return;
                }
                (void)navigate(base, s, way_back[way_back.size() - static_cast<std::size_t>(left)]);
                s.leave_pressed = true;
                s.leave.fetch_sub(1, std::memory_order_relaxed);
                s.leave_wait = now + 2000;
                return;
            }
            if (s.then || s.way != Way::none) style_layer::watch_stage();
            if (s.then && style_layer::stage_present() && s.way == Way::none) then = std::exchange(s.then, nullptr);
            else if (s.then && now > s.then_deadline) fail(s, "The editor could not open: its stage did not appear");
            else if (s.way != Way::none) step_way(base, s, now);
        }
        if (then) {
            then();
            // The cover stays while the editor screen comes up.
            overlay::cover("Opening the style editor", 900);
        }
    } catch (...) {}
}
} // namespace dingosdk::style_stage
