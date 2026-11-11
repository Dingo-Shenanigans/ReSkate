#include "native_menu.h"
#include "native_menu_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The pause menu's Hub page lists live-service tiles in two native TileLists.
// ReSkate has no store, skate.Pass, bounties, missions or crew bounties, and
// draws its own front page over the Hub (Extension/UI/Overlay/hub_home_page.cpp:
// news, patch notes, the profile), so every tile goes. The game keeps the
// controller while its menu is up and needs something to focus on the Hub, so
// one tile stays, emptied in place: drawn as nothing, with no action. The game
// rebuilds the Hub now and then (coming back to it from a page opened from it
// among others); this runs every frame, so it is re-applied then. Where the
// Hub's lists are not found the Hub is left as the game made it, and the front
// page is not drawn over it.
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
    // The game's empty fill: the tile style's disabled state when it uses it, else found by name from
    // the style's own fills (2026-11-04 in game the disabled state held another fill, and the Hub was
    // left as the game made it).
    const auto fill_name = [](Address fill) { return fill ? string(read<Address>(fill + 0x18), 96) : std::string(); };
    const auto is_empty_fill = [&](Address fill) {
        auto name = fill_name(fill);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return name.ends_with("/fillstyle_empty");
    };
    const auto disabled = read<Address>(c.address(c.path(source, {state_fills, 0x69cf50ca})));
    auto empty = is_empty_fill(disabled) ? disabled : Address{};
    for (const auto field : state_fill_fields) {
        if (empty) break;
        if (const auto anchor = read<Address>(c.address(c.path(source, {state_fills, field}))))
            for (const char *name : {"UI/Foundations/Styles/FIll/Common/FillStyle_Empty", "UI/Foundations/Styles/Fill/Common/FillStyle_Empty"})
                if (const auto found = named_asset(c, anchor, name); found && is_empty_fill(found)) {
                    empty = found;
                    break;
                }
    }
    if (!empty) throw std::runtime_error("Hub empty fill is unavailable (the tiles' disabled fill is \"" + fill_name(disabled) + "\").");
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
std::atomic<bool> front_page_on{true}; // set_hub_front_page
// The Hub tab's content is not a page model but a StackScreen (UI dumps of 2026-09-19: the Hub's tab
// shows UI/Foundations/Components/Stack/StackScreen, the Social tab Page_Widget): a stack whose items
// are pages, the Hub's own page (Page_Widget, a page model with the brushed title) first and what is
// opened from the Hub (the profile) pushed over it. Until 2026-11-04 the Hub was looked for as a page
// model, so its title and stack were never read.
constexpr Schema stack_screen{0xd71ee0a1, 80};
constexpr std::uint32_t page_title = 0x3781b603;
// The Hub tab's StackScreen, while the Hub is the pause menu's selected tab; nothing when another tab
// is, and an empty value when the Hub is but its content is not a StackScreen.
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
    if (!model.handle || !model.type || kind(model.type) != 2 || size(model.type) != stack_screen.size ||
        read<std::uint32_t>(read<Address>(model.type)) != stack_screen.hash)
        return Value{};
    return model;
}
// The Hub's own page: the first page of its stack, when that is a page model.
Value hub_page(const Context &c, Value hub) {
    const auto list = c.field(hub, items);
    unsigned count{}, stride{};
    c.array(list, 32, count, stride);
    if (!count) return {};
    const auto widget = read<Widget>(c.address(c.field(c.element(list, 0), content)));
    const Value model{widget.data.handle, widget.data.handle ? c.type_of(widget.data.handle) : 0};
    return model.handle && model.type == c.type(page) ? model : Value{};
}
// What the Hub's stack shows (the item and how many), for the log only: it does not decide whether
// the front page is drawn.
std::string hub_stack_said(const Context &c, Value hub) {
    try {
        unsigned count{}, stride{};
        c.array(c.field(hub, items), 32, count, stride);
        return std::format("item {} of {}", read<int>(c.address(c.field(hub, 0x18f8355b))), count);
    } catch (const std::exception &e) {
        return std::string("unreadable: ") + e.what();
    }
}

// No big brushed "HUB" title behind the player card: the Hub's page gets a single space for a title
// while the front page is over it, as ReSkate's own pages have (native_menu.cpp; an empty string
// once crashed the game), and the game's title back when the front page falls back or ReSkate lets go
// of the menu. Written again whenever the game rebuilds it.
struct HubTitle {
    Value page;
    Address manager{}; // the model manager the page is in
    std::string original;
} hub_title;
std::atomic<bool> hub_title_held{};
void give_back_hub_title(const Context &c) {
    const auto held = std::exchange(hub_title, {});
    hub_title_held = false;
    if (!held.page.handle || held.original.empty() || held.manager != c.manager || c.type_of(held.page.handle) != held.page.type) return;
    const auto title = c.path(held.page, {page_title, text_field});
    if (c.text(title, 64) == " ") c.text(title, held.original);
}
void blank_hub_title(const Context &c, Value page_model) {
    if (hub_title.page.handle != page_model.handle || hub_title.manager != c.manager) {
        give_back_hub_title(c);
        hub_title = {page_model, c.manager, {}};
    }
    const auto title = c.path(page_model, {page_title, text_field});
    auto text = c.text(title, 64);
    if (text == " ") return;
    logging::log(logging::Level::info, logging::Channel::ui, "Front page: the Hub's title \"{}\" is blanked.", text);
    hub_title.original = std::move(text);
    hub_title_held = true;
    c.text(title, " ");
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

// EDIT PROFILE sends what the Hub's Profile tile did, HUB_Nav_CollectionScore, which opens the
// profile over the Hub (2026-11-04 in game). The profile leaves the Hub the selected tab, and the
// Hub's StackScreen says "item -1 of 1" the whole time, so neither tells the profile is up.
//
// The menu's button bar does: it holds the buttons of the page on screen (character_room.cpp knows
// the Skaters page by its CASPresetPurchase the same way). The profile's page (PlayerCard_Page,
// titled ID_MENU_PROFILE, UI dumps of 2026-09-26) has the button PlayerProfile_EditCard, whose
// navigation is EditPlayerCardPage. The page's model alone says nothing: the pause menu keeps a model
// for each of its pages (15, the profile's among them) whether it is shown or not. No dump has the
// card editor (EditPlayerCardPage) up, so it is known by what it is not: once the profile was seen,
// the front page stays hidden until the bar is the Hub's again (as it was while the front page was
// last drawn, or with the pause menu's own GoToMyPlayerCard, which pages pushed over it replace).
// Before 2026-11-05 the first B after EDIT PROFILE was taken for leaving the profile, so B out of the
// card editor back to the profile brought the front page back over it.
//
// Where the profile's bar does not show up after EDIT PROFILE, the old rule stays as the fallback:
// hidden until the next B or Escape, and a moment for the game to take the profile away. Leaving the
// Hub (another tab, or the pause menu closed) ends it either way.
constexpr std::string_view profile_navigation = "HUB_Nav_CollectionScore";
constexpr std::uint64_t profile_give_up_ms = 2000, profile_back_ms = 300, hub_gone_ms = 200, profile_find_ms = 1500, bar_every_ms = 100;
constexpr std::uint32_t button_bar = 0xe6ab1489, button_bar_buttons = 0x5fc6738b, button_action = 0xa2b71c93;
constexpr std::uintptr_t button_model_handle = 8; // each 16-byte element of the bar holds its button model's handle here
constexpr std::string_view profile_bar_button = "EditPlayerCardPage ", pause_bar_button = "GoToMyPlayerCard ";
std::atomic<bool> profile_wanted{};
// How the front page tells the profile has gone: by the button bar, by B or Escape (the fallback), or
// not yet known (EDIT PROFILE's navigation went, and the profile's bar has not shown up yet).
enum class ProfileSignal { waiting, bar, keys };
struct ProfileAway {
    std::uint64_t asked{};      // GetTickCount64 when EDIT PROFILE was pressed, while the navigation waits; 0: not waiting
    bool away{};                // the front page is hidden for the profile
    ProfileSignal signal{};     // while away
    std::uint64_t sent_at{};    // when EDIT PROFILE's navigation went
    bool back_down{};           // B or Escape held at the last look
    std::uint64_t back_at{};    // when B or Escape was pressed on the profile; 0: not yet
    std::uint64_t gone_since{}; // when the Hub stopped being the tab shown; 0: it is
    // The button bar's navigations in one line, read at most every bar_every_ms; and the Hub's own,
    // as it was while the front page was drawn.
    std::string bar, hub_bar;
    bool bar_read{};
    std::uint64_t bar_at{};
} profile;
// The navigations of the button bar's buttons, each followed by a space; nothing when there is no bar.
std::optional<std::string> bar_navigations(const Context &c) {
    const auto roots = c.roots({button_bar});
    if (roots.empty()) return std::nullopt;
    std::string line;
    for (const auto &root : roots) {
        unsigned count{}, stride{};
        const auto bytes = c.array(c.field(root.model, button_bar_buttons), 64, count, stride);
        for (unsigned i = 0; i < count && stride >= button_model_handle + 8; ++i) {
            try {
                std::uint64_t handle{};
                std::memcpy(&handle, bytes.data() + i * stride + button_model_handle, sizeof(handle));
                if (!handle) continue;
                const Value shown_button{handle, c.type_of(handle)};
                line += c.text(c.path(shown_button, {button_action, navigation}), 64) + ' ';
            } catch (const std::exception &) {
                // (A button without an action.)
            }
        }
    }
    return line;
}
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
// B or Escape, as the overlay reads them (with the pause menu up).
bool back_down_now() {
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    const bool escape = (pad.keys[VK_ESCAPE / 64] >> (VK_ESCAPE % 64)) & 1;
    return escape || (pad.available && (pad.buttons & XINPUT_GAMEPAD_B));
}
// Whether the front page is hidden for the profile now.
bool away_for_profile(const Context &c, const std::optional<Value> &hub) {
    auto &p = profile;
    const auto now = GetTickCount64();
    if (!hub) {
        if (!p.gone_since) p.gone_since = now;
        if (p.away && now - p.gone_since >= hub_gone_ms) {
            p.away = false;
            logging::log(logging::Level::info, logging::Channel::ui, "Front page: back: the profile closed (the Hub was left).");
        }
        p.asked = 0;
        profile_wanted = false;
        return p.away;
    }
    p.gone_since = 0;
    if (!p.bar_at || now - p.bar_at >= bar_every_ms) {
        p.bar_at = now;
        try {
            auto line = bar_navigations(c);
            p.bar_read = line.has_value();
            p.bar = std::move(line).value_or(std::string());
        } catch (const std::exception &) {
            p.bar_read = false;
            p.bar.clear();
        }
    }
    const bool profile_bar = p.bar_read && p.bar.find(profile_bar_button) != std::string::npos;
    // The profile on screen, however it was opened (EDIT PROFILE, or the game's own View shortcut).
    if (profile_bar && (!p.away || p.signal != ProfileSignal::bar)) {
        p.away = true;
        p.signal = ProfileSignal::bar;
        p.asked = p.back_at = 0;
        logging::log(logging::Level::info, logging::Channel::ui, "Front page: hidden: the profile is open (the button bar has its EditPlayerCardPage).");
    }
    if (profile_wanted.exchange(false) && !p.away) p.asked = now;
    if (p.asked) {
        if (navigate(c, profile_navigation)) {
            // (What is held as it goes is not the press that leaves the profile.)
            p.asked = 0;
            p.away = true;
            p.signal = ProfileSignal::waiting;
            p.sent_at = now;
            p.back_down = true;
            p.back_at = 0;
            logging::log(logging::Level::info, logging::Channel::ui, "Front page: hidden: the profile is opening (EDIT PROFILE).");
        } else if (now - p.asked >= profile_give_up_ms) {
            p.asked = 0;
            logging::log(logging::Level::info, logging::Channel::ui, "Front page: the game's navigation stayed busy; EDIT PROFILE gave up on it.");
        }
    }
    if (!p.away) {
        // The Hub's own bar, kept up to date while the front page is over it.
        if (p.bar_read && hub_taken) p.hub_bar = p.bar;
        return false;
    }
    const bool down = back_down_now();
    if (p.signal != ProfileSignal::bar && down && !p.back_down && !p.back_at) p.back_at = now;
    p.back_down = down;
    switch (p.signal) {
    case ProfileSignal::bar:
        // The profile and anything opened from it (the card editor) keep it hidden; the bar of the Hub
        // itself brings it back.
        if (!profile_bar && p.bar_read && ((!p.hub_bar.empty() && p.bar == p.hub_bar) || p.bar.find(pause_bar_button) != std::string::npos)) {
            p.away = false;
            logging::log(logging::Level::info, logging::Channel::ui, "Front page: back: the profile closed (the button bar is the Hub's again).");
        }
        break;
    case ProfileSignal::waiting:
        if (now - p.sent_at < profile_find_ms) break;
        p.signal = ProfileSignal::keys;
        logging::log(logging::Level::info, logging::Channel::ui,
                     "Front page: the profile's button bar was not found ({}); leaving the profile is read from B or Escape instead.",
                     p.bar_read ? (p.bar.find_first_not_of(' ') == std::string::npos ? std::string("no navigations") : "it has " + p.bar)
                                : std::string("no bar"));
        [[fallthrough]];
    case ProfileSignal::keys:
        if (p.back_at && now - p.back_at >= profile_back_ms) {
            p.away = false;
            p.back_at = 0;
            logging::log(logging::Level::info, logging::Channel::ui, "Front page: back: the profile closed (B or Escape; the Hub's stack shows {}).",
                         hub->handle ? hub_stack_said(c, *hub) : std::string("nothing readable"));
        }
        break;
    }
    return p.away;
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
// Whether ReSkate's front page is drawn over the Hub now: the Hub is the selected tab, its lists were
// emptied, and the profile EDIT PROFILE opened is not up. Holds the Hub's Back for the overlay
// meanwhile and blanks the Hub page's title. Called whatever page is up (`in_menu` false when it is
// not the Hub's turn), so a held Back is always given back.
bool update_hub_front_page(const Context &c, Value menu, bool in_menu) {
    std::optional<Value> hub;
    try {
        if (in_menu) hub = selected_hub(c, menu);
    } catch (const std::exception &) {
        hub.reset();
    }
    bool away{};
    try {
        away = away_for_profile(c, hub);
    } catch (const std::exception &e) {
        profile = {};
        logging::log(logging::Level::warning, logging::Channel::ui, "Front page: EDIT PROFILE could not navigate ({}).", e.what());
    }
    // The game's side menu (pause pressed again) and what opens from it (Settings, Skatepedia, the
    // Appearance screens) go over the Hub without changing the tab: the Hub's page has no focus then
    // (its index is -1), as ReSkate's own pages check (native_menu.cpp). The front page steps aside
    // for that long; it was drawn over those screens.
    bool covered{};
    try {
        if (hub && hub->handle)
            if (const auto own_page = hub_page(c, *hub); own_page.handle)
                covered = read<int>(c.address(c.path(own_page, {0xfafda1a6, 0x20a39d92, 0x6b01fd01}))) < 0;
    } catch (const std::exception &e) {
        static bool said{};
        if (!std::exchange(said, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Front page: the Hub's focus could not be read ({}); it stays up over the game's side menu.", e.what());
    } catch (...) {}
    const bool on_hub = hub && hub_taken && !away && !covered && front_page_on.load(std::memory_order_relaxed);
    {
        // Said when it changes: whether it is drawn, and why not (the profile says so itself).
        static std::string said;
        const auto now_said = !hub ? std::string() : on_hub ? std::string("drawn") : away ? std::string("profile") : covered ? std::string("covered")
                                                                                                                         : std::string("lists");
        if (now_said != said) {
            if (on_hub) logging::log(logging::Level::info, logging::Channel::ui, "Front page: drawn over the Hub{}.", said.empty() ? "" : " again");
            else if (now_said == "lists")
                logging::log(logging::Level::info, logging::Channel::ui,
                             "Front page: not drawn: the Hub's lists were not found, so the game's Hub is left as it is.");
            said = now_said;
        }
    }
    Value page_model;
    try {
        if (hub && hub->handle) page_model = hub_page(c, *hub);
        if (!hub_taken) give_back_hub_title(c);
        else if (page_model.handle) blank_hub_title(c, page_model);
    } catch (const std::exception &e) {
        static bool said{};
        if (!std::exchange(said, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Front page: the Hub's title could not be blanked ({}).", e.what());
    }
    try {
        hold_back(c, page_model, on_hub && GetTickCount64() < back_wanted_until.load(std::memory_order_relaxed));
    } catch (const std::exception &e) {
        held_back = {};
        logging::log(logging::Level::warning, logging::Channel::ui, "Front page: the Hub's Back could not be held ({}).", e.what());
    }
    return on_hub;
}
// The game's title back on the Hub's page before ReSkate lets go of the menu (a level load, or the
// game closing).
bool hub_title_blanked() noexcept { return hub_title_held.load(); }
void restore_hub_title(const Context &c) noexcept {
    try {
        give_back_hub_title(c);
    } catch (...) {
        hub_title = {};
        hub_title_held = false;
    }
}

void open_native_profile() noexcept { profile_wanted = true; }
void set_hub_front_page(bool on) noexcept { front_page_on.store(on, std::memory_order_relaxed); }
void hold_hub_back(bool held) noexcept { back_wanted_until.store(held ? GetTickCount64() + 300 : 0, std::memory_order_relaxed); }
} // namespace dingosdk::multiplayer
