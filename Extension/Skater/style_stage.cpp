#include "style_stage.h"
#include "style_editor.h"
#include "style_internal.h"
#include "style_layer.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace dingosdk::style_stage {
namespace {
namespace menu_data = multiplayer::menu_data;
namespace model = addr::style;

// Skatepedia's title of each flip trick's entry, by trick number: other entries share the numbers.
constexpr std::array<std::string_view, 33> entry_titles{
    "", "OLLIE", "KICKFLIP", "HEELFLIP", "BS_POPSHUVIT", "VARIAL_KICKFLIP", "INWARDHEEL", "FS_POPSHUVIT", "VARIAL_HEELFLIP", "HARDFLIP", "360_POPSHUVIT",
    "360FLIP", "360_INWARDHEEL", "FS_360_POPSHUVIT", "LASERFLIP", "360_HARDFLIP", "NOLLIE", "NOLLIE_KICKFLIP", "NOLLIE_HEELFLIP", "NOLLIE_BS_POPSHUVIT",
    "N_HARDFLIP", "N_VARIALHEELFLIP", "NOLLIE_FS_POPSHUVIT", "N_VARIALKICKFLIP", "N_INWARDHEELFLIP", "N_FS360POPSHUVIT", "N_360HARDFLIP", "N_LASERFLIP",
    "N_360POPSHUVIT", "N_360FLIP", "N_360INWARDHEEL", "QUICKOLLIE", "QUICKNOLLIE"};
constexpr std::string_view title_prefix = "ID_SKATEPEDIA_";
// A part-of-the-board entry: Skatepedia shows the board alone for it.
constexpr std::string_view parked_entry = "ID_SKATEPEDIA_DECK";
// The way back: Skatepedia's Escape, then the settings page's and the pause menu's Back navigations. Found in a UI model dump on 2026-10-04.
constexpr std::array<std::string_view, 3> way_back{"Escape", "Back", "ToggleMenu"};

enum class Way { none, look, wait_stage, wait_closed, open, wait_open, settings, wait_settings, tile, wait_tile };
enum class Seek { none, find, set, learn };

struct State {
    std::mutex mutex;
    // The navigation event's type object, found in memory one time.
    std::atomic<std::uintptr_t> navigation_type{};
    std::atomic<bool> scanning{};
    std::uint64_t next_scan{};
    int scans{};       // attempts started: the scan costs a core for a long time
    WORD key_down{};   // a key held since the last tick, to release
    Way way{};
    std::uint64_t way_at{}, way_deadline{}, way_started{};
    int way_retries{}; // Skatepedia's navigation sent again
    Seek seek{};
    std::uint8_t seek_trick{};
    int seek_presses{}, seek_tabs{}, seek_retries{};
    std::uint64_t seek_at{}, seek_deadline{};
    bool nudged{}; // the highlight was already moved once this visit
    std::atomic<int> leave{};
    bool leave_pressed{}, leave_told{}, leave_retried{};
    std::uint64_t leave_wait{}; // the fallback time of the current step
    std::function<void()> then;
    std::uint64_t then_deadline{};
};
State &state() { static auto *value = new State; return *value; }

// Client thread only. The model of Skatepedia's highlighted entry, and the last name written to it.
std::uint64_t current_entry{};
std::string written_name, forced_title;
std::uint8_t forced_trick{};
std::vector<std::uint64_t> candidates;
std::string candidates_title;

std::uint8_t trick_of_title(std::string_view title) noexcept {
    if (!title.starts_with(title_prefix)) return 0;
    title.remove_prefix(title_prefix.size());
    for (std::size_t i = 1; i < entry_titles.size(); ++i)
        if (title == entry_titles[i]) return static_cast<std::uint8_t>(i);
    return 0;
}
std::uintptr_t model_manager(std::uintptr_t base) {
    const auto ui = menu_data::read<std::uintptr_t>(base + addr::engine::ui_manager);
    return ui ? menu_data::read<std::uintptr_t>(ui + model::ui_model_manager) : 0;
}
std::string highlighted_title(const menu_data::Context &context) {
    for (const auto &root : context.roots({model::info_card}))
        if (auto title = context.text(context.path(root.model, {model::info_card_title, model::label_text})); !title.empty()) return title;
    return {};
}
std::string name_of(const menu_data::Context &context, const menu_data::Root &root) {
    return context.text(context.field(root.model, model::skatepedia_entry_name));
}
// Before the highlight moves: records the entries that have the highlighted name.
void note_candidates(std::uintptr_t base) noexcept {
    candidates.clear();
    candidates_title.clear();
    try {
        const auto manager = model_manager(base);
        if (!manager) return;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        candidates_title = highlighted_title(context);
        if (candidates_title.empty()) return;
        for (const auto &root : context.roots({model::skatepedia_entry}))
            if (name_of(context, root) == candidates_title) candidates.push_back(root.model.handle);
    } catch (...) {}
}
// After the highlight moved: the candidate that has the new name is the highlighted entry's model.
void find_current(std::uintptr_t base) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        const auto title = highlighted_title(context);
        if (title.empty() || title == candidates_title) return;
        for (const auto &root : context.roots({model::skatepedia_entry})) {
            if (std::ranges::find(candidates, root.model.handle) == candidates.end() || name_of(context, root) != title) continue;
            current_entry = root.model.handle;
            written_name.clear();
            return;
        }
    } catch (...) {}
}
// Copies the row named `wanted` into the highlighted entry's model. Returns empty on success, or "unknown" if that model is not valid.
std::string put_entry(std::uintptr_t base, std::string_view wanted, std::uint8_t trick) {
    try {
        const auto manager = model_manager(base);
        if (!manager) return "the menu's models are not available";
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        const auto title = highlighted_title(context);
        const std::string name = wanted.empty() ? title : std::string(wanted);
        const auto entries = context.roots({model::skatepedia_entry});
        const menu_data::Root *current{}, *source{};
        for (const auto &root : entries) {
            if (root.model.handle == current_entry) current = &root;
            else if (!source && name_of(context, root) == name) source = &root;
        }
        // The model must still have the highlighted name or the last written name.
        if (const auto held = current ? name_of(context, *current) : std::string{}; !current || title.empty() || (held != title && held != written_name)) {
            current_entry = 0;
            return "unknown";
        }
        if (!source) return "the game has no entry for it";
        context.copy(current->model, context.address(source->model));
        written_name = name;
        forced_trick = trick;
        forced_title = title;
        return {};
    } catch (const std::exception &error) {
        return error.what();
    }
}
// The menu's current navigation as a name hash, or 0.
std::uint32_t current_navigation(std::uintptr_t base) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return 0;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        for (const auto &root : context.roots({model::navigation_queue}))
            return menu_data::read<std::uint32_t>(context.address(context.field(root.model, model::navigation_current)));
    } catch (...) {}
    return 0;
}
// True while Skatepedia's page has its entry models.
bool skatepedia_open(std::uintptr_t base) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return false;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        return !context.roots({model::skatepedia_entry}).empty();
    } catch (...) {}
    return false;
}
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
void send_key(WORD key, bool up) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(INPUT));
}
// The game window has the keyboard: key presses go to the game and to nothing else.
bool game_has_keyboard() noexcept {
    DWORD process{};
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() && overlay::keyboard_shortcuts_allowed();
}
// Presses a key as the keyboard does. The next tick releases it. False if the game does not have the keyboard.
bool press_key(State &s, WORD key) noexcept {
    if (!game_has_keyboard()) return false;
    send_key(key, false);
    s.key_down = key;
    return true;
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
    // A menu that this opened does not stay open. After the key presses, `then` shows that an open is in progress.
    if (s.way != Way::none || s.then) s.leave.store(2, std::memory_order_relaxed);
    s.way = Way::none;
    s.seek = Seek::none;
    s.seek_trick = 0;
    s.then = nullptr;
    style_editor::expect(0);
    overlay::cover({}, 0);
    logging::log(logging::Level::warning, logging::Channel::skater, "Style editor: {}.", why);
    overlay::notify(overlay::NoticeLevel::warning, "Style editor", std::string(why) + ".");
}
// Opens Skatepedia from any game state, through the pause menu and its settings page.
void step_way(std::uintptr_t base, State &s, std::uint64_t now) {
    const auto next = [&](Way way, std::uint64_t wait, std::uint64_t deadline = 0) {
        s.way = way;
        s.way_at = now + wait;
        s.way_started = now;
        s.way_deadline = now + deadline;
    };
    const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
    // Each step waits until the game has run the queued navigation. The fallback time applies only when the queue does not empty.
    const bool settled = !navigation_pending(base) || now > s.way_started + 1500;
    if (style_layer::stage_present()) s.way = Way::none;
    else if (!s.navigation_type.load(std::memory_order_acquire)) {
        if (!s.scanning.load() && now > s.way_deadline) fail(s, "The editor could not open: the game's menu was not found");
    } else if (now < s.way_at) {
    } else if (s.way == Way::look) {
        // In Skatepedia between two loops, its stage comes back. Another menu closes first.
        if (in_menu && current_navigation(base) == game::native_name_hash("Settings_Skatepedia")) next(Way::wait_stage, 0, 1500);
        else if (in_menu) {
            (void)navigate(base, s, "ToggleMenu");
            next(Way::wait_closed, 0, 4000);
        } else next(Way::open, 0);
    } else if (s.way == Way::wait_stage) {
        if (now > s.way_deadline) {
            (void)navigate(base, s, "ToggleMenu");
            next(Way::wait_closed, 0, 4000);
        }
    } else if (s.way == Way::wait_closed) {
        if (!in_menu && settled) next(Way::open, 0);
        else if (now > s.way_deadline) fail(s, "The editor could not open: the game's menu did not close");
    } else if (s.way == Way::open) {
        (void)navigate(base, s, "ToggleMenu");
        next(Way::wait_open, 0, 4000);
    } else if (s.way == Way::wait_open) {
        if (in_menu && settled) next(Way::settings, 0);
        else if (now > s.way_deadline) fail(s, "The editor could not open: the pause menu did not open");
    } else if (s.way == Way::settings) {
        if (!navigate(base, s, "GoToSettings")) return fail(s, "The editor could not open: the settings page did not open");
        next(Way::wait_settings, 0);
    } else if (s.way == Way::wait_settings) {
        if (settled) next(Way::tile, 0);
    } else if (s.way == Way::tile) {
        // The settings page's Skatepedia tile sends this navigation. The first open of a session loads the stage for up to 15 s.
        (void)navigate(base, s, "Settings_Skatepedia");
        next(Way::wait_tile, 0, 15000);
    } else if (s.way == Way::wait_tile) {
        // The stage's appearance ends this step. Without it, the navigation is sent one more time.
        if (now <= s.way_deadline) return;
        if (s.way_retries++ < 1) {
            logging::log(logging::Level::info, logging::Channel::skater, "Style editor: Skatepedia did not open, so its navigation is sent again.");
            next(Way::tile, 0);
        } else fail(s, "The editor could not open: Skatepedia did not open");
    }
}
// In Skatepedia: finds the highlighted entry's model, then makes its skater perform the wanted trick.
void step_seek(std::uintptr_t base, State &s, std::uint64_t now) {
    const auto done = [&] {
        s.seek = Seek::none;
        s.seek_trick = 0;
    };
    const auto shown = shown_trick(base);
    if (!multiplayer::sample_game_ui_state(base).in_menu) {
        if (s.seek_trick) fail(s, std::format("The {} could not be loaded: the editor's stage closed", style::flip_trick_titles[s.seek_trick]));
        else done();
    } else if (s.seek == Seek::find) {
        s.nudged = true;
        if (!shown && !current_entry && s.seek_tabs < 4 && s.seek_presses == 0) {
            // Go to the tab that lists flip tricks first.
            ++s.seek_tabs;
            (void)press_key(s, 'E');
            s.seek_at = now + 900;
        } else if (current_entry && s.seek_presses % 2 == 0) s.seek = Seek::set;
        else if (s.seek_presses >= 6) {
            if (s.seek_trick) fail(s, std::format("The {} could not be loaded: the editor's stage was not ready", style::flip_trick_titles[s.seek_trick]));
            else done();
        } else {
            // One step and back. The second attempt uses the opposite direction, in case the list ended.
            const bool up_first = (s.seek_presses / 2) % 2 != 0;
            if (s.seek_presses % 2 == 0) note_candidates(base);
            else find_current(base);
            (void)press_key(s, (s.seek_presses % 2 == 0) != up_first ? WORD{'S'} : WORD{'W'});
            ++s.seek_presses;
            s.seek_at = now + 600;
        }
    } else if (s.seek == Seek::set) {
        if (!s.seek_trick) return done();
        const auto failure = put_entry(base, std::string(title_prefix) + std::string(entry_titles[s.seek_trick]), s.seek_trick);
        if (failure == "unknown" && s.seek_retries < 2) {
            ++s.seek_retries;
            s.seek = Seek::find;
            s.seek_presses = 0;
        } else if (failure.empty()) {
            s.seek = Seek::learn;
            s.seek_deadline = now + 40000;
            style_editor::expect(s.seek_trick);
        } else fail(s, std::format("The {} could not be loaded: {}", style::flip_trick_titles[s.seek_trick], failure == "unknown" ? "the editor's stage was not ready" : failure));
    } else if (s.seek == Seek::learn) {
        if (style_editor::has_clip(s.seek_trick)) done();
        else if (now > s.seek_deadline) fail(s, std::format("The {} could not be loaded: its animation was not captured", style::flip_trick_titles[s.seek_trick]));
        else s.seek_at = now + 250;
    }
}
} // namespace

std::uint8_t shown_trick(std::uintptr_t base, std::string *shown) noexcept {
    try {
        const auto manager = model_manager(base);
        if (!manager) return 0;
        const menu_data::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        const auto title = highlighted_title(context);
        if (shown) *shown = title;
        if (title.empty()) {
            // Skatepedia is closed, so its known models are not valid.
            current_entry = 0;
            forced_trick = 0;
            return 0;
        }
        if (title != forced_title) {
            forced_trick = 0;
            return trick_of_title(title);
        }
        // A written entry stays valid until the highlight moves.
        return written_name == parked_entry ? std::uint8_t{} : forced_trick ? forced_trick : trick_of_title(title);
    } catch (...) {
        return 0;
    }
}
bool park(std::uintptr_t base, bool park) noexcept {
    return current_entry && put_entry(base, park ? parked_entry : std::string_view{}, 0).empty();
}
void open(std::function<void()> then) {
    auto &s = state();
    style_layer::watch_stage();
    if (style_layer::stage_present() && current_entry) return then();
    // Our menus would take the key presses that are for the game.
    overlay::close_menus();
    // The game's menus go past behind a cover.
    overlay::cover("Opening the style editor", 45000);
    const auto now = GetTickCount64();
    std::lock_guard lock(s.mutex);
    if (!style_layer::stage_present()) s.nudged = false;
    s.leave.store(0, std::memory_order_relaxed);
    s.scans = 0;
    s.then = std::move(then);
    s.then_deadline = now + 60000;
    if (s.way == Way::none) {
        s.way = Way::look;
        s.way_started = now;
        s.way_retries = 0;
        s.way_at = now + 400;
        s.way_deadline = now + 30000;
    }
}
void leave() noexcept {
    state().leave.store(static_cast<int>(way_back.size()), std::memory_order_relaxed);
    overlay::cover("Back to the world", 3000);
}
void fetch(std::uint8_t trick) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!trick || trick >= entry_titles.size()) return;
    // A new trick replaces the trick that was loading.
    s.seek_trick = trick;
    s.seek = Seek::find;
    s.seek_presses = s.seek_tabs = s.seek_retries = 0;
    s.seek_at = GetTickCount64();
    overlay::notify(overlay::NoticeLevel::info, "Style editor", std::format("Loading the {}. This takes about ten seconds. Then you can edit it.", style::flip_trick_titles[trick]));
}
void tick(std::uintptr_t base, bool ready) noexcept {
    auto &s = state();
    try {
        {
            // A pressed key is released also when the world is not ready, so that it does not stay down.
            std::lock_guard lock(s.mutex);
            if (const auto key = std::exchange(s.key_down, WORD{})) send_key(key, true);
        }
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
            if (s.leave.load(std::memory_order_relaxed)) {
                // Skatepedia gets the Escape key: only its Back button gives the skater's board sound back.
                const bool in_menu = multiplayer::sample_game_ui_state(base).in_menu;
                const auto done = [&] {
                    s.leave.store(0, std::memory_order_relaxed);
                    s.leave_pressed = s.leave_told = s.leave_retried = false;
                };
                const int left = std::clamp(s.leave.load(std::memory_order_relaxed), 0, static_cast<int>(way_back.size()));
                const auto step = way_back.size() - static_cast<std::size_t>(left); // the next step
                if (s.leave_pressed) {
                    // Each step waits for its event. The time is the fallback.
                    const bool ran = step == 1 ? !skatepedia_open(base) : !navigation_pending(base);
                    if (in_menu && !ran && now < s.leave_wait) return;
                    s.leave_pressed = false;
                    // A lost Escape: Skatepedia gets one more.
                    if (step == 1 && in_menu && !ran && !std::exchange(s.leave_retried, true)) s.leave.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!in_menu || left == 0) return done();
                // Skatepedia gets its highlighted entry back first.
                if (current_entry && !written_name.empty()) (void)put_entry(base, {}, 0);
                written_name.clear();
                style_editor::expect(0);
                s.way = Way::none;
                s.seek = Seek::none;
                s.seek_trick = 0;
                s.then = nullptr;
                if (step > 0) {
                    (void)navigate(base, s, way_back[step]);
                    s.leave_pressed = true;
                } else if (skatepedia_open(base)) {
                    // With Escape as a ReSkate key, the player presses Back. The press also waits until the game has the keyboard.
                    if (const auto keys = launcher::overlay_keys(); keys.menu == VK_ESCAPE || keys.console == VK_ESCAPE) {
                        if (!std::exchange(s.leave_told, true))
                            overlay::notify(overlay::NoticeLevel::info, "Style editor", "Press Back to leave Skatepedia.");
                        return;
                    }
                    if (!press_key(s, VK_ESCAPE)) return;
                    s.leave_pressed = true;
                }
                s.leave.fetch_sub(1, std::memory_order_relaxed);
                s.leave_wait = now + 2000;
                return;
            }
            if (s.then || s.way != Way::none || s.seek != Seek::none) style_layer::watch_stage();
            const bool on_stage = style_layer::stage_present();
            // On the stage, the highlighted entry's model is found before the editor opens.
            if (s.then && on_stage && s.way == Way::none && s.seek == Seek::none && !current_entry && !s.nudged) {
                s.seek = Seek::find;
                s.seek_presses = s.seek_tabs = s.seek_retries = 0;
                s.seek_at = now + 1500;
                s.then_deadline = now + 30000;
                overlay::close_menus();
            }
            if (s.then && on_stage && s.way == Way::none && s.seek == Seek::none) then = std::exchange(s.then, nullptr);
            else if (s.then && now > s.then_deadline) fail(s, "The editor could not open: its stage did not appear");
            else if (s.seek != Seek::none) {
                if (now >= s.seek_at) step_seek(base, s, now);
            } else if (s.way != Way::none) step_way(base, s, now);
        }
        if (then) {
            then();
            // The cover stays while the editor screen comes up.
            overlay::cover("Opening the style editor", 900);
        }
    } catch (...) {}
}
} // namespace dingosdk::style_stage
