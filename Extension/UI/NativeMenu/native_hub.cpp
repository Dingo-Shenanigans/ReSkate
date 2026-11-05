#include "native_menu.h"
#include "native_menu_internal.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>

// The pause menu's Hub page lists live-service tiles in two native TileLists.
// ReSkate has no store, skate.Pass, bounties, missions or crew bounties, and
// draws its own front page over the Hub (Extension/UI/Overlay/hub_home_page.cpp:
// news, patch notes, the profile), so every tile goes. The game keeps the
// controller while its menu is up and needs something to focus on the Hub, so
// one tile stays, emptied in place: drawn as nothing, with no action. The game
// rebuilds the Hub now and then; this is re-applied then. Where the Hub's lists
// are not found the Hub is left as the game made it, and the front page is not
// drawn over it.
namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace {
constexpr std::uint32_t tile_list_schema = 0x74b1ea1d, tile_items = 0x67223da7;
// TileListItem -> presenter -> {button, content widget}.
constexpr std::uint32_t tile_presenter = 0x214d4984, tile_button = 0x0fb0d794, tile_content = 0x214d4984;
constexpr std::uint32_t navigation = 0xb1e14cd6, delegate = 0x9c76b86c;
constexpr std::uint32_t action_active = 0x4b3146e9, limited_interactivity = 0x67212c85, layer_blueprint = 0x8d1441c7;
constexpr std::uint32_t default_index = 0x1600d48e;
Value interact(const Context &c, Value item) {
    return c.path(item, {tile_presenter, tile_button, 0xa704272a, 0xc52416ef, 0xa704272a});
}
// The tile's background comes from its button style, which every Hub tile
// shares. A hidden tile gets its own copy with every fill (idle, focus and
// the rest) set to the game's empty fill, so nothing of it is drawn.
constexpr Schema button_style{0x3d13ff54, 120};
constexpr std::uint32_t button_style_ref = 0x8cc042ef, state_fills = 0x2d393a3a, focus_fills = 0x343abac3;
constexpr std::array<std::uint32_t, 7> state_fill_fields{0xb6c6aace, 0xe8f882b7, 0x69cf50ca, 0x182004ff,
                                                         0xfadedb6a, 0x24996c65, 0xb0db14be};
constexpr std::array<std::uint32_t, 3> focus_fill_fields{0xf622ea66, 0x943abf2d, 0x287f4815};
constexpr std::uint64_t empty_style_id = 0x52534d5048554253ULL; // "RSMPHUBS"

Value empty_style;
Address empty_style_manager{};
Value empty_tile_style(const Context &c, Value style_ref) {
    auto &style = empty_style;
    auto &manager = empty_style_manager;
    if (style.handle && manager == c.manager && c.type_of(style.handle) == style.type) return style;
    // Copy the tile's own style so everything but the fills stays as authored.
    const auto current = read<Ref>(c.address(style_ref));
    const Value source{current.handle, current.handle ? c.type_of(current.handle) : 0};
    require(source.handle && source.type == c.type(button_style), "Hub tile style differs.");
    // The authored tile style already uses the empty fill for its disabled state.
    const auto empty = read<Address>(c.address(c.path(source, {state_fills, 0x69cf50ca})));
    require(empty && string(read<Address>(empty + 0x18), 96) == "UI/Foundations/Styles/FIll/Common/FillStyle_Empty",
            "Hub empty fill is unavailable.");
    style = c.create(button_style, empty_style_id);
    manager = c.manager;
    c.copy(style, c.address(source));
    for (const auto field : state_fill_fields) c.set(c.path(style, {state_fills, field}), empty);
    for (const auto field : focus_fill_fields) c.set(c.path(style, {focus_fills, field}), empty);
    return style;
}
// A tile hidden earlier has lost its title and action, so it is known by the
// empty style it was given.
bool already_hidden(const Context &c, Value item) {
    if (!empty_style.handle || empty_style_manager != c.manager) return false;
    return read<Ref>(c.address(c.path(item, {tile_presenter, tile_button, button_style_ref}))).handle == empty_style.handle;
}
// Keeps the tile's slot, size included, so the tiles after it stay where they
// were; it just shows and does nothing. Runs every frame, so each field is
// written only when it isn't hidden already.
void hide_in_place(const Context &c, Value item) {
    const auto style_ref = c.path(item, {tile_presenter, tile_button, button_style_ref});
    const auto style = empty_tile_style(c, style_ref);
    if (read<Ref>(c.address(style_ref)).handle != style.handle) c.set(style_ref, Ref{0, style.handle});
    const auto tile_widget = c.path(item, {tile_presenter, tile_content});
    const auto shown = read<Widget>(c.address(tile_widget));
    if (shown.blueprint || shown.data.record || shown.data.handle) c.set(tile_widget, Widget{});
    // An inactive tile draws the "limited interactivity" layer (a gray panel
    // with a scribble, LimitedInteractivity_Default_Rough); without its widget
    // blueprint nothing is drawn.
    const auto limited_layer = c.path(item, {tile_presenter, limited_interactivity, layer_blueprint});
    if (read<Address>(c.address(limited_layer))) c.set(limited_layer, Address{});
    const auto handler = interact(c, item);
    const auto action = c.field(handler, 0xa2b71c93);
    if (!c.text(c.field(action, navigation), 64).empty()) c.text(c.field(action, navigation), "");
    if (read<Address>(c.address(c.field(action, delegate)))) c.set(c.field(action, delegate), Address{});
    if (read<bool>(c.address(c.field(handler, action_active)))) c.set(c.field(handler, action_active), false);
}
// With every tile but one gone, the Hub's partition
// (HorizontalPartition_LinearFocus_Widget) must still start on its left side,
// which an empty side isn't: the one list that keeps its tile goes on the left
// and the right side is left empty.
constexpr std::uint32_t partition_schema = 0xf0b42cc6, partition_left = 0xf750f0a2, partition_right = 0x29023866;
constexpr std::string_view hub_list_record = "HUB_Base_ContentResources/HUB_TileList";
// Record names elsewhere can be long; only the prefix is compared.
bool named_like(Address record, std::string_view prefix) {
    const auto name = read<Address>(record + 0x28);
    if (!name) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (read<char>(name + i) != prefix[i]) return false;
    return true;
}
// The tile count of a Hub tile list shown in a partition side, or nothing.
std::optional<unsigned> hub_list_tiles(const Context &c, const Widget &widget, Address list_type) {
    if (!widget.blueprint || !widget.data.record || !widget.data.handle) return std::nullopt;
    if (!named_like(widget.data.record, hub_list_record) || c.type_of(widget.data.handle) != list_type) return std::nullopt;
    unsigned count{}, stride{};
    c.array(c.field(Value{widget.data.handle, list_type}, tile_items), 32, count, stride);
    return count;
}
// Each Hub list is anchored toward the centre split: the right list with
// ListAnchorPoint 1, the left one with 0. A moved list takes the left one's.
constexpr std::uint32_t list_anchor_point = 0xb9eae335;
constexpr std::int32_t left_list_anchor = 0;
// Every tile of the list hidden in place, and all but the first dropped.
// Runs every frame, so a list already like that is left alone.
void empty_hub_list(const Context &c, Value list) {
    const auto rows = c.field(list, tile_items);
    unsigned count{}, stride{};
    c.array(rows, 32, count, stride);
    if (!count) return;
    for (unsigned i = 0; i < count; ++i)
        if (const auto item = c.element(rows, i); !already_hidden(c, item)) hide_in_place(c, item);
    c.set_if_changed(c.field(list, default_index), std::int32_t{0});
    c.set_if_changed(c.field(list, list_anchor_point), left_list_anchor);
    if (count == 1) return;
    // Hiding a tile clears its inline navigation CString, delegate, content and
    // style refs. A row snapshot from before would borrow released values and
    // undo those edits when republished.
    unsigned live_count{}, live_stride{};
    const auto bytes = c.array(rows, 32, live_count, live_stride);
    require(live_count == count && live_stride == stride, "Native Hub rows changed while hiding tiles.");
    std::vector<std::byte> kept(bytes.begin(), bytes.begin() + stride);
    c.array(rows, kept, 1);
}
// Returns whether the Hub's lists were found (and so emptied).
bool take_over_hub(const Context &c) {
    const auto list_type = c.type({tile_list_schema, 392});
    bool found{};
    for (const auto &root : c.roots({partition_schema})) {
        const auto left = c.field(root.model, partition_left), right = c.field(root.model, partition_right);
        auto left_widget = read<Widget>(c.address(left));
        const auto right_widget = read<Widget>(c.address(right));
        const auto left_tiles = hub_list_tiles(c, left_widget, list_type), right_tiles = hub_list_tiles(c, right_widget, list_type);
        if (!left_tiles && !right_tiles) continue;
        found = true;
        if ((!left_tiles || !*left_tiles) && right_tiles && *right_tiles) {
            c.set(left, right_widget);
            left_widget = right_widget;
        }
        if (right_tiles) c.set(right, Widget{});
        if (hub_list_tiles(c, left_widget, list_type)) empty_hub_list(c, Value{left_widget.data.handle, list_type});
    }
    return found;
}

// ---- the front page over the Hub (hub_home_page.cpp)

constexpr std::string_view hub_tab_label = "ID_MENU_HUB";
// Set while the Hub's lists were last found and emptied; the front page is only drawn then.
bool hub_taken{};
// The Hub's page model, while the Hub is the pause menu's selected tab; nothing when another tab is,
// and an empty value when the Hub is but its page is not a page model of the kind ReSkate's are.
std::optional<Value> selected_hub(const Context &c, Value menu) {
    const auto list = c.path(menu, {content, items});
    unsigned count{}, stride{};
    c.array(list, 16, count, stride);
    const auto selected = read<int>(c.address(c.path(menu, {content, 0x18f8355b})));
    if (selected < 0 || static_cast<unsigned>(selected) >= count) return std::nullopt;
    const auto item = c.element(list, static_cast<unsigned>(selected));
    if (c.text(c.path(item, {tab_data, label, text_field}), 64) != hub_tab_label) return std::nullopt;
    const auto widget = read<Widget>(c.address(c.field(item, content)));
    const Value model{widget.data.handle, widget.data.handle ? c.type_of(widget.data.handle) : 0};
    return model.handle && model.type == c.type(page) ? model : Value{};
}

// The Hub's Back held while the front page has something open (hold_hub_back): its input and
// navigation taken off, as the Mod Manager's own page does with its Back, and put back as they were.
std::atomic<std::uint64_t> back_wanted_until{};
struct HeldBack {
    Value page;
    Address input{};
    std::string navigation;
    bool active{};
} held_back;
Value back_base(const Context &c, Value page_model) {
    return c.path(page_model, {0x10810f0b, button_field, 0xa704272a, 0xc52416ef, 0xa704272a});
}
void hold_back(const Context &c, Value page_model, bool held) {
    if (held_back.page.handle && (!held || held_back.page.handle != page_model.handle)) {
        // (A page model the game has since let go of has nothing to put back.)
        if (c.type_of(held_back.page.handle) == held_back.page.type) {
            const auto base = back_base(c, held_back.page), back = c.field(base, 0xa2b71c93);
            c.set(c.field(back, 0x3d00de27), held_back.input);
            c.text(c.field(back, 0xb1e14cd6), held_back.navigation);
            c.set(c.field(base, 0x4b3146e9), held_back.active);
        }
        held_back = {};
    }
    if (!held || held_back.page.handle || !page_model.handle) return;
    const auto base = back_base(c, page_model), back = c.field(base, 0xa2b71c93);
    held_back = {page_model, read<Address>(c.address(c.field(back, 0x3d00de27))), c.text(c.field(back, 0xb1e14cd6), 64),
                 read<bool>(c.address(c.field(base, 0x4b3146e9)))};
    // Active with no input and no navigation: it swallows Escape and B (native_menu.cpp build_back_action).
    c.set(c.field(back, 0x3d00de27), Address{});
    c.text(c.field(back, 0xb1e14cd6), "");
    c.set(c.field(base, 0x4b3146e9), true);
}

// EDIT PROFILE: what the Hub's Profile tile did (its navigation), and failing that what the top
// bar's player card button does, each given a moment to take the menu off the Hub. Which one works
// from the Hub is said in the log. Neither: the overlay presses View for the player.
constexpr std::array<std::string_view, 2> profile_navigations{"HUB_Nav_CollectionScore", "GoToMyPlayerCard"};
constexpr std::uint64_t profile_wait_ms = 700, profile_give_up_ms = 2000;
std::atomic<bool> profile_wanted{};
std::atomic<std::uint64_t> profile_failed_at{}; // GetTickCount64; 0: not failed
struct ProfileWalk {
    bool active{};
    std::size_t step{};
    std::uint64_t asked{}, sent{}; // GetTickCount64: when this step began, and when its navigation went
} profile_walk;
// As QueueStateNavigation does (Extension/Throwdowns/native_flag_session.cpp navigate): the
// navigation's name hash and, for a navigation without arguments, metadata [-1]. False while the
// game's own navigation is still on its way.
bool navigate(const Context &c, std::string_view name) {
    const auto roots = c.roots({0xcc4776b5});
    require(roots.size() == 1, "Native navigation model is unavailable.");
    const auto current = c.field(roots.front().model, 0xf3ba9cf8);
    if (read<std::uint32_t>(c.address(current))) return false;
    c.set(current, game::native_name_hash(name));
    c.array(c.field(roots.front().model, 0xf437b255), std::vector<std::int32_t>{-1});
    logging::log(logging::Level::info, logging::Channel::ui, "Front page: EDIT PROFILE sent the navigation {}.", name);
    return true;
}
void walk_to_profile(const Context &c, bool on_hub) {
    const auto now = GetTickCount64();
    if (profile_wanted.exchange(false) && !profile_walk.active) profile_walk = {true, 0, now, 0};
    auto &w = profile_walk;
    if (!w.active) return;
    if (!on_hub) {
        if (w.sent) logging::log(logging::Level::info, logging::Channel::ui, "Front page: {} took the menu off the Hub.", profile_navigations[w.step]);
        w = {};
        return;
    }
    if (w.sent && now < w.sent + profile_wait_ms) return;
    if (w.sent) {
        logging::log(logging::Level::info, logging::Channel::ui, "Front page: {} left the menu on the Hub.", profile_navigations[w.step]);
        ++w.step, w.sent = 0, w.asked = now;
    }
    const bool late = now >= w.asked + profile_give_up_ms;
    if (w.step >= profile_navigations.size() || late) {
        if (late) logging::log(logging::Level::info, logging::Channel::ui, "Front page: the game's navigation stayed busy; EDIT PROFILE gave up on it.");
        w = {};
        profile_failed_at = now;
        return;
    }
    if (navigate(c, profile_navigations[w.step])) w.sent = now;
}
} // namespace

// Runs every frame while the pause menu is open, so the tiles never get a
// frame on screen when the Hub is rebuilt. Never lets the Hub disturb
// ReSkate's own tabs: a failure is logged once and retried later.
void update_hub(const Context &c) {
    static std::uint64_t next_update{};
    static std::string last_error;
    const auto now = GetTickCount64();
    if (now < next_update) return;
    try {
        hub_taken = take_over_hub(c);
        last_error.clear();
    } catch (const std::exception &e) {
        hub_taken = false;
        next_update = now + 2000;
        if (last_error != e.what()) {
            last_error = e.what();
            logging::log(logging::Level::warning, logging::Channel::ui, "Hub tiles: {}", e.what());
        }
    }
}
// Whether ReSkate's front page is drawn over the Hub now: the Hub is the selected tab with the
// player on it (not under the game's side menu), and its lists were emptied. Holds its Back and
// walks to the profile for the overlay meanwhile. Called whatever page is up (`in_menu` false when
// it is not the Hub's turn), so a held Back is always given back.
bool update_hub_front_page(const Context &c, Value menu, bool in_menu) {
    std::optional<Value> hub;
    try {
        if (in_menu) hub = selected_hub(c, menu);
    } catch (const std::exception &) {
        hub.reset();
    }
    bool on_hub = hub && hub_taken;
    if (on_hub && hub->handle) {
        try {
            // No big brushed title: the front page starts where it was, as ReSkate's own pages do.
            if (const auto title = c.path(*hub, {0x3781b603, text_field}); c.text(title, 64) != " ") c.text(title, " ");
            // (The page's screen record, and in it the focused index: -1 under the game's side menu.)
            on_hub = read<int>(c.address(c.path(*hub, {0xfafda1a6, 0x20a39d92, 0x6b01fd01}))) >= 0;
        } catch (const std::exception &e) {
            static bool said{};
            if (!std::exchange(said, true))
                logging::log(logging::Level::warning, logging::Channel::ui,
                             "Front page: the Hub's page could not be read ({}); it is drawn whenever the Hub is the tab.", e.what());
        }
    }
    try {
        hold_back(c, hub ? *hub : Value{}, on_hub && GetTickCount64() < back_wanted_until.load(std::memory_order_relaxed));
    } catch (const std::exception &e) {
        held_back = {};
        logging::log(logging::Level::warning, logging::Channel::ui, "Front page: the Hub's Back could not be held ({}).", e.what());
    }
    try {
        walk_to_profile(c, on_hub);
    } catch (const std::exception &e) {
        profile_walk = {};
        profile_failed_at = GetTickCount64();
        logging::log(logging::Level::warning, logging::Channel::ui, "Front page: EDIT PROFILE could not navigate ({}).", e.what());
    }
    return on_hub;
}

void open_native_profile() noexcept { profile_failed_at = 0, profile_wanted = true; }
// (Only just now: one the overlay did not see before the menu closed is not acted on the next time.)
bool native_profile_failed() noexcept {
    const auto at = profile_failed_at.exchange(0);
    return at && GetTickCount64() - at < 1000;
}
void hold_hub_back(bool held) noexcept { back_wanted_until.store(held ? GetTickCount64() + 300 : 0, std::memory_order_relaxed); }
} // namespace dingosdk::multiplayer
