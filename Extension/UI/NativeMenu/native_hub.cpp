#include "native_menu_internal.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <optional>

// The pause menu's Hub page lists live-service tiles in two native TileLists.
// ReSkate has no store, skate.Pass, bounties, missions or crew bounties, so
// those tiles go; News and Rip Score (profile) stay. The Hub resolves a press
// by the tile's position, so a tile with a kept tile after it is hidden in
// place (an empty gap with no action) rather than removed; trailing ones are
// removed. The game rebuilds the Hub now and then; this is re-applied then.
namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace {
constexpr std::uint32_t tile_list_schema = 0x74b1ea1d, tile_items = 0x67223da7;
// TileListItem -> presenter -> {button, content widget}; a tile's data names its title.
constexpr std::uint32_t tile_presenter = 0x214d4984, tile_button = 0x0fb0d794, tile_content = 0x214d4984;
constexpr std::uint32_t tile_title = 0x77b8ed0b, navigation = 0xb1e14cd6, delegate = 0x9c76b86c;
constexpr std::uint32_t action_active = 0x4b3146e9, limited_interactivity = 0x67212c85, layer_blueprint = 0x8d1441c7;
constexpr std::uint32_t default_index = 0x1600d48e;
// Tiles navigate by name (bounties, missions, crew bounties), or open the
// Store and skate.Pass through their own actions, known by their titles.
constexpr std::array<std::string_view, 3> removed_navigation{"HUB_Nav_Events", "HUB_Nav_Quests", "HUB_Nav_CrewProgression"};
constexpr std::array<std::string_view, 2> removed_titles{"ID_MENU_STORE", "ID_MENU_SKATEPASS"};

Value interact(const Context &c, Value item) {
    return c.path(item, {tile_presenter, tile_button, 0xa704272a, 0xc52416ef, 0xa704272a});
}
std::string navigation_name(const Context &c, Value item) {
    return c.text(c.path(interact(c, item), {0xa2b71c93, navigation}), 64);
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
std::string title_key(const Context &c, Value item) {
    const auto widget = read<Widget>(c.address(c.path(item, {tile_presenter, tile_content})));
    const auto type = widget.data.handle ? c.type_of(widget.data.handle) : 0;
    if (!type) return {};
    try {
        return c.text(c.path(Value{widget.data.handle, type}, {tile_title, text_field}), 64);
    } catch (const std::exception &) {
        return {}; // a tile whose data has no title
    }
}
template <std::size_t N> bool listed(const std::array<std::string_view, N> &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
void remove_hub_tiles(const Context &c) {
    for (const auto &root : c.roots({tile_list_schema})) {
        const auto list = c.field(root.model, tile_items);
        unsigned count{}, stride{};
        c.array(list, 32, count, stride);
        std::vector<bool> unwanted(count);
        unsigned last_kept = 0; // one past the last tile that stays
        for (unsigned i = 0; i < count; ++i) {
            const auto item = c.element(list, i);
            unwanted[i] = already_hidden(c, item) || listed(removed_navigation, navigation_name(c, item)) ||
                          listed(removed_titles, title_key(c, item));
            if (!unwanted[i]) last_kept = i + 1;
        }
        // Hide the ones before a kept tile; drop the trailing ones.
        for (unsigned i = 0; i < last_kept; ++i)
            if (unwanted[i]) hide_in_place(c, c.element(list, i));
        // Focus entering the list starts on its first shown tile, not a hidden one.
        const auto first_kept = static_cast<std::int32_t>(std::find(unwanted.begin(), unwanted.end(), false) - unwanted.begin());
        const auto entry = c.field(root.model, default_index);
        const auto current_entry = read<std::int32_t>(c.address(entry));
        if (first_kept < static_cast<std::int32_t>(last_kept) && current_entry >= 0 &&
            current_entry < static_cast<std::int32_t>(count) && unwanted[static_cast<unsigned>(current_entry)])
            c.set(entry, first_kept);
        if (last_kept < count && std::any_of(unwanted.begin() + last_kept, unwanted.end(), [](bool u) { return u; })) {
            // Hiding an earlier tile clears its inline navigation CString,
            // delegate, content and style refs. The earlier row snapshot would
            // borrow released values and undo those edits when republished.
            unsigned live_count{},live_stride{};
            const auto bytes=c.array(list,32,live_count,live_stride);
            require(live_count==count && live_stride==stride,"Native Hub rows changed while hiding tiles.");
            std::vector<std::byte> kept(bytes.begin(), bytes.begin() + std::size_t{last_kept} * stride);
            c.array(list, kept, last_kept);
        }
    }
}
// With every tile of the left list gone, the Hub has nothing to focus when the
// pause menu opens: its partition (HorizontalPartition_LinearFocus_Widget)
// always starts on the left side, and an empty side isn't focusable. So the
// right list (News, Profile) moves into the left side and the right side is
// left empty.
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
void move_hub_column(const Context &c) {
    const auto list_type = c.type({tile_list_schema, 392});
    for (const auto &root : c.roots({partition_schema})) {
        const auto left = c.field(root.model, partition_left), right = c.field(root.model, partition_right);
        const auto right_widget = read<Widget>(c.address(right));
        const auto right_tiles = hub_list_tiles(c, right_widget, list_type);
        if (!right_tiles || !*right_tiles) continue;
        const auto left_tiles = hub_list_tiles(c, read<Widget>(c.address(left)), list_type);
        if (left_tiles && *left_tiles) continue; // the left list still has tiles of its own
        c.set(left, right_widget);
        c.set(right, Widget{});
    }
}
// Each Hub list is anchored toward the centre split: the right list with
// ListAnchorPoint 1, the left one with 0. The moved list takes the left one's.
constexpr std::uint32_t list_anchor_point = 0xb9eae335;
constexpr std::int32_t left_list_anchor = 0;
void align_hub_column(const Context &c) {
    const auto list_type = c.type({tile_list_schema, 392});
    for (const auto &root : c.roots({partition_schema})) {
        const auto left = read<Widget>(c.address(c.field(root.model, partition_left)));
        const auto left_tiles = hub_list_tiles(c, left, list_type);
        if (!left_tiles || !*left_tiles) continue;
        if (read<Widget>(c.address(c.field(root.model, partition_right))).blueprint) continue; // not the moved column
        const auto anchor = c.field(Value{left.data.handle, list_type}, list_anchor_point);
        if (read<std::int32_t>(c.address(anchor)) != left_list_anchor) c.set(anchor, left_list_anchor);
    }
}
} // namespace

// Runs every frame while the pause menu is open, so the removed tiles never
// get a frame on screen when the Hub is rebuilt. Never lets the Hub disturb
// ReSkate's own tabs: a failure is logged once and retried later.
void update_hub(const Context &c) {
    static std::uint64_t next_update{};
    static std::string last_error;
    const auto now = GetTickCount64();
    if (now < next_update) return;
    try {
        remove_hub_tiles(c);
        move_hub_column(c);
        align_hub_column(c);
        last_error.clear();
    } catch (const std::exception &e) {
        next_update = now + 2000;
        if (last_error != e.what()) {
            last_error = e.what();
            logging::log(logging::Level::warning, logging::Channel::ui, "Hub tiles: {}", e.what());
        }
    }
}
} // namespace dingosdk::multiplayer
