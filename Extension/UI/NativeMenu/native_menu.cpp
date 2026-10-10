#include "native_menu.h"
#include "native_menu_internal.h"
#include "native_menu_dump.h"
#include "ui_sound.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace {
using menu_view::page_count, menu_view::tools_page;
// ReSkate has no store: while its tabs are in, the native Store tab is taken out, and it is put
// back before our models are released. Native tabs are found by their label, not index.
constexpr std::string_view store_tab_label = "ID_MENU_STORE", social_tab_label = "ID_MENU_SOCIAL";
constexpr unsigned social_tab_index = 4; // Before the Store is removed.
constexpr std::array<unsigned, page_count> page_slots{0, 1};
// The Store item taken out of the pause menu, its index there, and that menu.
struct RemovedStore { std::vector<std::byte> item; unsigned index{}; Address core{}; };
RemovedStore removed_store;
// When insert() last left a copy of that Store item last in the menu (0: not there); trim_store drops it.
std::uint64_t store_last_since{};
thread_local State* current_state{};
std::uint32_t menu_key() { return our_key + state().slot * 0x100; }
std::mutex callbacks_mutex;
overlay::CallbacksV3 tools_callbacks{};
MenuLifetime lifetime;
void status(std::string message) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.status == message) return;
    s.status = std::move(message);
    logging::write(logging::Level::info, logging::Channel::ui, s.status);
}
template<unsigned Slot, std::size_t Index> void activate() noexcept {
    if (lifetime.blocked()) return;
    auto& s = page_state(Slot);
    if (Index >= s.action_count.load(std::memory_order_acquire)) return;
    try {
        // Under the lock: a render pass can hand this slot to another command.
        std::lock_guard lock(s.mutex);
        const auto& action = s.actions[Index].action;
        if (action.generation != s.generation.load(std::memory_order_acquire)) return;
        if (s.pending.size() < 8) s.pending.push_back(action);
    } catch (...) { }
}
template<unsigned Slot, std::size_t... I> auto callbacks(std::index_sequence<I...>) {
    return std::array{&activate<Slot, I>...};
}
} // namespace
namespace native_menu_detail {
State& page_state(unsigned slot) {
    static auto* values = new std::array<State, page_count>;
    return (*values)[slot];
}
State& state() { return current_state ? *current_state : page_state(0); }
Address action(const Context& context, std::string command, std::string argument) {
    auto& s = state();
    const auto generation = s.generation.load();
    const auto count = s.action_count.load();
    const auto slot = action_slot(count, max_actions, [&](std::size_t i) -> const Action& { return s.actions[i].action; },
        command, argument, s.pass);
    require(slot.has_value(), "Native menu action limit reached; restart to refresh the menu.");
    auto& result = s.actions[*slot];
    const bool unused = *slot == count;
    if (unused) {
        static const std::array functions{callbacks<0>(std::make_index_sequence<max_actions>{}),
            callbacks<1>(std::make_index_sequence<max_actions>{})};
        // Native void-action descriptor used by this build. Only the invokers are
        // replaced; it is never added to the game's global function registry.
        const auto prototype = context.base + addr::native_menu::action_prototype;
        require(read<Address>(prototype + 0x40) == context.base + addr::native_menu::action_invoker &&
                read<std::uint8_t>(prototype + 0x68) == 0,
                "Native menu action ABI differs.");
        require(memory::read_bytes(prototype, result.descriptor.data(), result.descriptor.size()), "Native menu action template is unavailable.");
        const auto function = reinterpret_cast<Address>(functions[s.slot][count]);
        for (const auto offset : {0x30, 0x40, 0x48}) std::memcpy(result.descriptor.data() + offset, &function, sizeof(function));
        result.info = reinterpret_cast<Address>(result.descriptor.data());
    }
    {
        // activate() reads the slot on the game's own thread.
        std::lock_guard lock(s.mutex);
        if (unused || result.action.command != command || result.action.argument != argument) {
            result.action.command = std::move(command);
            result.action.argument = std::move(argument);
        }
        result.action.generation = generation;
        result.action.pass = s.pass;
    }
    if (unused) s.action_count.store(count + 1, std::memory_order_release);
    return reinterpret_cast<Address>(&result.info);
}
Address blueprint(std::string_view name) {
    const auto& assets = state().assets;
    const auto found = assets.find(name);
    if (found == assets.end()) throw std::runtime_error("Widget not loaded: " + std::string(name));
    require(read<Address>(found->second + 8) == state().base + addr::native_menu::widget_blueprint_type,
            "Native menu widget asset has expired.");
    return found->second;
}
Value make(const Context& context, Schema schema) {
    auto& s = state();
    // The menu root's generation is part of our identity. A later pause-menu
    // instance cannot accidentally acquire another instance's page or inputs.
    const auto value = context.create(schema, (s.owner ^ 0x52534d5000000000ULL) + s.slot * 0x100000 + s.next_id++);
    s.owned_models.track(value);
    return value;
}
} // namespace native_menu_detail
namespace {
void render(const Context& context, const MultiplayerModel& model) {
    auto& s = state();
    ++s.pass;
    // Both pages are drawn by the overlay (Extension/UI/Overlay/hub_page.cpp), their tabs too:
    // here each is one body with nothing in it.
    s.section = Section::browser;
    render_section(context, model, Section::browser);
}
void ensure_body(const Context& context) {
    auto& s = state();
    const auto list = context.path(s.page, {content, items});
    unsigned count{}, stride{};
    const auto current = context.array(list, section_count, count, stride);
    require(stride == stack_item.size, "Native page body schema differs.");
    if (count == s.section_total) return;
    std::vector<std::byte> bytes(stack_item.size * s.section_total);
    for (unsigned i = 0; i < s.section_total; ++i)
        require(memory::read_bytes(context.address(s.bodies[i]), bytes.data() + i * stack_item.size, stack_item.size), "Native menu body is unavailable.");
    context.array(list, bytes, s.section_total);
    context.set(context.path(s.page, {content, 0x18f8355b}), static_cast<int>(s.section));
    context.set(context.path(s.page, {content, 0x55511280}), static_cast<int>(s.section));
}
void tab_label(const Context& context, Value item, const std::string& name, std::uint32_t key) {
    const auto tab = context.field(item, tab_data);
    context.text(context.path(tab, {label, text_field}), name);
    const auto info = make(context, {0xb50f9e3a, 48});
    context.set(context.field(tab, 0x87a8a504), Ref{0, info.handle});
    context.text(context.field(info, 0x1c1d36ea), name);
    context.text(context.field(info, 0xe538148f), "ReSkate.Multiplayer." + name);
    context.set(context.field(info, 0xd0f2dee5), key);
}
std::optional<unsigned> find_tab(const Context& context, Value list, unsigned count, std::string_view name) {
    for (unsigned i = 0; i < count; ++i)
        if (context.text(context.path(context.element(list, i), {tab_data, label, text_field}), 64) == name) return i;
    return {};
}
unsigned social_index(const Context& context, Value menu) {
    const auto list = context.path(menu, {content, items});
    unsigned count{}, stride{};
    context.array(list, 16, count, stride);
    return find_tab(context, list, count, social_tab_label).value_or(social_tab_index);
}
// Each time the pause menu opens the game rebuilds its native tabs (new keys,
// the Store back in place) without ours and selects its default slot. Remember
// the last ReSkate tab the player had open; when our tabs have gone missing,
// keep that memory and select it again once they are re-inserted. Only one
// page slot runs this; the tab stack is shared.
// The pause menu's top bar shows the player's wallets (San Van Bucks,
// Influence) through one presenter on the menu core. ReSkate has no store and
// the wallets' icons live on the retired CDN, so they draw as empty boxes:
// publish an empty presenter instead. Checked each tick; cheap when already empty.
constexpr std::uint32_t top_bar_currency = 0x75320e5f;
void hide_top_bar_currency(const Context& context, Value menu) {
    const auto field = context.field(menu, top_bar_currency);
    const auto current = read<Widget>(context.address(field));
    if (current.blueprint || current.data.record || current.data.handle) context.set(field, Widget{});
}
} // namespace
void update_hub(const Context& context);
namespace {
// Which of ReSkate's pages is on screen, for the overlay (native_menu_page): when one was last
// seen selected with the pause menu up (GetTickCount64; 0: not now), and which.
std::atomic<std::uint64_t> page_seen{};
std::atomic<int> page_shown{}; // 0: Multiplayer, 1: Mod Options
std::atomic<bool> game_in_menu{};
// Returns the key of the tab the pause menu has selected (0: none of ours can be told).
std::uint32_t restore_last_tab(const Context& context, Value menu) {
    static std::uint32_t remembered{};
    static bool reopening{};
    static int previous = -2;
    const auto list = context.path(menu, {content, items});
    const auto target = context.path(menu, {content, 0x18f8355b});
    unsigned count{}, stride{};
    const auto values = context.array(list, 16, count, stride);
    if (stride != stack_item.size) return 0;
    const auto key_at = [&](int index) {
        std::uint32_t key{};
        if (index >= 0 && static_cast<unsigned>(index) < count) std::memcpy(&key, values.data() + index * stride + 0x568, 4);
        return key;
    };
    const auto owned = [](std::uint32_t key) {
        return key >= our_key && key < our_key + page_count * 0x100 && (key - our_key) % 0x100 == 0;
    };
    bool present{};
    for (unsigned i = 0; i < count; ++i) present |= owned(key_at(static_cast<int>(i)));
    if (!present) { reopening = true; previous = -2; return 0; }
    const auto selected = read<int>(context.address(target));
    if (reopening) {
        reopening = false;
        for (unsigned i = 0; remembered && i < count; ++i)
            if (key_at(static_cast<int>(i)) == remembered) {
                if (static_cast<int>(i) != selected) {
                    context.set(target, static_cast<int>(i));
                    context.set(context.path(menu, {content, 0x55511280}), static_cast<int>(i));
                    logging::printf(logging::Level::info, logging::Channel::ui, "Pause menu reopened on ReSkate tab %u.", i);
                }
                previous = static_cast<int>(i);
                return key_at(static_cast<int>(i));
            }
    }
    if (selected != previous) {
        previous = selected;
        if (selected >= 0) remembered = owned(key_at(selected)) ? key_at(selected) : 0;
    }
    return key_at(selected);
}
// Page Back is a button action: input UI_Back, navigation "ToggleMenu", no
// delegate, handled by the shared navigation handler. An active action with
// neither swallows Escape/controller B. Build it directly rather than copying
// Map's: the game rebuilds Map's page on every open, and a copy taken before
// that page is filled in leaves Back inert. Rebuilt whenever our tabs return.
void build_back_action(const Context& context, State& s) {
    const auto base = context.path(s.page, {0x10810f0b, button_field, 0xa704272a, 0xc52416ef, 0xa704272a});
    const auto back = context.field(base, 0xa2b71c93);
    const Address back_input = named_asset(context, blueprint("UI/Foundations/Templates/Page/Page_Widget"),
                                           "Configurations/Input/UI/UI_Back");
    if (!back_input) { status("Native menu: Back input unavailable; waiting for Map's page."); return; }
    context.set(context.field(back, 0x3d00de27), back_input);
    context.text(context.field(back, 0xb1e14cd6), "ToggleMenu");
    context.set(context.field(base, 0x4b3146e9), true);
}
void insert(const Context& context) {
    auto& s = state();
    const auto list = context.path(s.core, {content, items});
    unsigned count{}, stride{};
    auto values = context.array(list, 16, count, stride);
    require(stride == stack_item.size && count >= 5 && count < 16, "Native pause menu stack differs.");
    std::vector<std::uint32_t> keys(count);
    for (unsigned i = 0; i < count; ++i) std::memcpy(&keys[i], values.data() + i * stride + 0x568, 4);
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t key{}; std::memcpy(&key, values.data() + i * stride + 0x568, 4);
        if (key == menu_key()) {
            const auto existing = context.element(list, i);
            const auto widget = read<Widget>(context.address(context.field(existing, content)));
            if (widget.data.handle != s.page.handle) context.copy(existing, context.address(s.menu_item));
            else for (const auto hash : {0xd4f0f044U, 0xbc92d1faU, 0xa3c18619U})
                context.copy(context.path(existing, {tab_data, hash}), context.address(context.path(s.menu_item, {tab_data, hash})));
            return;
        }
    }
    bool has_tools{};
    { std::lock_guard callbacks_lock(callbacks_mutex); has_tools = tools_callbacks.read_model != nullptr; }
    const bool offline = launcher::offline_mode();
    const auto missing = menu_view::missing_tabs(keys, our_key, has_tools, !offline);
    // Offline, the Multiplayer tab whose key ends the scan above never exists.
    if (offline && missing.empty()) return;
    // Prepare both pages before exposing either. Their initial insertion is
    // atomic and ordered; never reorder live stack items or their focus bindings.
    for (const auto slot : missing) {
        const auto& candidate = page_state(slot);
        if (candidate.core.handle != s.core.handle || !candidate.page.handle || !candidate.menu_item.handle ||
            context.type_of(candidate.page.handle) != candidate.page.type) return;
    }
    require(count + missing.size() <= 16, "Native pause menu has no room for ReSkate tabs.");
    // Take the Store item out. Pause-menu detection needs at least five items in the stack, so
    // only together with ours.
    bool store_last{};
    if (const auto store = find_tab(context, list, count, store_tab_label);
        store && !missing.empty() && count - 1 + missing.size() >= 5) {
        const auto at = values.begin() + static_cast<std::ptrdiff_t>(*store) * stride;
        removed_store = {std::vector<std::byte>(at, at + stride), *store, s.core.handle};
        values.erase(at, at + stride);
        --count;
        // The game has already opened the page at its selected index, which it keeps
        // from the player's last visit, before ours were back. Taking the Store out
        // changes the tab at that index if it is the Store's or later, and the stack
        // then shows the new tab's page only if its length changes too: online the
        // Multiplayer tab makes it longer, but offline Mod Options just takes the
        // Store's place, and the old page stays up, blank, with Back dead. Then keep
        // a copy of the Store item last for now, so the length changes, and drop it
        // once the stack has seen that (trim_store).
        const auto selected = read<int>(context.address(context.path(s.core, {content, 0x18f8355b})));
        store_last = missing.size() == 1 && selected >= static_cast<int>(*store);
    }
    for (const auto slot : missing) {
        auto& owned = page_state(slot);
        const auto old_size = values.size(); values.resize(old_size + stride);
        require(memory::read_bytes(context.address(owned.menu_item), values.data() + old_size, stride),
                "Native menu item is unavailable.");
        ++count;
    }
    if (store_last) {
        values.insert(values.end(), removed_store.item.begin(), removed_store.item.end());
        ++count;
        store_last_since = GetTickCount64();
    }
    context.array(list, values, count);
    // The menu was reopened (our tabs had gone): refresh every page's Back.
    for (unsigned slot = 0; slot < page_count; ++slot) {
        auto& page_owner = page_state(slot);
        if (page_owner.core.handle == s.core.handle && page_owner.page.handle &&
            context.type_of(page_owner.page.handle) == page_owner.page.type) {
            const auto* previous = current_state;
            current_state = &page_owner;
            try { build_back_action(context, page_owner); } catch (...) {}
            current_state = const_cast<State*>(previous);
        }
    }
    // StackTabs observes Content.Items and creates its focus bindings. Existing
    // tab models, identities and callbacks remain in their original order.
}
// The copy is the item last in the stack with the removed Store's key.
bool is_store_copy(std::span<const std::byte> item) {
    return removed_store.item.size() == item.size() &&
        std::equal(item.begin() + 0x568, item.begin() + 0x56c, removed_store.item.begin() + 0x568);
}
// Drops the Store copy insert() left last, a few frames after the stack saw it there.
void trim_store(const Context& context, Value menu) {
    if (!store_last_since || GetTickCount64() < store_last_since + 50) return;
    const auto list = context.path(menu, {content, items});
    unsigned count{}, stride{};
    auto values = context.array(list, 16, count, stride);
    store_last_since = 0;
    // Not there: the game rebuilt its tabs, the Store back in place, as the menu reopened.
    if (stride != stack_item.size || count < 6 || !is_store_copy(std::span(values).last(stride))) return;
    const auto target = context.path(menu, {content, 0x18f8355b});
    const auto selected = read<int>(context.address(target));
    if (selected == static_cast<int>(count - 1)) {
        context.set(target, static_cast<int>(count - 2));
        context.set(context.path(menu, {content, 0x55511280}), static_cast<int>(count - 2));
    }
    values.resize(values.size() - stride);
    context.array(list, values, count - 1);
    logging::printf(logging::Level::info, logging::Channel::ui,
        "Pause menu reopened on tab %d with a Store copy last for a moment.", selected);
}
Value owned_style(const Context& context, const char* name, Schema schema) {
    const auto found = state().assets.find(name);
    if (found == state().assets.end()) throw std::runtime_error(std::string("Native menu style has not loaded yet: ") + name);
    const auto record = found->second;
    require(read<Address>(record + 8) == context.base + addr::native_menu::asset_record_type &&
        read<Address>(record + 0x18) == context.type(schema), "Native menu style schema differs.");
    const auto value = make(context, schema);
    context.copy(value, read<Address>(record + 0x20));
    return value;
}
Value make_list(const Context& context, float item_height = 192.f) {
    const auto list = make(context, linear_list);
    context.array(context.field(list, 0x67223da7), std::vector<Ref>{});
    context.set(context.field(list, 0x55ca89f4), blueprint("UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
    context.set(context.field(list, 0x19ff199a), item_height);
    context.set(context.field(list, 0xebca7354), 32.f);
    context.set(context.field(list, 0x4cb61cac), 1);
    context.set(context.field(list, 0x4554761c), true);
    context.set(context.field(list, 0x8bbf7d67), true); // Scroll with wheel/focus; avoid permanent rails.
    context.set(context.field(list, 0xafe8434b), true); // Scroll bar (believed; set on few native lists).
    context.set(context.field(list, 0xc33d3081), true); // RememberFocusedIndex.
    return list;
}
void initialize(const Context& context, Root root, const MultiplayerModel& model) {
    auto& s = state();
    s.generation.fetch_add(1, std::memory_order_acq_rel);
    s.core = root.model; s.owner = root.model.handle; s.next_id = 1;
    s.rows.clear(); for (auto& displayed : s.displayed) displayed.clear(); s.feedback.clear();
    for (auto& nodes : s.fixed_rows) nodes.clear(); s.selected_player.clear();
    s.copy_code_requested = false;
    s.protected_lobby.clear(); s.feedback_until = 0; s.was_active = false; s.browsing = false;
    s.browser = {}; s.section = Section::browser;
    s.assets.clear();
    for (unsigned slot = 0; slot < page_count && s.assets.empty(); ++slot) {
        const auto& other = page_state(slot);
        if (slot != s.slot && other.manager == context.manager && other.core.handle == root.model.handle &&
            other.page.handle && context.type_of(other.page.handle) == other.page.type) s.assets = other.assets;
    }
    if (s.assets.empty()) s.assets = blueprints(context, root.model);
    // Validate every required widget before exposing the new tab.
    for (const auto* name : {"UI/Foundations/Templates/Page/Page_Widget",
        "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget",
        "UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget",
        "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget",
        "UI/Foundations/Components/Text/InputField/InputField_Widget",
        "UI/Foundations/Components/Text/Label/Widget/Label_Widget", anchored_widget, horizontal_widget, vertical_widget}) blueprint(name);
    s.menu_style = owned_style(context, "ButtonStyleList/TileButton.RoughPartial.Default", {0x3d13ff54, 120});
    s.primary_style = owned_style(context, "ButtonStyleList/ButtonStyle.CTA.Hero", {0x3d13ff54, 120});
    // Hero's authored focus fill is empty. Give our copy the native blue focus
    // treatment while retaining its yellow idle fill; shared styles stay intact.
    const auto focus_field = context.member(s.menu_style.type, 0x343abac3);
    const auto focused_field = context.member(focus_field.type, 0xf622ea66);
    const auto native_row_style = read<Address>(s.assets.at("ButtonStyleList/TileButton.RoughPartial.Default") + 0x20);
    context.copy(context.path(s.primary_style, {0x343abac3, 0xf622ea66}),
                 native_row_style + focus_field.offset + focused_field.offset);
    s.page = make(context, page); s.menu_item = make(context, stack_item);
    // Social's authored page layout is already resident. Borrow it through a
    // typed reference rather than waiting for an unrelated page style to load.
    const auto social_widget = read<Widget>(context.address(context.field(
        context.element(context.path(s.core, {content, items}), social_index(context, s.core)), content)));
    const Value social{social_widget.data.handle, context.type_of(social_widget.data.handle)};
    require(social.type == s.page.type, "Native Social page is unavailable.");
    context.copy(context.field(s.page, 0x22705a14), context.address(context.field(social, 0x22705a14)));
    // A translucent black backdrop over the paused game. The style is our own
    // copy (released with the page), so the shared native style is untouched.
    constexpr Schema background_style{0x9f5f4db5, 64};
    Value backdrop_style;
    try { backdrop_style = owned_style(context, "BackgroundStylesList/BackgroundStyle.Black.Opaque", background_style); }
    catch (const std::exception&) { backdrop_style = owned_style(context, "BackgroundStylesList/BackgroundStyle.Dark", background_style); }
    context.set(context.field(backdrop_style, 0x87638719), false); // Opaque: off, so opacity applies.
    context.set(context.field(backdrop_style, 0x22092eae), 0.75f); // Opacity.
    const auto backdrop = make(context, {0x4d7e89f7, 32});
    context.set(context.field(backdrop, 0xff242844), 11);
    context.set(context.field(backdrop, 0xccf9c3c6), Ref{0, backdrop_style.handle});
    const auto backdrop_item = make(context, stack_item);
    context.set(context.field(backdrop_item, content), Widget{
        blueprint("UI/Foundations/Layers/Backgrounds/Background_Widget"), {0, backdrop.handle}});
    context.set(context.field(backdrop_item, key_field), menu_key() + 16);
    const auto backdrop_stack = context.field(s.page, 0x2d393a3a);
    context.copy(context.field(backdrop_item, 0xd78d240b),
                 context.address(context.field(backdrop_stack, 0x54c9abee)));
    std::vector<std::byte> backdrop_bytes(stack_item.size);
    require(memory::read_bytes(context.address(backdrop_item), backdrop_bytes.data(), backdrop_bytes.size()),
            "Native backdrop is unavailable.");
    context.array(context.field(backdrop_stack, items), backdrop_bytes, 1);
    context.set(context.field(backdrop_stack, 0x18f8355b), 0);
    context.set(context.field(backdrop_stack, 0x369babfe), true);
    context.set(context.field(backdrop_stack, 0x3b14d43c), false);
    context.set(context.field(backdrop_stack, 0x2a98baa3), false);
    build_back_action(context, s);
    static constexpr std::array<const char*, page_count> titles{"MULTIPLAYER", "MOD OPTIONS"},
        tab_names{"Multiplayer", "Mod Options"};
    context.text(context.path(s.page, {0x3781b603, text_field}), titles[s.slot]);
    context.set(context.field(s.page, 0xe1d821a3), true);
    context.set(context.field(s.page, 0x616cb924), false);
    // One body for either page: the overlay draws the page and its tabs.
    const std::vector<const char*> sections{" "};
    s.section_total = static_cast<unsigned>(sections.size());
    std::vector<std::byte> body_bytes(stack_item.size * sections.size());
    for (unsigned i = 0; i < sections.size(); ++i) {
        const bool scrolling = scrolling_list(s.slot, i * 2), side_scrolling = scrolling_list(s.slot, i * 2 + 1);
        // Level lists (World, Custom Maps) use the taller rows of a first column.
        const bool level_list = i == 0 || (s.slot == tools_page && i == native_tools::custom_section);
        s.lists[i] = scrolling ? make_list(context, level_list ? 192.f : 112.f) : make(context, vertical_panel);
        s.side_lists[i] = side_scrolling ? make_list(context, 112.f) : make(context, vertical_panel);
        const auto columns = make(context, horizontal_panel);
        const auto partition = context.field(columns, 0x2f1e0746);
        context.set(context.field(partition, 0x51fa3f1c), 0.f);
        context.set(context.field(partition, 0x307f2934), 96.f);
        context.set(context.field(partition, 0x613f2c2), main_width + 48.f);
        context.set(context.field(partition, 0x4fdf869b), 0);
        context.set(context.field(columns, 0xf750f0a2), Widget{blueprint(scrolling ?
            "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget" : vertical_widget), {0, s.lists[i].handle}});
        context.set(context.field(columns, 0x29023866), Widget{blueprint(side_scrolling ?
            "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget" : vertical_widget), {0, s.side_lists[i].handle}});
        const auto body = s.bodies[i] = make(context, stack_item);
        context.set(context.field(body, content), Widget{blueprint(horizontal_widget), {0, columns.handle}});
        context.set(context.field(body, key_field), menu_key() + i + 1);
        // The native push operation supplies Transition. Direct publication
        // must initialize it too, or an active item still renders blank.
        context.copy(context.field(body, 0xd78d240b),
                     context.address(context.path(s.page, {content, 0x54c9abee})));
        tab_label(context, body, sections[i], menu_key() + i + 1);
        require(memory::read_bytes(context.address(body), body_bytes.data() + i * stack_item.size, stack_item.size), "Native menu body is unavailable.");
    }
    context.array(context.path(s.page, {content, items}), body_bytes, static_cast<unsigned>(sections.size()));
    context.set(context.path(s.page, {content, 0x18f8355b}), 0); // TargetIndex.
    context.set(context.path(s.page, {content, 0x55511280}), 0);
    context.set(context.path(s.page, {content, 0x369babfe}), true);
    context.set(context.path(s.page, {content, 0x3b14d43c}), true); // IsInteractable.
    context.set(context.path(s.page, {content, 0xc912aa1d}), false); // Prebuild all four sections.
    context.set(context.path(s.page, {content, 0x10db7267}), false); // Keep native widgets between tab switches.
    context.set(context.path(s.page, {content, 0x2a98baa3}), false); // Our page owns its content across widget teardown.
    context.set(context.field(s.menu_item, content), Widget{blueprint("UI/Foundations/Templates/Page/Page_Widget"), {0, s.page.handle}});
    context.set(context.field(s.menu_item, key_field), menu_key());
    context.copy(context.field(s.menu_item, 0xd78d240b),
                 context.address(context.path(s.core, {content, 0x54c9abee})));
    tab_label(context, s.menu_item, tab_names[s.slot], menu_key());
    // Seed both states with an already-live native fallback: Social for
    // Multiplayer, Hub for ReSkate. Optional art must never gate either page.
    // Copy only visual fields; actions remain attached to our own models.
    const auto core_items = context.path(s.core, {content, items});
    const auto source_tab = context.field(context.element(core_items, s.slot ? 1 : social_index(context, s.core)), tab_data);
    const auto target_tab = context.field(s.menu_item, tab_data);
    for (const auto hash : {0xd4f0f044U, 0xbc92d1faU, 0xa3c18619U})
        context.copy(context.field(target_tab, hash), context.address(context.field(source_tab, hash)));
    // Prefer the exact requested glyph, resolving it from the locked native
    // image registry rather than searching neighboring widget allocations.
    static constexpr std::array<const char*, page_count> idle_icons{"UI/Textures/Icons/HUB/img_Icon_SocialDefault_512",
        "UI/Textures/Skatepedia/img_Skatepedia_SkateTricks_Idle_1024"};
    static constexpr std::array<const char*, page_count> focused_icons{"UI/Textures/Icons/HUB/img_Icon_SocialFocused_512",
        "UI/Textures/Skatepedia/img_Skatepedia_SkateTricks_Focused_1024"};
    const auto* idle = idle_icons[s.slot];
    const auto* focused = focused_icons[s.slot];
    bool requested_icon{};
    try { requested_icon = publish_tab_icons(context, target_tab, idle, focused); }
    catch (const std::exception&) { /* The authored fallback is already retained. */ }
    if (!requested_icon) logging::printf(logging::Level::info, logging::Channel::ui,
        "Native %s menu: using native fallback icons; optional pair unavailable: %s / %s",
        tab_names[s.slot], idle, focused);
    render(context, model);
    insert(context);
    status(std::string("Native ") + tab_names[s.slot] + " menu: tab registered.");
}
} // namespace

void tick_page(std::uintptr_t base, bool loading) noexcept {
    auto& s = state();
    if (loading || !base || !game::native_data().models.create) {
        if (s.core.handle) { s.generation.fetch_add(1); s.core = {}; s.page = {}; s.rows.clear(); s.assets.clear(); s.next_scan = 0; s.next_retry = 0; }
        return;
    }
    const auto now = GetTickCount64();
    if (now < s.next_update || now < s.next_retry) return;
    s.next_update = now + 50;
    std::string card_request;
    std::optional<std::string> card_name;
    try {
        const auto ui = read<Address>(base + addr::engine::ui_manager);
        if (!ui) return;
        const auto manager = read<Address>(ui + 0x140);
        if (!manager) return;
        if (s.manager && s.manager != manager) {
            s.owned_models.manager_replaced();
            s.generation.fetch_add(1); s.core = {}; s.page = {}; s.next_scan = 0; s.next_retry = 0;
        }
        s.base = base; s.manager = manager;
        const Context context(base, manager);
        game::ModelWriteLock lock(manager);
        Root active;
        const auto populated = [&](Value candidate) {
            if (!candidate.handle || context.type_of(candidate.handle) != candidate.type) return false;
            const auto list = context.path(candidate, {content, items});
            const auto data = read<Address>(context.address(list));
            const auto count = data ? read<std::uint32_t>(data - 4) & 0x7fffffff : 0;
            return count >= 5 && count <= 16;
        };
        if (populated(s.core)) active = {s.core, context.address(s.core)};
        else if (now >= s.next_scan) {
            // All three page slots look for the same pause-menu core, and finding it copies the
            // whole model registry: one scan serves every slot for its 250 ms. A cached handle is
            // checked against its type again (populated) and its storage looked up afresh.
            static struct { Address manager{}; ULONGLONG at{}; std::vector<Root> roots; } shared;
            if (shared.manager != manager || now - shared.at >= 250) {
                shared.roots = context.roots({core.hash});
                shared.manager = manager;
                shared.at = now;
            }
            for (const auto& candidate : shared.roots) {
                if (populated(candidate.model)) { active = {candidate.model, context.address(candidate.model)}; break; }
            }
            if (!active.model.handle) s.next_scan = now + 250;
        }
        if (!active.model.handle) return;
        trim_store(context, active.model);
        if (!s.slot) {
            const auto shown_tab = restore_last_tab(context, active.model);
            hide_top_bar_currency(context, active.model);
            update_hub(context);
            // Tell the overlay while this page is the one on screen: it draws the body.
            const bool ours = shown_tab == our_key || shown_tab == our_key + tools_page * 0x100;
            const auto& shown_page = page_state(shown_tab == our_key ? 0 : tools_page);
            const bool on_screen = ours && game_in_menu.load(std::memory_order_relaxed) && shown_page.page.handle &&
                                   context.type_of(shown_page.page.handle) == shown_page.page.type;
            // (Said at once when another page takes its place, not left to go stale.)
            page_shown.store(shown_tab == our_key ? 0 : 1, std::memory_order_relaxed);
            page_seen.store(on_screen ? now : 0, std::memory_order_relaxed);
        }
        bool pending{};
        { std::lock_guard pending_lock(s.mutex); pending = !s.pending.empty(); }
        if (active.model.handle == s.core.handle && s.page.handle && now < s.next_render && !pending &&
            context.type_of(s.page.handle) == s.page.type &&
            read<int>(context.address(context.path(s.page, {content, 0x18f8355b}))) == static_cast<int>(s.section)) return;
        const auto model = multiplayer::model();
        s.next_render = now + 200;
        if (active.model.handle != s.core.handle || !s.page.handle || context.type_of(s.page.handle) != s.page.type) {
            if (now < s.next_retry) return;
            s.next_retry = now + 500;
            initialize(context, active, model);
            s.next_retry = 0; s.failures = 0;
        } else {
            if (!s.slot) process_actions(context, model);
            else if (card_request == "card-name") card_name = input_text(context, "card-name");
            else if (card_request == "card-name-reset") { clear_input(context, "card-name"); card_name.emplace(); }
            ensure_body(context);
            render(context, model);
            insert(context);
        }
    } catch (const std::exception& e) {
        s.generation.fetch_add(1);
        s.core = {}; s.page = {};
        s.failures = std::min(s.failures + 1, 5U);
        s.next_retry = now + std::min<std::uint64_t>(30000, 1000ULL << s.failures);
        status(std::string("Native multiplayer menu: ") + e.what());
    } catch (...) {
        s.generation.fetch_add(1); s.core = {}; s.page = {};
        s.failures = std::min(s.failures + 1, 5U);
        s.next_retry = now + std::min<std::uint64_t>(30000, 1000ULL << s.failures);
        status("Native multiplayer menu: unavailable.");
    }
    if (!card_name) return;
    // Persist outside the native model lock. Only the local card changes.
    try {
        set_local_player_card_name(*card_name);
        s.tools.feedback = local_profile_player_card().feedback;
    } catch (...) { s.tools.feedback = "Couldn't save the card name. Try again."; }
    s.next_render = 0;
}
bool prepare_native_menu_level_load(std::uintptr_t base) noexcept {
    if (std::all_of(page_slots.begin(), page_slots.end(), [](unsigned slot) { return page_state(slot).owned_models.empty(); }))
        return true;
    try {
        const auto ui = read<Address>(base + addr::engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        require(manager != 0, "Native menu manager is unavailable during level cleanup.");
        const Context context(base, manager);
        game::ModelWriteLock lock(manager);
        for (unsigned slot = 0; slot < page_count; ++slot) {
            auto& s = page_state(slot);
            ++s.generation;
            { std::lock_guard pending_lock(s.mutex); s.pending.clear(); }
            if (s.manager != manager) {
                s.owned_models.manager_replaced();
                continue;
            }
            s.owned_models.release([&] {
                if (!s.core.handle || context.type_of(s.core.handle) != s.core.type) return;
                const auto stack = context.field(s.core, content);
                const auto list = context.field(stack, items);
                unsigned count{}, stride{};
                const auto values = context.array(list, 16, count, stride);
                require(stride == stack_item.size, "Native menu cleanup stack differs.");
                const auto key_offset = context.member(context.type(stack_item), key_field).offset;
                std::vector<std::byte> retained;
                bool ours{};
                for (unsigned i = 0; i < count; ++i) {
                    const auto first = values.data() + i * stride;
                    // A Store copy insert() left last: the Store goes back to its own index below.
                    if (removed_store.core == s.core.handle && i + 1 == count &&
                        is_store_copy(std::span(first, stride))) continue;
                    std::uint32_t key{}; std::memcpy(&key, first + key_offset, sizeof(key));
                    if (key >= our_key && key < our_key + page_count * 0x100 && (key - our_key) % 0x100 == 0) {
                        ours = true;
                        continue;
                    }
                    retained.insert(retained.end(), first, first + stride);
                }
                // Put the native Store item back at its original index: it is out while ours are in.
                if (ours && removed_store.core == s.core.handle && removed_store.item.size() == stride &&
                    removed_store.index * stride <= retained.size()) {
                    retained.insert(retained.begin() + static_cast<std::ptrdiff_t>(removed_store.index) * stride,
                                    removed_store.item.begin(), removed_store.item.end());
                    removed_store = {};
                }
                if (retained.size() == values.size() && std::equal(retained.begin(), retained.end(), values.begin())) return;
                // Return focus to the first native tab before removing our
                // appended items. The remaining native tabs keep their order.
                context.set(context.field(stack, 0x18f8355b), 0);
                context.set(context.field(stack, 0x55511280), 0);
                context.array(list, retained, static_cast<unsigned>(retained.size() / stride));
            }, [&](Value value) { context.destroy(value); });
            s.core = {}; s.page = {}; s.menu_item = {};
            s.rows.clear(); s.assets.clear();
            s.next_scan = s.next_retry = s.next_update = s.next_render = 0;
            s.copy_code_requested = false;
        }
        return true;
    } catch (const std::exception& e) {
        logging::write(logging::Level::warning, logging::Channel::ui,
            std::string("Native menu cleanup failed: ") + e.what());
        return false;
    } catch (...) {
        return false;
    }
}
void native_menu_before_level_transition(std::uintptr_t base, unsigned next) noexcept {
    lifetime.before_transition(next, [&] {
        // Both shutdown and multiplayer pause dumps fault in the engine
        // while destroying a 0x590-byte menu item with an unloaded widget asset.
        // Native level/sublevel exits bypass ReSkate's explicit load scheduler.
        // Release roots before those transitions and block ticks/actions until
        // state 13/21 announces the next active world.
        const bool owned = std::any_of(page_slots.begin(), page_slots.end(),
            [](unsigned slot) { return !page_state(slot).owned_models.empty(); });
        const bool cleaned = prepare_native_menu_level_load(base);
        if (owned || !cleaned) logging::printf(cleaned ? logging::Level::info : logging::Level::warning,
            logging::Channel::ui, cleaned ? "Native ReSkate menu models released before state %u." :
                "Native ReSkate menu cleanup could not finish before state %u.", next);
    });
}
overlay::HubPage native_menu_page() noexcept {
    // Seen every 50 ms while it is up; a little longer than that and it has gone.
    const auto seen = page_seen.load(std::memory_order_relaxed);
    if (!seen || GetTickCount64() - seen > 200) return {};
    return {true, page_shown.load(std::memory_order_relaxed)};
}
void tick_native_menu(std::uintptr_t base, bool loading) noexcept {
    run_pending_ui_dump(base);
    // The menu sounds the overlay's pages asked for (ui_sound.h).
    tick_ui_sounds(base, loading);
    // (Read here: it takes the UI model's lock, which a page's tick then holds.)
    game_in_menu.store(!loading && sample_game_ui_state(base).in_menu, std::memory_order_relaxed);
    if (lifetime.blocked()) return;
    for (unsigned slot = 0; slot < page_count; ++slot) {
        current_state = &page_state(slot);
        current_state->slot = slot;
        tick_page(base, loading);
        // Clipboard ownership can involve other windows; keep it outside the
        // native model write lock and use the latest session code on activation.
        process_clipboard();
    }
    current_state = nullptr;
}
void set_native_menu_callbacks(const overlay::CallbacksV3& callbacks) {
    std::lock_guard lock(callbacks_mutex);
    tools_callbacks = callbacks;
}
} // namespace dingosdk::multiplayer
