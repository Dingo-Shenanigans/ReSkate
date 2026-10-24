#include "throwdown_menu.h"
#include "Extension/Throwdowns/native_flag_cursor_height.h"
#include "Extension/Throwdowns/challenge_registry.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "native_menu_data.h"
#include "native_menu_lifetime.h"
#include "native_menu_ownership.h"
#include "native_setup_roles.h"
#include "native_hud_selection.h"
#include "native_hud_values.h"
#include "Extension/UI/Overlay/overlay.h"
#include "native_menu_dump.h"
#include "native_menu_internal.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/native_flag_bubble.h"
#include "Extension/Throwdowns/native_flag_session.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "native_join_context.h"
#include "native_flag_interaction.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

// The native Throwdowns UI (SmokeyDawg58's): the logo grid, the stock Spot Battle setup page
// relabelled, the waiting card, the in-game HUD, the Throwdown Panel and the map/flag branding.
// Every piece renders the throwdown kit's documents (throwdown_kit.h) and reports what the player
// does as kit events; nothing here knows a particular mode.
namespace dingosdk::multiplayer {
namespace kit = throwdowns::kit;
namespace {
// The native flag registration behind kit::flag (native_flag_session.h): the answers and hooks
// the native page needs that kit::flag::State does not carry.
namespace flag_plumbing {
bool setup_page() noexcept { return native_flag::setup_active(); }
bool retiring() noexcept { return native_flag::registration_retiring(); }
bool owns(std::uint32_t mmid) noexcept { return native_flag::owns_registration(mmid); }
float cursor_height() noexcept { return native_flag::cursor_height(); }
void set_options(unsigned seconds,unsigned players) noexcept { native_flag::set_setup_limits(seconds,players); }
void tick(const menu_data::Context& c) { native_flag::tick(c); }
} // namespace flag_plumbing
using namespace menu_data;
namespace build = game::build::v20260929;
constexpr Schema tile_schema{0xdeb20e0f, 1200}, category_schema{0x628aa99c, 320},
    anchor_schema{0x0e3be640, 192}, label_schema{0xbfe23948, 104}, action_schema{0x85b7eac0, 72},
    panel_schema{0xd29bbf63,576}, header_schema{0x4efe5e19,312}, display_schema{0x1a647e6e,1040},
    setting_schema{0x94803e85,176}, selector_schema{0xf45cb4c0,1344},
    selector_item_schema{0x6d9d5106,312}, notice_schema{0x7290e514,256},
    texture_schema{0x370c9975,144}, content_tile_schema{0xcbe47f59,1184},
    page_action_schema{0x896f8a1a,784};
constexpr std::uint32_t content = 0x716496c8, items = 0x61742cb4, widget = 0x214d4984,
    rows_field = 0x67223da7, text_field = 0x4d8e01b9;
// Reflection NameHash from PlayerProfileViewModel. Graph property hashes from
// native_name_hash are a different namespace and cannot address model fields.
constexpr std::uint32_t profile_name_tag_field = 0x042ddc73;
using Action = MenuAction;
struct Descriptor { alignas(8) std::array<std::byte, 0x70> bytes{}; Address info{}; Action action; };
struct Original { Value tile, category, description; std::array<float,2> tile_size{}, description_size{}; float icon_width{}, icon_height{}; };
struct MatchPanelPlayer { Value row,rank,outcome; bool portrait_attempted{}; std::string picture_url; };
struct MatchPanel {
    ActivityMenuTransitionGate transition;
    float camera_fov_before{};
    std::string match_key;
    Value page, panel, header, header_rows, header_titles, badge, notice, settings, columns, outer_anchor;
    Value quit, board, prompt, prompt_list, prompt_anchor, prompt_item;
    std::array<Value,3> extra_prompts;
    unsigned extra_count{}, prompt_rows{};
    std::array<float,3> board_widths{};
    std::vector<std::uint64_t> player_order;
    Value mounted_slot;
    std::array<Value,3> setting_rows, setting_values, setting_titles, setting_icons, setting_lists, setting_anchors;
    std::array<std::string,3> setting_icon_names;
    unsigned setting_count=3;
    std::map<std::uint64_t,MatchPanelPlayer> players;
    std::uint64_t requested_until{};
    std::uint64_t request_after{};
    std::uint64_t retry_after{};
    bool quitting{};
    bool menu_was_open{};
    bool prompt_visible{};
};
struct ModeCard { Value tile, category, description, logo; };
struct BrandingPresenter { Value slot, saved, model; };
struct BubbleBranding { Value display, saved; Address applied{}; };
struct MapBranding {
    Address map{};
    std::map<Handle,std::vector<BrandingPresenter>> markers;
    std::map<Handle,BubbleBranding> bubbles;
    Value panel;
    std::vector<BrandingPresenter> card;
    std::uint32_t card_mmid{};
};
struct JoinCard {std::vector<BrandingPresenter> presenters;Value badge;};
struct ModeMarker { Value notice, anchor, item; };
struct NativeSetupRows {
    Value list;
    std::vector<Ref> records;
    std::vector<Handle> handles;
    // Keep a typed native owner for the original inline rows. Raw saved bytes
    // cannot retain their nested arrays after the live list changes count.
    Value retained_rows;
    bool stack{};
    bool owned_tiles{};
    Value timer_row;
    unsigned timer_index{};
    std::map<std::string,Value> extra;
    struct Choice { Value selector;std::vector<Handle> options;int shown=-1,pending=-1;std::uint64_t pending_at{}; };
    std::map<std::string,Choice> choices; // extra rows that pick one of several named options
};
struct NativeSetupActions { std::size_t callback_offset{};std::vector<OwnedNativeAction> rows; };
// A setup slider the page adapted: which document row it is ("players", "timer" or a row id), and
// the values last shown from the document and last chosen by the player.
struct SetupSlider { Value model; std::string id; float shown=-1.f, chosen=-1.f; };
// One bounded header owns background, icon and title. It never positions a
// fixed-size logo independently over the centered title's text.
struct ChallengeHeader {
    Value frame,row,content,title,badge,frame_anchor,frame_item,anchor,item;
};
struct BoardCells { Value list, rank, name, value; std::vector<Value> stamps; std::string key; };
// The kit HUD's private native models, by the document part they show.
struct KitHud {
    ChallengeHeader header;
    HudPublicationGate publication;
    std::uint64_t generation{};
    Value stack, title, status, badge;
    Value score, score_anchor, score_item; std::array<Value,4> counters; std::string score_key;
    Value clock, clock_anchor, clock_item;
    Value countdown, countdown_anchor, countdown_item;
    std::uint64_t go_until{}; bool was_counting{};
    Value intro_row, intro_anchor, intro_item, intro_badge, intro_heading, intro_rules, intro_style;
    Value intro_rules_anchor, intro_rules_item, intro_background_anchor, intro_background_item;
    Value blind_anchor, blind_item;
    std::map<std::size_t,BoardCells> rows; Value board_rows; std::vector<Handle> row_order;
    std::array<float,3> widths{100.f,224.f,320.f};
    Value board, board_anchor, board_item, board_title, board_label, board_columns; std::array<Value,3> board_captions; unsigned board_children{};
    Value results, results_anchor, results_item, results_header, results_title, results_notice, results_columns;
    Value results_panel_anchor, results_panel_item; std::array<Value,3> results_captions;
    Value position, position_anchor, position_item;
    std::array<Value,2> timer_values, timer_anchors, timer_items;
    Value timer_caption, timer_caption_anchor, timer_caption_item;
    Value warning, warning_anchor, warning_item;
    Value banner, banner_anchor, banner_item, banner_badge; std::uint64_t banner_serial{}, banner_until{};
    Value callout, callout_list, callout_anchor, callout_item; std::uint64_t callout_serial{}, callout_until{}; std::string callout_text;
    std::map<std::string,Value> callout_styles;
    Value card_caption, card_caption_list, card_caption_anchor, card_caption_item;
    Value card_image, card_image_list, card_image_anchor, card_image_item;
    Value card_title, card_title_list, card_title_anchor, card_title_item;
    bool card_image_visible{}, card_title_visible{};
    Value list_caption, list_list, list_anchor, list_item; std::array<Value,5> list_cells; unsigned list_width{}; std::size_t list_rows{};
    Value style_neon52, style_clean52, style_clean64, style_grey;
    std::map<std::pair<std::uint32_t,std::string>,Value> tints;
    Value compass, compass_list, compass_anchor, compass_item; bool compass_visible{};
    std::array<ModeMarker,32> markers; std::vector<Value> world_items;
    std::map<std::string,Value> glyphs;
    std::array<Value,12> pair_lists, lines, line_captions; std::array<std::string,12> pair_keys, line_keys;
    Value editor_panel_anchor, editor_panel_item, editor_list, editor_anchor, editor_item, editor_title;
    std::array<Value,11> editor_row_anchors, editor_row_items;
    std::vector<Handle> editor_composition;
    bool editor_builder{}; unsigned editor_rows{};
    Value info_list, info_anchor, info_item, info_panel_anchor, info_panel_item; std::array<Value,8> info_labels; bool info_visible{};
    Value toast, toast_list, toast_anchor, toast_item, toast_style; bool toast_visible{};
    Value modal_text, modal_list, modal_anchor, modal_item, modal_panel_anchor, modal_panel_item;
    std::array<Value,2> modal_actions, modal_captions; std::array<std::string,2> modal_keys; std::string modal_key; bool modal_visible{};
};
struct HudSurfaceSlot { Value anchor,item;Widget display{}; };
struct HudSurface {
    Value stack,transition;
    std::map<std::int32_t,HudSurfaceSlot> slots;
    std::set<std::int32_t> visible;
    HudSurfaceContentGate content_gate;
    HudActivationHandoff activation;
    bool revealed{},suspended{};
    std::size_t displayed_count{};
};
struct PromptBinding { std::uint16_t mask{}; std::string command; };
struct State {
    HudPublicationGate hud_publication;
    HudStackActiveBudget hud_active_budget;
    std::map<Handle,HudSurface> hud_surfaces;
    std::map<std::string,ModeCard> mode_cards;
    KitHud kit_hud;
    std::optional<kit::Hud> hud_doc; // the HUD document of the last full tick, for frame-rate clocks
    MapBranding map_branding;
    std::atomic<std::uint32_t> join_mmid{};
    std::map<Handle,JoinCard> join_cards;
    std::string join_report; // join_card_report's last line
    MatchPanel match_panel;
    std::atomic<bool> match_panel_open{};
    std::mutex panel_input_mutex;
    MatchPanelStartInput panel_start_input;
    std::string panel_start_request;
    std::vector<PromptBinding> panel_bindings;
    std::string panel_plugin, panel_mode;
    std::uint16_t panel_buttons{};
    bool in_menu{};
    std::atomic<bool> gameplay_input_allowed{};
    bool panel_key_down{};

    // Borrowed native marker fields, restored when this match releases them.
    std::map<Handle,std::pair<Value,bool>> marker_originals;
    Address manager{}, base{}, anchor_asset{};
    Value page, cards, category, original_body, original_back;
    Value picker_grid, picker_anchor;
    std::array<Value,3> picker_stock, picker_stock_logos;
    Value source_label;
    Value waiting_countdown, waiting_badge;
    Value hud_root, hud_stack;
    std::atomic<bool> hud_ready{};
    std::uint64_t waiting_diagnostic{};
    std::set<Handle> waiting_widgets;
    std::set<Handle> waiting_retired_notifications;
    std::mutex interaction_mutex;
    menu_data::NativeFlagInteraction interaction;
    struct InteractionBranding {Value model,title,icon,badge;std::string applied;};
    InteractionBranding interaction_branding;
    std::uint64_t waiting_stock_revision{};
    struct StockWaitingSlot {Value slot,saved;Handle notification{};};
    std::map<Handle,StockWaitingSlot> stock_waiting_slots;
    std::uint64_t stock_waiting_generation{};

    std::set<Handle> waiting_hidden_slots;
    std::map<Handle,Value> waiting_originals;
    std::map<Handle,std::pair<Value,Value>> waiting_presentations;
    std::map<Handle,Value> waiting_counts;
    std::map<Handle,std::pair<Value,Value>> official_originals;
    std::set<Handle> official_panels;
    std::map<Handle,Value> official_headers;
    std::map<Handle,Value> official_contents;
    std::map<Handle,Handle> official_slot_panels;
    std::map<Handle,SetupSlider> official_sliders;
    std::map<Handle,NativeSetupRows> official_setup_rows;
    std::map<Handle,NativeSetupActions> official_page_actions;
    std::map<Handle,Value> solo_sliders;
    // When the setup page closes into flag placement, the stock page is put back only once its
    // exit transition is over: a write while it closes crashed the game's UI walk (2026-10-20).
    std::uint64_t official_restore_at{};
    std::uint32_t official_game{};
    std::string official_mode;
    Value official_badge;
    std::uint64_t entry_guard_until{};
    std::map<std::string,Address,std::less<>> assets;
    OwnedMenuModels<Value> owned;
    // Models a release let go of, destroyed once the game has had time to unbind them: a native
    // page in its exit transition can still be bound to them (2026-10-20, a 1-UP Confirm).
    std::vector<std::pair<Value,std::uint64_t>> doomed;
    std::vector<Original> originals;
    std::set<std::pair<Handle,Handle>> generations;
    std::array<Descriptor, 128> descriptors;
    unsigned used{};
    std::mutex mutex;
    std::vector<Action> pending;
    std::atomic<std::uint64_t> owner{};
    std::atomic<bool> setup_open{};
    std::uint64_t next_id{}, next_tick{}, pass{};
    std::string original_title;
};
State& state() { static auto* s = new State; return *s; }
void session_marker_controls(const Context& c,bool release=false);
void mode_cards(const Context& c);
void logo_picker(const Context& c);
void mode_art(const Context& c,std::span<const Value> fields,const std::string& logo);
void restore_interaction_branding(const Context& c);
void custom_flag_interaction(const Context& c);
void match_panel_notice_width(const Context&,Value,float);
void match_panel_table_style(const Context&,Value);
void restore_match_panel(const Context& c);
void restore_map_branding(const Context& c);
void restore_join_branding(const Context& c);
void custom_join_details(const Context& c);
void throwdown_map_identity(const Context& c);
void kit_hud_publish(const Context& c,const std::vector<Value>& visible);
bool request_menu_toggle(const Context& c);
// A document's mode as the registry knows it: "<plugin>/<id>" (a document may name the bare id).
std::string full_mode(const std::string& plugin,const std::string& mode) {
    return mode.empty() || mode.find('/')!=std::string::npos?mode:plugin+"/"+mode;
}
// The setup document for the mode whose native flag is at its setup page, if its plugin sent one.
std::optional<kit::Setup> current_setup(const std::string& mode) {
    if(mode.empty())return {};
    const auto docs=kit::documents_for(mode);
    if(!docs || !docs->setup || full_mode(docs->plugin,docs->setup->mode)!=mode)return {};
    auto setup=docs->setup;setup->mode=mode;
    return setup;
}
Value reference(const Context& c, Ref ref) { return {ref.handle, c.type_of(ref.handle)}; }
Value primary(const Context& c, Value tile) {
    return c.path(tile, {widget, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value back(const Context& c, Value page_value) {
    return c.path(page_value, {0x10810f0b, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value body(const Context& c, Value p) { return c.field(c.element(c.path(p, {content, items}), 0), content); }
std::optional<Value> mounted_body(const Context& c, Value p) {
    if(!p.handle || c.type_of(p.handle)!=p.type || !game::native_data().models.value(c.manager,p.handle,0,1))return {};
    const auto children=c.path(p,{content,items});
    const auto pointer=read<Address>(c.address(children));
    // Native page navigation empties the content array while the next view is
    // being installed. This is an ordinary transition, not a failed menu build.
    if(!pointer || !(read<std::uint32_t>(pointer-4)&0x7fffffff))return {};
    return c.field(c.element(children,0),content);
}
Value make(const Context& c, Schema schema, Address authored_type=0) {
    auto& s = state(); const auto id=0x3155500000000000ULL + ++s.next_id;
    Value value;
    if(authored_type) {
        require((kind(authored_type)==2 || kind(authored_type)==4) && size(authored_type)==schema.size && read<std::uint32_t>(read<Address>(authored_type))==schema.hash,
                "Throwdown authored model schema differs.");
        value={game::native_data().models.create(c.manager,authored_type,id,game::native_name_hash("ReSkate.NativeMultiplayerMenu"),false,2),authored_type};
        c.address(value);
        c.initialize_text(value);
    } else value=c.create(schema,id);
    s.owned.track(value); return value;
}
template<std::size_t I> void activate() noexcept {
    auto& s = state();
    try {
        std::unique_lock lock(s.mutex);
        // The native waiting card can outlive its picker generation. These
        // actions are checked against its live private presenter at dispatch.
        if (I < s.used && action_generation_current(s.descriptors[I].action,s.owner.load()) && s.pending.size() < 8 &&
            std::none_of(s.pending.begin(),s.pending.end(),[&](const auto& a){return a.command==s.descriptors[I].action.command && a.argument==s.descriptors[I].action.argument;})) {
            logging::log(logging::Level::debug,logging::Channel::ui,"Throwdowns callback: {} {}.",
                s.descriptors[I].action.command,s.descriptors[I].action.argument);
            const auto now=GetTickCount64();
            const auto& command=s.descriptors[I].action.command;
            // One card press opens one setup; the native button repeats while held.
            if(command=="mode-open") {
                if(now<s.entry_guard_until)return;
                s.entry_guard_until=now+350;
            }
            s.pending.push_back(s.descriptors[I].action);
        }
    } catch (...) {}
}
template<std::size_t... I> auto functions(std::index_sequence<I...>) { return std::array{&activate<I>...}; }
Address callback(const Context& c, std::string command, std::string argument = {}) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto slot=action_slot(s.used,s.descriptors.size(),[&](std::size_t i)->const Action& {return s.descriptors[i].action;},command,argument,s.pass);
    require(slot.has_value(), "Throwdown native action capacity reached.");
    static const auto f = functions(std::make_index_sequence<128>{});
    auto& d = s.descriptors[*slot];
    if(*slot==s.used) {
        const auto prototype = c.base + build::native_menu::action_prototype;
        require(read<Address>(prototype + 0x40) == c.base + build::native_menu::action_invoker && read<std::uint8_t>(prototype + 0x68) == 0, "Throwdown action ABI differs.");
        require(memory::peek_bytes(prototype, d.bytes.data(), d.bytes.size()), "Throwdown action prototype unavailable.");
        const auto invoker = reinterpret_cast<Address>(f[*slot]);
        for (const auto offset : {0x30,0x40,0x48}) std::memcpy(d.bytes.data()+offset, &invoker, 8);
        d.info = reinterpret_cast<Address>(d.bytes.data()); ++s.used;
    }
    // A waiting card may be mounted after its picker has retired (owner=0).
    // Its manager plus the exact presentation check own these delegates.
    const auto generation=waiting_card_action(command)?c.manager:s.owner.load();
    d.action = {generation,std::move(command),std::move(argument),s.pass};
    return reinterpret_cast<Address>(&d.info);
}
Address asset(const Context& c, const char* name) {
    auto& s=state();
    if(const auto cached=s.assets.find(name);cached!=s.assets.end())return cached->second;
    std::vector<Address> anchors;
    std::set<unsigned> domains;
    const auto add_anchor=[&](Address anchor) {
        // A prior map's raw blueprint is not an asset-domain lease. Discard
        // expired anchors locally so they cannot abort resident-domain lookup.
        try {
            if(!anchor)return;
            const auto type=read<Address>(anchor+8);
            if(type!=c.base+build::native_menu::widget_blueprint_type &&
               type!=c.base+build::native_menu::texture_asset_type &&
               type!=c.base+build::native_menu::image_asset_type)return;
            const auto domain=read<std::uint16_t>(anchor+0x16);
            if(domain<0xbbf && read<Address>(c.base+build::engine::domain_owners+domain*8ULL) &&
               domains.insert(domain).second)anchors.push_back(anchor);
        } catch(const std::exception&) { /* The native level retired this anchor. */ }
    };
    // CategoryTile_Widget is a shared foundation asset. Its parent domain
    // cannot find the Throwdown resources in the child domain. The retained
    // badge is owned by that same child bundle, even after its menu retires.
    const auto add_icon=[&](Value model,std::uint32_t field) {
        if(!model.handle || c.type_of(model.handle)!=model.type)return;
        const auto icon=read<Address>(c.address(c.field(model,field)));
        if(!icon)return;
        const auto type=read<Address>(icon+8);
        if(type==c.base+build::native_menu::texture_asset_type || type==c.base+build::native_menu::image_asset_type)
            add_anchor(icon);
    };
    add_icon(s.waiting_badge,0x6e469d2a);
    add_icon(s.category,0x189084da);
    add_anchor(s.anchor_asset);
    // Gameplay and menu presenters can own additional native widget domains.
    for(const auto& root:c.roots({presenter.hash})) {
        const auto blueprint=read<Widget>(root.value).blueprint;
        if(!blueprint || read<Address>(blueprint+8)!=c.base+build::native_menu::widget_blueprint_type)continue;
        add_anchor(blueprint);
        if(domains.size()>=32)break;
    }
    const auto find_loaded=[&](const char* path) {
        for(const auto anchor:anchors)if(const auto found=named_asset(c,anchor,path))return found;
        // Shared widgets may belong to a sibling/resident UI domain after a
        // custom-map switch. Only the engine's loaded-domain lookup is used;
        // no stale asset pointer or unloaded archive entry is retained.
        const auto find=game::native_data().find_asset;
        if(find)for(unsigned candidate=0;candidate<0xbbf;++candidate) {
            if(!read<Address>(c.base+build::engine::domain_owners+candidate*8ULL))continue;
            if(const auto found=find(static_cast<std::uint16_t>(candidate),path))return found;
        }
        return Address{};
    };
    if(const auto found=find_loaded(name)) {s.assets.emplace(name,found);return found;}
    // Exported dynamic-model records are not indexed as ordinary assets.
    // Resolve their actual resource list, then select the exact exported name.
    constexpr std::array containers{
        std::pair{"Activities_Shared_ContentResource/","UI/Features/Activities/Resources/Activities_Shared_ContentResource"},
        std::pair{"Activities_CelebrationMenus_ContentResource/","UI/Features/Activities/Resources/Activities_CelebrationMenus_ContentResource"},
        std::pair{"UI_Layer_Data/","UI/Foundations/Layers/UI_Layer_Data"},
        std::pair{"Activities_DetailsMenu_ContentResource/","UI/Features/Activities/Resources/Activities_DetailsMenu_ContentResource"},
        std::pair{"Throwdown_DetailsMenu_ContentResource/","UI/Features/Activities/Resources/Throwdown_DetailsMenu_ContentResource"},
        std::pair{"Activities_DetailsMenu_ContentResource_Challenge/","UI/Features/Activities/Resources/Activities_DetailsMenu_ContentResource_Challenge"},
        std::pair{"StackPanelStylesList/","UI/Foundations/Components/StackVariations/StackPanel/StackPanelStylesList"},
        std::pair{"TD_HostParameters_ContentResource/","UI/Features/Throwdowns/ContentResources/TD_HostParameters_ContentResource"},
        std::pair{"TD_Page_ContentResources/","UI/Features/Throwdowns/TD_Page_ContentResources"},
        std::pair{"TD_Hud_ContentResources/","UI/Features/Throwdowns/TD_Hud_ContentResources"},
        std::pair{"TD_Scoreboard_ContentResources/","UI/Features/Throwdowns/TD_Scoreboard_ContentResources"},
        std::pair{"Activities_HudElementData_ContentResource/","UI/Features/Activities/Resources/Activities_HudElementData_ContentResource"},
        std::pair{"ActivityBannerStylesList/","UI/Features/Activities/Models/ActivityBannerStylesList"},
        std::pair{"SettingWrapperStylesList/","UI/Foundations/Components/Settings/Widgets/SettingWrapperStylesList"},
        std::pair{"ColorPaletteStylesList/","UI/Foundations/Styles/Lists/ColorPaletteStylesList"},
        std::pair{"TextStylesList/","UI/Foundations/Styles/Lists/TextStylesList"},
        std::pair{"InteractivityColorStylesList/","UI/Foundations/Styles/Lists/InteractivityColorStylesList"},
        std::pair{"NoticeStylesList/","UI/Foundations/Components/Notices/NoticeStylesList"},
        std::pair{"ButtonStyleList/","UI/Foundations/Styles/Lists/ButtonStyleList"},
        std::pair{"","UI/Features/Throwdowns/ContentResources/Throwdown_ModeSelect_ContentResources"}};
    for(const auto& [prefix,path]:containers) if(std::string_view(name).starts_with(prefix)) {
        const Address list=find_loaded(path);
        if(!list || read<Address>(list+8)!=c.base+build::native_menu::asset_list_type)continue;
        const auto records=read<Address>(list+0x20);
        const auto count=records?read<std::uint32_t>(records-4)&0x7fffffff:0;
        require(count<=2048,"Throwdown native resource list exceeds its limit.");
        for(unsigned i=0;i<count;++i) {
            const auto record=read<Address>(records+i*8ULL);
            if(!record || read<Address>(record+8)!=c.base+build::native_menu::asset_record_type)continue;
            const auto exported_name=string(read<Address>(record+0x28),256);
            s.assets.emplace(exported_name,record);
        }
        if(const auto found=s.assets.find(name);found!=s.assets.end())return found->second;
    }
    // Shared foundation widgets can live outside the Throwdowns asset domain.
    // Reuse the native Multiplayer page's discovered, retained blueprints.
    for (unsigned slot=0; slot<menu_view::page_count; ++slot) {
        const auto& page_state = native_menu_detail::page_state(slot);
        if (page_state.base != c.base || page_state.manager != c.manager) continue;
        if (const auto found=page_state.assets.find(name); found!=page_state.assets.end() &&
            (read<Address>(found->second+8)==c.base+build::native_menu::widget_blueprint_type ||
             read<Address>(found->second+8)==c.base+build::native_menu::asset_record_type)) return found->second;
    }
    // Throwdowns can open before the separate ReSkate pause page has loaded.
    // Use the same bounded, validated foundation discovery directly instead
    // of depending on that unrelated page having populated its cache first.
    for(const auto& root:c.roots({core.hash})) {
        try {
            const auto found=blueprints(c,root.model);
            s.assets.insert(found.begin(),found.end());
            if(const auto loaded=s.assets.find(name);loaded!=s.assets.end())return loaded->second;
        } catch(const std::exception&) { }
    }
    throw std::runtime_error(std::string("Throwdown foundation widget unavailable: ")+name);
}
Value category(const Context& c, Value tile) {
    const auto w = read<Widget>(c.address(c.path(tile, {widget, widget})));
    const auto model = reference(c,w.data); require(model.type && size(model.type)==category_schema.size && read<std::uint32_t>(read<Address>(model.type))==category_schema.hash, "Throwdown category schema differs."); return model;
}
Address native_record_type(const Context& c,Address record) {
    // A DataRef may contain a native record or an already materialized handle.
    // Never treat an arbitrary asset's name/fields as record TypeInfo: the game
    // model factory assumes a valid TypeInfo pointer and cannot reject it safely.
    require(record && read<Address>(record+8)==c.base+build::native_menu::asset_record_type,
            "Private Throwdown UI reference is not a native data-model record.");
    const auto type=read<Address>(record+0x18);
    require(type && read<Address>(record+0x20),"Private Throwdown UI record has no type or data.");
    return type;
}
Value record_model(const Context& c, const char* name, Schema schema) {
    const auto record=asset(c,name);
    const auto type=native_record_type(c,record),data=read<Address>(record+0x20);
    // A loaded authored record already carries validated native TypeInfo.
    // Its type need not have a live model yet. The native factory registers it.
    auto result=make(c,schema,type); c.copy(result,data); return result;
}
Value record_model(const Context& c,const char* name) {
    const auto record=asset(c,name),type=native_record_type(c,record);
    require(kind(type)==2,"Throwdown HUD template schema differs.");
    return record_model(c,name,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))});
}
Value clone_widget_model(const Context& c,Value target) {
    auto w=read<Widget>(c.address(target));
    const auto type=w.data.handle?c.type_of(w.data.handle):(w.data.record?native_record_type(c,w.data.record):0);
    require(type && kind(type)==2,"Native HUD widget model unavailable.");
    const auto data=w.data.handle?c.address({w.data.handle,type}):read<Address>(w.data.record+0x20);
    const auto result=make(c,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))},type);
    c.copy(result,data); w.data={0,result.handle}; c.set(target,w); return result;
}
// A typed native copy retains DataRefs; it does not clone the models they
// reference. Copy every mutable UI edge before mounting a private menu. Asset
// styles, expression contexts and backend bindings remain borrowed read-only.
struct PrivateUiGraph {
    const Context& c;
    UiCloneCache<std::pair<Handle,Address>,Value> copies;
    std::set<Handle> private_roots, walked;
    unsigned budget=2400;
    Value clone(Ref source,unsigned depth=0) {
        const auto type=source.handle?c.type_of(source.handle):(source.record?native_record_type(c,source.record):0);
        require(type && (kind(type)==2 || kind(type)==4),"Private Throwdown UI model is unavailable.");
        if(source.handle && (private_roots.contains(source.handle) || walked.contains(source.handle)))return {source.handle,type};
        const auto key=std::pair{source.handle,source.handle?type:source.record};
        return copies.clone(key,[&] {
            require(depth<96 && budget,"Private Throwdown UI graph exceeds its limit.");--budget;
            const auto data=source.handle?c.address({source.handle,type}):read<Address>(source.record+0x20);
            const auto result=make(c,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))},type);
            c.copy(result,data);private_roots.insert(result.handle);return result;
        },[&](Value result){walk(result,depth+1);});
    }
    void walk(Value value,unsigned depth) {
        if(!walked.insert(value.handle).second)return;
        require(depth<96,"Private Throwdown UI graph depth differs.");
        const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==presenter.hash) {
            auto w=read<Widget>(c.address(value));
            if(w.data.handle || w.data.record) {w.data={0,clone(w.data,depth).handle};c.set(value,w);}
            return;
        }
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));
            if(ref.handle || ref.record) {
                const auto type=ref.handle?c.type_of(ref.handle):native_record_type(c,ref.record);
                // Scalar backend expressions are borrowed read-only. Mutable
                // widgets/lists are private, including array-valued models.
                if(type && (kind(type)==2 || kind(type)==4))c.set(value,Ref{0,clone(ref,depth).handle});
            }
            return;
        }
        if(kind(value.type)==4) {
            unsigned count{},stride{};auto bytes=c.array(value,128,count,stride);
            for(unsigned i=0;i<count;++i) {
                const auto child=c.element(value,i);
                if(kind(child.type)==2 || kind(child.type)==4) {
                    // Publishing an inline array element can retire its
                    // siblings' storage. Adapt detached roots, then replace
                    // the counted array once after every element is ready.
                    const auto copy=clone({0,child.handle},depth+1);
                    require(memory::peek_bytes(c.address(copy),bytes.data()+i*stride,stride),"Private UI array copy is unavailable.");
                }
            }
            c.array(value,bytes,count);
        } else if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);
            require(count<=128,"Private Throwdown field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto bytes=c.array(child,128,n,stride);
                    require(stride==sizeof(Handle),"Private UI handle array differs.");
                    std::vector<Handle> handles(n);if(n)std::memcpy(handles.data(),bytes.data(),bytes.size());
                    for(auto& handle:handles)if(handle)handle=clone({0,handle},depth+1).handle;
                    c.array(child,handles);
                } else if(kind(child.type)==2 || kind(child.type)==4)walk(child,depth+1);
            }
        }
    }
};
Value clone_widget_tree(const Context& c,Value slot) {
    auto w=read<Widget>(c.address(slot));
    PrivateUiGraph graph{c};const auto result=graph.clone(w.data);
    w.data={0,result.handle};c.set(slot,w);return result;
}
std::vector<Value> ui_models(const Context& c,Value root,std::uint32_t wanted) {
    std::vector<Value> result;std::set<Handle> seen;unsigned budget=2400;
    std::function<void(Value,unsigned)> visit=[&](Value value,unsigned depth) {
        if(!value.handle || !value.type || !budget || depth>96 || !seen.insert(value.handle).second)return;
        --budget;const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==wanted) {result.push_back(value);return;}
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));if(ref.handle)visit(reference(c,ref),depth+1);return;
        }
        if(kind(value.type)==4) {
            unsigned count{},stride{};c.array(value,128,count,stride);
            for(unsigned i=0;i<count;++i)visit(c.element(value,i),depth+1);
        } else if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);require(count<=128,"Throwdown UI field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto bytes=c.array(child,128,n,stride);require(stride==sizeof(Handle),"UI handle list differs.");
                    for(unsigned j=0;j<n;++j) {
                        Handle handle{};std::memcpy(&handle,bytes.data()+j*stride,stride);
                        if(handle)visit({handle,c.type_of(handle)},depth+1);
                    }
                } else if(kind(child.type)==2 || kind(child.type)==4)visit(child,depth+1);
            }
        }
    };
    visit(root,0);return result;
}
void hud_list(const Context& c,Value& list,const std::vector<Widget>& widgets,bool horizontal,float item_size,float spacing,float last_height=0.f,float width=850.f,float first_width=0.f,bool fit_first=false) {
    if(!list.handle)list=make(c,linear_list);
    // Stock StackComponent always binds a one-item focus window; its blueprint
    // does not connect MaxFocusedItemCount to the list element. A native linear
    // list presents every noninteractive anchored HUD item, without that owner.
    c.set(c.field(list,0x55ca89f4),asset(c,"UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
    c.set(c.field(list,0x4cb61cac),horizontal?0:1);
    c.set(c.field(list,0x64b9a9e8),0); // Preserve the presenters' authored measures.
    c.set(c.field(list,0x19ff199a),item_size);
    c.set(c.field(list,0xebca7354),spacing);
    c.set(c.field(list,0x4554761c),false); // No clipping of the last, variable-height leaderboard.
    c.set(c.field(list,0xafe8434b),false); // No virtualization of penalty rows.
    c.set(c.field(list,0x8bbf7d67),true);
    c.array(c.field(list,rows_field),std::vector<Ref>{});
    std::vector<Handle> handles;
    for(unsigned i=0;i<widgets.size();++i) {
        const auto anchor=make(c,anchor_schema);
        c.set(c.field(anchor,widget),widgets[i]);
        c.set(c.field(anchor,0x144aee01),false);
        // The engine measures the name itself, so penalty stamps follow the
        // actual lettering rather than an empty fixed-width name column.
        c.set(c.field(anchor,0x6efc1a61),horizontal && i==0 && fit_first);
        const float height=(!horizontal && i+1==widgets.size() && last_height>0)?last_height:item_size;
        c.set(c.field(anchor,0x1cff7243),std::array<float,2>{horizontal?(i==0 && first_width>0?first_width:item_size):width,height});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const auto layout=c.field(anchor,axis);
            c.set(c.field(layout,0x99bbed5a),0.f);c.set(c.field(layout,0x49943bed),0.f);
            c.set(c.field(layout,0xfec5e0b1),0.f);c.set(c.field(layout,0xd01f4a21),1.f);
        }
        handles.push_back(anchor.handle);
    }
    c.array(c.field(list,0xf2c90867),handles);
}
void native_timer_seconds(const Context& c,Value timer,std::uint32_t field,std::uint64_t milliseconds) {
    const auto value=c.field(timer,field);
    require(kind(value.type)==17,"Native countdown time is not Uint64.");
    const auto seconds=native_hud_seconds(milliseconds);
    // A new value starts the native number transition. Publish once per
    // displayed second rather than restarting its animation on every tick.
    if(read<std::uint64_t>(c.address(value))!=seconds)c.set(value,seconds);
}
bool hud_list_has_items(const Context& c,Value list,unsigned expected) {
    if(!list.handle)return false;
    unsigned count{},stride{};
    c.array(c.field(list,0xf2c90867),expected+1,count,stride);
    return count==expected;
}
Ref record_ref(const Context& c,const char* name) {
    const auto record=asset(c,name);native_record_type(c,record);return {record,0};
}
void clear_value(const Context& c,Value value) {
    if(kind(value.type)==7) {if(!read<Address>(c.address(value)) || !c.text(value).empty())c.text(value,"");return;}
    if(kind(value.type)==4) {
        // Native array publication reads the count preceding the pointer even
        // for an empty array. A null pointer is not an empty counted array.
        c.array(value,std::span<const std::byte>{},0); return;
    }
    if(kind(value.type)==2) {
        // Bindings can contain counted arrays and owned delegates. Reset from
        // a native-constructed value instead of fabricating zeroed storage.
        const auto defaults=make(c,{read<std::uint32_t>(read<Address>(value.type)),static_cast<std::uint16_t>(size(value.type))},value.type);
        c.copy(value,c.address(defaults)); return;
    }
    const std::vector<std::byte> empty(size(value.type));
    std::vector<std::byte> current(empty.size());
    require(memory::peek_bytes(c.address(value),current.data(),current.size()),"Native reset value is unavailable.");
    if(current!=empty)c.publish(value,empty.data());
}
constexpr std::int32_t hud_panel_id=0x31555076;
void clear_hud_state(bool preserve_match_input=false) {
    auto& s=state();s.hud_ready.store(false);
    s.hud_root={};s.hud_stack={};
    s.match_panel={};s.match_panel_open.store(false);s.kit_hud={};s.hud_doc.reset();
    s.panel_bindings.clear();
    {std::lock_guard input_lock(s.panel_input_mutex);reset_match_panel_input(s.panel_start_input,s.panel_start_request,preserve_match_input);}
    // Waiting notifications outlive HUD rebuilds. Keep their hidden ownership
    // until queue teardown or a genuine native creation/join.
}
template<class PublishRows> void publish_gameplay_hud_rows(const Context& c,Value stack,std::span<const std::byte> rows,unsigned stride,unsigned id_offset,PublishRows&& publish_rows) {
    unsigned owned{};
    for(std::size_t at=0;at<rows.size();at+=stride) {
        std::int32_t id{};std::memcpy(&id,rows.data()+at+id_offset,sizeof(id));
        owned+=custom_hud_item(id);
    }
    const auto limit=c.field(stack,0x6a93bc8a); // MaxFocusedItemCount / MaxActiveItemCount
    const auto live=read<int>(c.address(limit));
    state().hud_active_budget.publish(stack.handle,live,owned,[&](int next) {
        c.set_if_changed(limit,next);
        logging::log(logging::Level::debug,logging::Channel::ui,
            "Custom HUD active budget: stack={:#x}, owned={}, limit={} -> {}.",stack.handle,owned,live,next);
    },std::forward<PublishRows>(publish_rows));
}
void restore_hud_active_budgets(const Context& c) {
    auto& budget=state().hud_active_budget;
    for(const auto handle:budget.stacks()) {
        const auto type=c.type_of(handle);
        if(!type || type!=c.type({0xd71ee0a1,80}))continue;
        const auto limit=c.field(Value{handle,type},0x6a93bc8a);
        budget.update(handle,read<int>(c.address(limit)),0,[&](int next){c.set_if_changed(limit,next);});
    }
    budget.manager_replaced();
}
Value opaque_hud_transition(const Context& c,Value stack,HudSurface& surface) {
    if(surface.transition.handle)return surface.transition;
    const auto checkpoint=state().owned.checkpoint();
    try {
        // The captured default fades 0->1 and scales 1.5->1. These are menu
        // entrance effects, inappropriate for persistent gameplay overlays.
        // Clone every mutable curve edge; never edit shared native styles.
        PrivateUiGraph graph{c};
        const auto style=graph.clone(read<Ref>(c.address(c.field(stack,0x54c9abee))));
        require(read<std::uint32_t>(read<Address>(style.type))==0x69df7d5a && size(style.type)==64,
                "Native HUD transition style differs.");
        const auto curve=[&](std::uint32_t key,std::uint32_t hash) {
            const auto value=reference(c,read<Ref>(c.address(c.field(style,key))));
            require(value.handle && read<std::uint32_t>(read<Address>(value.type))==hash,
                    "Native HUD transition curve differs.");
            return c.field(value,0x600f43a4); // Keyframes, with original input times retained.
        };
        const auto alpha=curve(0x36120f6c,0x6beddefb);
        unsigned count{},stride{};auto points=c.array(alpha,128,count,stride);
        require(count>=2 && stride==8,"Native HUD opacity keyframes differ.");
        const auto alpha_type=c.element(alpha,0).type;
        points=constant_hud_curve<float>(points,stride,c.member(alpha_type,0x2358a817).offset,1.f);
        c.array(alpha,points,count);
        const auto transform=curve(0x5f61032a,0xa5a8bd1f);
        points=c.array(transform,128,count,stride);
        require(count>=2 && stride==80,"Native HUD transform keyframes differ.");
        const auto transform_type=c.element(transform,0).type;
        const std::array<float,16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,0};
        points=constant_hud_curve(points,stride,c.member(transform_type,0x2358a817).offset,identity);
        c.array(transform,points,count);
        surface.transition=style;
        logging::log(logging::Level::info,logging::Channel::ui,
            "Custom HUD private transition: stack={:#x}, opacity=1, transform=identity; stock styles unchanged.",stack.handle);
        return style;
    } catch(...) {
        state().owned.rollback(checkpoint,[&](Value value){c.destroy(value);});
        throw;
    }
}
HudSurfaceSlot& hud_surface_slot(const Context& c,Value stack,std::int32_t id) {
    auto& surface=state().hud_surfaces[stack.handle];surface.stack=stack;
    auto& slot=surface.slots[id];
    if(slot.anchor.handle && slot.item.handle)return slot;
    const auto transition=opaque_hud_transition(c,stack,surface);
    slot.anchor=make(c,anchor_schema);slot.item=make(c,stack_item);
    c.set(c.field(slot.anchor,widget),Widget{});
    c.set(c.field(slot.item,0x1e95752c),id);
    c.set(c.field(slot.item,0xd78d240b),Ref{0,transition.handle});
    c.set(c.field(slot.item,content),Widget{asset(c,"UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"),{0,slot.anchor.handle}});
    c.set(c.field(slot.item,0x369babfe),true);
    for(const auto track:{0x8c0583eeU,0xc5c7cc11U}) {
        c.set(c.path(slot.item,{track,0x20247753}),1.f);c.set(c.path(slot.item,{track,0x2426103f}),1.f);
        c.set(c.path(slot.item,{track,0x1e3ce013}),true);
    }
    return slot;
}
void hide_hud_surface_content(const Context& c,HudSurface& surface) {
    surface.content_gate.reset();surface.revealed=false;
    for(auto& [id,slot]:surface.slots)if(slot.anchor.handle && c.type_of(slot.anchor.handle)==slot.anchor.type) {
        const auto value=c.field(slot.anchor,widget);
        const auto display=read<Widget>(c.address(value));
        if(display.blueprint)slot.display=display;
        c.set_if_changed(value,Widget{});
    }
}
void suspend_hud_surface_content(const Context& c) {
    for(auto& [handle,surface]:state().hud_surfaces) {
        surface.suspended=true;hide_hud_surface_content(c,surface);
    }
}
void publish_hud_surface(const Context& c,Value stack,std::span<const Value> visible,bool mode) {
    auto& s=state();auto found=s.hud_surfaces.find(stack.handle);
    if(found==s.hud_surfaces.end() && visible.empty())return;
    auto& surface=s.hud_surfaces[stack.handle];surface.stack=stack;
    clear_hud_publisher_visibility(surface.visible,mode);
    for(const auto item:visible)if(item.handle)surface.visible.insert(read<int>(c.address(c.field(item,0x1e95752c))));
    const auto target=c.field(stack,items);unsigned count{},stride{};
    const auto old=c.array(target,custom_hud_stack_item_limit,count,stride);
    require(stride==stack_item.size,"Native stable HUD stack ABI differs.");
    const auto id_offset=c.member(c.type(stack_item),0x1e95752c).offset;
    const auto presenter_offset=c.member(c.type(stack_item),content).offset;
    std::vector<std::byte> next;
    for(unsigned i=0;i<count;++i) {
        std::int32_t id{};std::memcpy(&id,old.data()+i*stride+id_offset,4);
        if(!custom_hud_item(id))next.insert(next.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
    }
    if(!surface.visible.empty())for(const auto id:custom_hud_slot_order) {
        auto& slot=hud_surface_slot(c,stack,id);
        // Dynamic artwork (rank color, badge, results panel) is still authored
        // through the same private anchor. Remember it before blanking a slot.
        const auto display=read<Widget>(c.address(c.field(slot.anchor,widget)));
        if(display.blueprint)slot.display=display;
        const auto at=next.size();next.resize(at+stride);
        require(memory::peek_bytes(c.address(slot.item),next.data()+at,stride),"Native stable HUD source unavailable.");
        next[at+c.member(c.type(stack_item),0x369babfe).offset]=std::byte{1};
    }
    const auto stable=reconcile_hud_items<Widget>(old,next,stride,id_offset,presenter_offset);
    if(stable)hide_hud_surface_content(c,surface);
    publish_gameplay_hud_rows(c,stack,next,stride,id_offset,[&] {
        if(stable) {
            c.array(target,*stable,static_cast<unsigned>(stable->size()/stride));
            surface.activation.remounted(GetTickCount64());
            logging::log(logging::Level::info,logging::Channel::ui,
                "Stable HUD presenters mounted: stack={:#x}, native_items={}, custom_active_items={}; content waits for native activation.",
                stack.handle,next.size()/stride-(surface.visible.empty()?0:custom_hud_slot_order.size()),
                surface.visible.empty()?0:custom_hud_slot_order.size());
        }
    });
    if(surface.visible.empty()) {hide_hud_surface_content(c,surface);return;}
    if(surface.suspended) {
        // A real native menu retires/rebinds item contexts even though its
        // retained presenter identities stay the same. Bootstrap that return
        // once; ordinary content changes never set suspended.
        surface.suspended=false;surface.activation.remounted(GetTickCount64());
    }
    // Context construction follows the initial list notification. Once it
    // exists, one same-identity bootstrap refresh supplies the full active
    // state before any content is revealed. New HUD elements never do this.
    const bool handoff=surface.activation.due(GetTickCount64());
    if(handoff) {
        c.array(target,next,static_cast<unsigned>(next.size()/stride));
        surface.activation.published();surface.content_gate.reset();
        logging::log(logging::Level::info,logging::Channel::ui,
            "Stable HUD activation handoff: stack={:#x}; presenter count/IDs unchanged, content still blank.",stack.handle);
    }
    // Native content creation can temporarily reset IsActive/interpolation for
    // every initialized sibling. The private curves stay opaque/identity, so
    // copying live inline items or blanking their content would cause the
    // very flicker this path must avoid. Only real mount/menu gates hide it.
    const bool settled=!stable && !handoff && !surface.activation.pending();
    const bool reveal=surface.content_gate.allow(settled,GetTickCount64());
    if(reveal && (!surface.revealed || surface.displayed_count!=surface.visible.size()))
        logging::log(logging::Level::debug,logging::Channel::ui,
            "Stable HUD content visible: stack={:#x}, visible={}, mounted={}; Items unchanged.",
            stack.handle,surface.visible.size(),custom_hud_slot_order.size());
    surface.revealed=reveal;surface.displayed_count=surface.visible.size();
    for(auto& [id,slot]:surface.slots)
        c.set_if_changed(c.field(slot.anchor,widget),reveal && surface.visible.contains(id)?slot.display:Widget{});
}
void detach_hud_surfaces(const Context& c) {
    for(auto& [handle,surface]:state().hud_surfaces) {
        hide_hud_surface_content(c,surface);
        if(c.type_of(handle)!=surface.stack.type)continue;
        const auto target=c.field(surface.stack,items);unsigned count{},stride{};
        const auto old=c.array(target,custom_hud_stack_item_limit,count,stride);
        const auto offset=c.member(c.type(stack_item),0x1e95752c).offset;
        std::vector<std::byte> next;
        for(unsigned i=0;i<count;++i) {
            int id{};std::memcpy(&id,old.data()+i*stride+offset,4);
            if(!custom_hud_item(id))next.insert(next.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
        }
        publish_gameplay_hud_rows(c,surface.stack,next,stride,offset,[&] {
            if(next.size()!=old.size())c.array(target,next,static_cast<unsigned>(next.size()/stride));
        });
    }
    state().hud_surfaces.clear();
}
// The match panel's prompt is the only item of the panel-range publisher: the kit HUD's pieces
// publish in the mode range (kit_hud_publish).
void publish_prompt(const Context& c) {
    auto& s=state();if(!s.hud_stack.handle)return;
    if(c.type_of(s.hud_stack.handle)!=s.hud_stack.type) {s.hud_ready.store(false);return;}
    const bool prompt=s.match_panel.prompt_visible && !s.in_menu && !s.match_panel.quitting;
    const bool ready=hud_publication_allowed(read<bool>(c.address(c.field(s.hud_stack,0x369babfe))),s.in_menu,
        prompt && s.match_panel_open.load());
    const bool settled=s.hud_publication.allow(s.hud_stack.handle,s.kit_hud.generation,ready,GetTickCount64());
    if(!ready || (prompt && !settled)) {
        if(const auto found=s.hud_surfaces.find(s.hud_stack.handle);found!=s.hud_surfaces.end()) {
            if(!ready)found->second.suspended=true;
            hide_hud_surface_content(c,found->second);
        }
        return;
    }
    std::vector<Value> visible;
    if(prompt && s.match_panel.prompt_item.handle)visible.push_back(s.match_panel.prompt_item);
    publish_hud_surface(c,s.hud_stack,visible,false);
}
void plain_label(const Context& c,Value value,const std::string& text) {
    const auto label_text=c.field(value,text_field);
    if(!read<Address>(c.address(label_text)) || c.text(label_text,2048)!=text) c.text(label_text,text);
    const auto visible=c.field(value,0x042924a4);
    if(!read<bool>(c.address(visible)))c.set(visible,true);
}
bool setup_body_content_matches(const Context& c,Value live,Value saved) {
    unsigned live_count{},live_stride{},saved_count{},saved_stride{};
    const auto live_items=c.array(c.field(live,items),128,live_count,live_stride);
    const auto saved_items=c.array(c.field(saved,items),128,saved_count,saved_stride);
    require(live_stride==stack_item.size && saved_stride==stack_item.size,"Native setup body item schema differs.");
    const auto offset=c.member(c.type(stack_item),content).offset;
    return matching_presenters<Widget>(live_items,saved_items,stack_item.size,offset);
}
void mount_setup_body(const Context& c,Value live,Value saved) {
    // Array publication makes a separate allocation. Compare presenter identity,
    // never its address or the native transition/focus fields surrounding Items.
    const auto live_array=c.field(live,items),saved_array=c.field(saved,items);
    unsigned live_count{},live_stride{},saved_count{},saved_stride{};
    const auto live_items=c.array(live_array,128,live_count,live_stride);
    const auto saved_items=c.array(saved_array,128,saved_count,saved_stride);
    require(live_stride==stack_item.size && saved_stride==stack_item.size,"Native setup body item schema differs.");
    const auto offset=c.member(c.type(stack_item),content).offset;
    // The body still has the same authored notice/list slots. Change just their
    // presenters; do not restart the StackItems' native visibility animation.
    if(const auto next=remount_presenters<Widget>(live_items,saved_items,stack_item.size,offset))
        c.array(live_array,*next,saved_count);
}
void restore_official(const Context& c) {
    auto& s=state();
    std::set<Handle> owned_panels;
    for(const auto& [panel,header]:s.official_headers) {
        if(c.type_of(panel)!=c.type(panel_schema) || !game::native_data().models.value(c.manager,panel,0,1))continue;
        if(read<Widget>(c.address(c.field({panel,c.type(panel_schema)},0xc0f1b449))).data.handle==header.handle)owned_panels.insert(panel);
    }
    for(const auto& [id,pair]:s.official_originals) {
        // A retired panel can still have a typed handle after its embedded
        // storage disappears. That must not block every later menu update.
        if(c.type_of(id)!=pair.first.type || !game::native_data().models.value(c.manager,id,0,1))continue;
        if(const auto owned=s.official_page_actions.find(id);owned!=s.official_page_actions.end()) {
            unsigned count{},stride{},saved_count{},saved_stride{};
            const auto live=c.array(pair.first,8,count,stride),saved=c.array(pair.second,8,saved_count,saved_stride);
            require(stride==page_action_schema.size && saved_stride==stride,"Native setup action schema differs during cleanup.");
            if(const auto restored=restore_owned_actions(live,saved,stride,owned->second.callback_offset,owned->second.rows))
                c.array(pair.first,*restored,count);
            continue;
        }
        // Body is an inline native StackComponent, whereas header/footer are
        // Widget presenters. Restore only content that is still ours.
        const auto panel=s.official_slot_panels.find(id);
        if(panel==s.official_slot_panels.end())continue;
        if(read<std::uint32_t>(read<Address>(pair.first.type))==0xd71ee0a1) {
            // A new native menu can have replaced the header first. Restore the
            // body only while it still presents our controls, and leave its
            // active/selection/transition state under the native owner's control.
            const auto private_body=s.official_contents.find(id);
            if(private_body!=s.official_contents.end() && setup_body_content_matches(c,pair.first,private_body->second))
                mount_setup_body(c,pair.first,pair.second);
        } else if(owned_panels.contains(panel->second))c.copy(pair.first,c.address(pair.second));
    }
    s.official_originals.clear(); s.official_panels.clear(); s.official_headers.clear(); s.official_contents.clear(); s.official_slot_panels.clear(); s.official_sliders.clear();s.official_setup_rows.clear();s.official_page_actions.clear();
}
void remember_official(const Context& c,Value value) {
    auto& originals=state().official_originals;
    if(originals.contains(value.handle))return;
    const auto copy=make(c,{read<std::uint32_t>(read<Address>(value.type)),static_cast<std::uint16_t>(size(value.type))},value.type);
    c.copy(copy,c.address(value)); originals.emplace(value.handle,std::pair{value,copy});
}
void label_styles(const Context& c,Value label,const char* idle,const char* focused) {
    c.set_if_changed(c.field(label,0xf94d8cc6),record_ref(c,idle));
    c.set_if_changed(c.field(label,0xa1e84eb1),record_ref(c,focused));
    c.set_if_changed(c.field(label,0xfc23a999),record_ref(c,idle));
    c.set_if_changed(c.field(label,0x25189231),record_ref(c,focused));
}
Value description(const Context& c, Value cat) { return reference(c, read<Widget>(c.address(c.field(cat,widget))).data); }
std::optional<Value> card_list(const Context& c, Value p) {
    try {
        const auto slot=mounted_body(c,p);if(!slot)return {};
        auto w = read<Widget>(c.address(*slot));
        // The private grid replaces the presenter, not the stock list. Retain
        // the original list as the generation's identity while our grid mounts.
        const auto& s=state();
        if(p.handle==s.page.handle && s.picker_anchor.handle && w.data.handle==s.picker_anchor.handle &&
           c.type_of(s.cards.handle)==s.cards.type) return s.cards;
        for (unsigned depth=0; depth<4; ++depth) {
            auto value = reference(c,w.data); if (!value.type) return {};
            const auto hash = read<std::uint32_t>(read<Address>(value.type));
            if (hash == linear_list.hash && size(value.type)==linear_list.size) {
                unsigned count{}, stride{}; auto bytes=c.array(c.field(value,rows_field),4,count,stride);
                if (count!=3 || stride!=sizeof(Ref)) return {};
                // The live Skate Jam card navigates through ThrowdownerActive;
                // JamSession is its gameplay mode, not its menu action name.
                const std::array<std::string_view,3> expected{"ThrowdownSkate","SpotBattle","ThrowdownerActive"};
                for (unsigned i=0;i<3;++i) {
                    Ref ref{}; std::memcpy(&ref,bytes.data()+i*stride,stride); auto t=reference(c,ref);
                    if (!t.type || size(t.type)!=tile_schema.size || read<std::uint32_t>(read<Address>(t.type))!=tile_schema.hash ||
                        c.text(c.field(primary(c,t),0xb1e14cd6)) != expected[i]) return {};
                }
                return value;
            }
            if (hash!=anchor_schema.hash || size(value.type)!=anchor_schema.size) return {};
            w=read<Widget>(c.address(c.field(value,widget)));
        }
    } catch (...) {}
    return {};
}
// Every model of a retired UI manager is gone with it: forget them without touching it.
void forget_manager() {
    auto& s=state();
    s.doomed.clear(); // they went with the old manager
    s.owned.manager_replaced(); s.hud_active_budget.manager_replaced(); s.hud_surfaces.clear();
    s.picker_grid={};s.picker_anchor={};s.picker_stock={};s.picker_stock_logos={};s.mode_cards.clear();
    s.map_branding={};s.interaction_branding={};{std::lock_guard interaction_lock(s.interaction_mutex);s.interaction={};}
    s.join_cards.clear();s.join_mmid.store(0);s.marker_originals.clear(); clear_hud_state();
    s.official_originals.clear(); s.official_panels.clear(); s.official_headers.clear(); s.official_contents.clear(); s.official_slot_panels.clear();
    s.official_sliders.clear();s.official_setup_rows.clear();s.official_page_actions.clear(); s.solo_sliders.clear();s.official_restore_at=0;
    s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_presentations.clear(); s.waiting_counts.clear();
    s.waiting_retired_notifications.clear(); s.waiting_hidden_slots.clear(); s.waiting_stock_revision=0;s.stock_waiting_slots.clear();s.stock_waiting_generation=0;
    s.waiting_countdown={}; s.waiting_badge={}; s.waiting_diagnostic=0; s.owner.store(0); s.setup_open.store(false);
    s.page={}; s.cards={}; s.official_badge={};s.official_game=0;s.official_mode.clear(); s.originals.clear(); s.assets.clear();s.anchor_asset=0;
    s.generations.clear();
}
// The stock card page's body, Back and actions as the game made them, while they still show ours.
void restore_page(const Context& c) {
    auto& s=state();
    if(const auto slot=mounted_body(c,s.page)) {
        const auto mounted=read<Widget>(c.address(*slot)).data.handle;
        if(mounted==s.picker_anchor.handle) {
            c.copy(*slot,c.address(s.original_body));
            c.copy(back(c,s.page),c.address(s.original_back));
            c.text(c.path(s.page,{0x3781b603,text_field}),s.original_title);
        }
    }
    if(s.cards.handle && c.type_of(s.cards.handle)==s.cards.type) {
        unsigned count{},stride{};const auto bytes=c.array(c.field(s.cards,rows_field),64,count,stride);
        require(stride==sizeof(Ref),"Throwdowns card list changed during picker retirement.");
        std::vector<Ref> retained;
        for(unsigned i=0;i<count;++i) {
            Ref ref{};std::memcpy(&ref,bytes.data()+i*stride,stride);
            if(std::none_of(s.mode_cards.begin(),s.mode_cards.end(),
                [&](const auto& entry){return entry.second.tile.handle==ref.handle;}))retained.push_back(ref);
        }
        c.array(c.field(s.cards,rows_field),retained);
    }
    for(const auto& o:s.originals) {
        if(c.type_of(o.tile.handle)==o.tile.type)c.set(c.field(o.tile,0x868043e1),o.tile_size);
        if(c.type_of(o.category.handle)==o.category.type) {
            c.set(c.field(o.category,0x495ffd43),o.icon_width);c.set(c.field(o.category,0x24fb5ca8),o.icon_height);
        }
        if(c.type_of(o.description.handle)==o.description.type)c.set(c.field(o.description,0x1cff7243),o.description_size);
    }
}
void release(const Context& c,bool preserve_match_input=false) {
    auto& s=state(); s.owner.store(0);
    if(preserve_match_input) {
        // A picker generation is not a world/UI-manager lifetime. Retiring
        // it during Start previously destroyed the active HUD, panel and
        // delegates while native navigation still retained those references.
        restore_page(c);
        // Retained roots are released by the existing real world/manager
        // cleanup. Native cached pages can still reference this generation.
        s.page={};s.cards={};
        s.picker_grid={};s.picker_anchor={};s.picker_stock={};s.picker_stock_logos={};
        s.mode_cards.clear();s.originals.clear();s.setup_open.store(false);
        logging::write(logging::Level::info,logging::Channel::ui,
            "Throwdowns: picker generation retired; active gameplay HUD, native panel and delegates retained.");
        return;
    }
    restore_interaction_branding(c);
    {std::lock_guard interaction_lock(s.interaction_mutex);s.interaction={};}
    restore_join_branding(c);s.join_mmid.store(0);
    restore_map_branding(c);
    restore_match_panel(c);
    session_marker_controls(c,true);
    s.solo_sliders.clear();
    s.waiting_retired_notifications.insert(s.waiting_widgets.begin(),s.waiting_widgets.end());
    for(const auto& [handle,presentation]:s.waiting_presentations)
        if(c.type_of(handle)==presentation.first.type) {
            const auto at=game::native_data().models.value(c.manager,handle,0,1);
            if(!at)continue;
            const auto current=read<Widget>(at).data.handle;
            const bool owned=(s.waiting_hidden_slots.contains(handle) && !current) || std::any_of(s.waiting_originals.begin(),s.waiting_originals.end(),[&](const auto& entry) {
                return current==entry.second.handle || current==c.field(entry.second,0xc13bcfcd).handle;
            });
            // The donor is a temporary backend queue. Restoring its original
            // Spot Battle card during exit exposes a challenge we never joined.
            if(owned)c.set(presentation.first,Widget{});
        }
    s.waiting_presentations.clear();
    s.waiting_hidden_slots.clear();s.stock_waiting_slots.clear();s.stock_waiting_generation=0;
    { std::lock_guard lock(s.mutex);
      // Match controls belong to the match, not the picker generation.
      std::erase_if(s.pending,[](const Action& a){return a.command!="panel-quit" && a.command!="panel-open" && !waiting_card_action(a.command);}); }
    s.owned.release([&] {
        detach_hud_surfaces(c);restore_hud_active_budgets(c);s.hud_ready.store(false);
        restore_official(c);
        s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_presentations.clear(); s.waiting_counts.clear();
        s.waiting_countdown={}; s.waiting_badge={}; s.waiting_diagnostic=0;
        restore_page(c);
    },[&](Value value){s.doomed.emplace_back(value,GetTickCount64()+3000);});
    if(!s.doomed.empty())logging::log(logging::Level::info,logging::Channel::ui,
        "Throwdowns: released the page's native models; {} destroyed in 3 s.",s.doomed.size());
    s.page={}; s.cards={}; s.official_badge={};s.official_game=0;s.official_mode.clear();
    s.picker_grid={};s.picker_anchor={};s.picker_stock={};s.picker_stock_logos={};
    s.mode_cards.clear();s.map_branding={};s.interaction_branding={};{std::lock_guard interaction_lock(s.interaction_mutex);s.interaction={};}
    s.join_cards.clear();s.join_mmid.store(0);s.originals.clear(); s.assets.clear();s.anchor_asset=0;
    clear_hud_state(preserve_match_input);
    s.setup_open.store(false,std::memory_order_release);
}
void initialize(const Context& c, Value p, Value list) {
    auto& s=state(); s.page=p; s.cards=list; s.owner.store(p.handle);
    unsigned count{},stride{}; auto bytes=c.array(c.field(list,rows_field),3,count,stride);
    std::vector<Ref> refs(count); std::memcpy(refs.data(),bytes.data(),bytes.size());
    for (const auto& ref:refs) {
        auto tile=reference(c,ref), cat=category(c,tile), desc=description(c,cat);
        s.originals.push_back({tile,cat,desc,read<std::array<float,2>>(c.address(c.field(tile,0x868043e1))),read<std::array<float,2>>(c.address(c.field(desc,0x1cff7243))),
            read<float>(c.address(c.field(cat,0x495ffd43))),read<float>(c.address(c.field(cat,0x24fb5ca8)))});
    }
    const auto donor=s.originals[1]; const auto card_widget=read<Widget>(c.address(c.path(donor.tile,{widget,widget})));
    s.anchor_asset=card_widget.blueprint;
    const auto donor_label=reference(c,read<Widget>(c.address(c.field(donor.description,widget))).data);
    require(donor_label.type && size(donor_label.type)==label_schema.size,"Throwdowns description label unavailable.");
    // Gameplay and the setup page outlive the picker that supplied this
    // label. Own its typed copy instead of borrowing a retired native page.
    s.source_label=make(c,label_schema,donor_label.type);c.copy(s.source_label,c.address(donor_label));
    // The Skate Jam category's resources resolve the Throwdowns asset domain (asset()).
    s.category=make(c,category_schema,s.originals[2].category.type); c.copy(s.category,c.address(s.originals[2].category));
    s.original_body=make(c,presenter,body(c,p).type); c.copy(s.original_body,c.address(body(c,p)));
    s.original_back=make(c,action_schema,back(c,p).type); c.copy(s.original_back,c.address(back(c,p)));
    s.original_title=c.text(c.path(p,{0x3781b603,text_field}));
    // The cards only need their three live siblings. Setup content is adapted
    // after the native screen opens; unavailable optional widgets must never
    // prevent the plugin cards from appearing.
    mode_cards(c);
    logo_picker(c);
    s.generations.emplace(p.handle,list.handle);
    logging::write(logging::Level::info,logging::Channel::ui,"Throwdowns: native logo grid registered below the page heading.");
}
// Adapt the ACTUAL native setup mounted by SpotBattle. Its stack, anchors,
// transitions, footer and Confirm navigation stay under the game's ownership.
// Removes the stock rows the setup document hides ("rounds", ...) from a private copy.
void remove_native_rows(const Context& c,Value root,const std::vector<std::string>& hidden) {
    std::set<Handle> seen;
    std::function<bool(Value,unsigned)> visit=[&](Value value,unsigned depth) -> bool {
        require(depth<96,"Throwdown settings graph depth differs.");
        if(!value.handle)return false;
        const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==setting_schema.hash) {
            const auto id=native_setup_role_id(native_setup_role(c.text(c.path(value,{0x58c9d354,text_field}))));
            return !id.empty() && std::find(hidden.begin(),hidden.end(),id)!=hidden.end();
        }
        if(!seen.insert(value.handle).second)return false;
        struct Unvisit { std::set<Handle>& seen;Handle handle;~Unvisit(){seen.erase(handle);} } unvisit{seen,value.handle};
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));
            return ref.handle && visit(reference(c,ref),depth+1);
        }
        if(kind(value.type)==4) {
            unsigned n{},stride{};const auto old=c.array(value,128,n,stride);
            std::vector<std::byte> kept;
            for(unsigned i=0;i<n;++i) {
                const auto child=c.element(value,i);
                if(kind(child.type)==2 || kind(child.type)==4) {
                    const auto copy=make(c,{read<std::uint32_t>(read<Address>(child.type)),static_cast<std::uint16_t>(size(child.type))},child.type);
                    c.copy(copy,c.address(child));
                    if(visit(copy,depth+1))continue;
                    const auto offset=kept.size();kept.resize(offset+stride);
                    require(memory::peek_bytes(c.address(copy),kept.data()+offset,stride),"Private settings row is unavailable.");
                } else kept.insert(kept.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
            }
            if(kept!=old)c.array(value,kept,static_cast<unsigned>(kept.size()/stride));
            return false; // Removed an individual row, not its enclosing list.
        }
        bool remove=false;
        if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);require(count<=128,"Throwdown settings field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto old=c.array(child,128,n,stride);
                    require(stride==sizeof(Handle),"Throwdown setting handle list differs.");
                    std::vector<Handle> kept;
                    for(unsigned j=0;j<n;++j) {
                        Handle handle{};std::memcpy(&handle,old.data()+j*stride,stride);
                        if(!handle || !visit({handle,c.type_of(handle)},depth+1))kept.push_back(handle);
                    }
                    if(kept.size()!=n)c.array(child,kept);
                } else if(kind(child.type)==2 || kind(child.type)==4)remove=visit(child,depth+1)||remove;
            }
        }
        return remove;
    };
    visit(root,0);
}
NativeSetupRows native_setup_rows(const Context& c,Value body_value) {
    // HostParameters materializes its settings into the native LinearList's
    // OwnedItems ContentPresenterTile array. ListItems/ListItemHandles are
    // legitimate alternate representations but are empty in the captured page.
    for(const auto list:ui_models(c,body_value,linear_list.hash)) {
        NativeSetupRows result;result.list=list;
        const auto owned=c.field(list,0x345b75b1);unsigned owned_count{},owned_stride{};
        c.array(owned,128,owned_count,owned_stride);
        require(owned_stride==content_tile_schema.size,"Native setup OwnedItems tile schema differs.");
        bool owned_players{};unsigned timer_index{};
        for(unsigned i=0;i<owned_count;++i) {
            const auto row=c.element(owned,i);
            for(const auto setting:ui_models(c,row,setting_schema.hash)) {
                const auto role=native_setup_role(c.text(c.path(setting,{0x58c9d354,text_field})));
                owned_players=owned_players || role==NativeSetupRole::players;
                if(role==NativeSetupRole::timer) {result.timer_row=row;timer_index=i;}
            }
        }
        if(owned_players && result.timer_row.handle) {
            result.retained_rows=make(c,{read<std::uint32_t>(read<Address>(owned.type)),
                static_cast<std::uint16_t>(size(owned.type))},owned.type);
            c.copy(result.retained_rows,c.address(owned));
            result.timer_row=c.element(result.retained_rows,timer_index);
            result.timer_index=timer_index;result.owned_tiles=true;return result;
        }
        result.timer_row={};
        unsigned records_count{},records_stride{},handles_count{},handles_stride{};
        const auto records=c.array(c.field(list,rows_field),128,records_count,records_stride);
        const auto handles=c.array(c.field(list,0xf2c90867),128,handles_count,handles_stride);
        require(records_stride==sizeof(Ref) && handles_stride==sizeof(Handle),"Native setup list item types differ.");
        if(records_count && handles_count)continue;
        result.records.resize(records_count);result.handles.resize(handles_count);
        if(records_count)std::memcpy(result.records.data(),records.data(),records.size());
        if(handles_count)std::memcpy(result.handles.data(),handles.data(),handles.size());
        bool players{};
        const auto inspect=[&](Value row) {
            for(const auto setting:ui_models(c,row,setting_schema.hash)) {
                const auto id=c.text(c.path(setting,{0x58c9d354,text_field}));
                const auto role=native_setup_role(id);
                if(role==NativeSetupRole::players)players=true;
                if(role==NativeSetupRole::timer)result.timer_row=row;
            }
        };
        for(const auto ref:result.records)if(ref.handle)inspect(reference(c,ref));
        for(const auto handle:result.handles)if(handle)inspect({handle,c.type_of(handle)});
        if(players && result.timer_row.handle)return result;
    }
    // StackPanel.Body is the exported StackViewModel (0xd71ee0a1). Some
    // native HostParameters generations mount each setting as a StackItem
    // instead of using a nested LinearList. Inspect its real Items contract.
    if(read<std::uint32_t>(read<Address>(body_value.type))==0xd71ee0a1) {
        NativeSetupRows result;result.list=body_value;result.stack=true;
        const auto items_value=c.field(body_value,items);unsigned count{},stride{};
        c.array(items_value,128,count,stride);
        require(stride==stack_item.size,"Native HostParameters StackItem schema differs.");
        bool players{};unsigned timer_index{};
        for(unsigned i=0;i<count;++i) {
            const auto row=c.element(items_value,i);
            const auto settings=ui_models(c,row,setting_schema.hash);
            if(settings.size()!=1)continue;
            const auto id=c.text(c.path(settings.front(),{0x58c9d354,text_field}));
            const auto role=native_setup_role(id);
            if(role==NativeSetupRole::players)players=true;
            if(role==NativeSetupRole::timer) {result.timer_row=row;timer_index=i;}
        }
        if(players && result.timer_row.handle) {
            result.retained_rows=make(c,{read<std::uint32_t>(read<Address>(items_value.type)),
                static_cast<std::uint16_t>(size(items_value.type))},items_value.type);
            c.copy(result.retained_rows,c.address(items_value));
            result.timer_row=c.element(result.retained_rows,timer_index);
            result.timer_index=timer_index;return result;
        }
    }
    throw std::runtime_error("Native HostParameters list with Players and Timer is not mounted yet.");
}
Value native_setup_extra(const Context& c,NativeSetupRows& rows,const std::string& key,const std::string& caption) {
    if(const auto found=rows.extra.find(key);found!=rows.extra.end())return found->second;
    PrivateUiGraph graph{c};const auto row=graph.clone({0,rows.timer_row.handle});
    const auto settings=ui_models(c,row,setting_schema.hash);
    require(settings.size()==1,"Native setup template must contain one SettingWrapper.");
    plain_label(c,c.field(settings.front(),0x58c9d354),caption);
    if(rows.stack)c.set(c.field(row,0x1e95752c),static_cast<std::int32_t>(2000+rows.extra.size()));
    rows.extra.emplace(key,row);return row;
}
void native_setup_action_row(const Context& c,NativeSetupRows& rows,const std::string& key,
        const std::string& caption,const std::string& value,const std::string& command,const std::string& argument) {
    const bool fresh=!rows.extra.contains(key);
    const auto checkpoint=state().owned.checkpoint();
    try {
    const auto row=native_setup_extra(c,rows,key,caption);
    const auto setting=ui_models(c,row,setting_schema.hash).front();
    plain_label(c,c.field(setting,0x58c9d354),caption);
    auto slot=c.field(setting,content);
    Value control;
    if(fresh) {
        control=make(c,button);
        // Reuse the game's setting label's immutable font styles, with a fresh
        // LabelButton input model. No shared host parameter/backend binding.
        c.copy(c.field(control,0x58c9d354),c.address(c.field(setting,0x58c9d354)));
        const auto donor=record_model(c,"Throwdown_DetailsMenu_ContentResource/ThrowdownDetails_HUDButton",button);
        for(const auto field:{0x8cc042efU,0xf9515817U,0xf343d160U})
            c.copy(c.field(control,field),c.address(c.field(donor,field)));
        clear_value(c,c.field(control,0x67212c85));
        c.set(c.path(control,{0x0fb0d794,0x8cc042ef}),record_ref(c,"ButtonStyleList/TileButton.RoughPartial.Default"));
        c.set(slot,Widget{asset(c,"UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget"),{0,control.handle}});
    } else control=reference(c,read<Widget>(c.address(slot)).data);
    plain_label(c,c.field(control,0x58c9d354),value);
    const auto set_action=[&](Value model) {
        const auto input_root=c.path(model,{0x0fb0d794,0xa704272a,0xc52416ef});
        const auto interactable=c.field(input_root,0xa704272a);
        c.set(c.field(interactable,0x4b3146e9),true);c.set(c.field(interactable,0x38f7355f),false);
        clear_value(c,c.path(input_root,{0x66a997b9,0x66a997b9,0x4c09193f}));
        const auto action=c.field(interactable,0xa2b71c93);
        c.set(c.field(action,0x9c76b86c),callback(c,command,argument));c.text(c.field(action,0xb1e14cd6),"");
        const auto primary_input=asset(c,"Configurations/Input/UI/UI_PrimaryAction");
        c.set(c.field(action,0x3d00de27),primary_input);c.set(c.field(action,0x0169c2db),primary_input);
    };
    set_action(control);
    // Native HostParameters items use ContentPresenterTile for controller
    // selection. Keep its authored template and replace its private action.
    for(const auto tile:ui_models(c,row,content_tile_schema.hash)) {
        set_action(tile);
        c.set(c.path(tile,{0x0fb0d794,0xa704272a,0xc52416ef,0x0abf7c31}),Ref{0,control.handle});
    }
    } catch(...) {
        if(fresh) {
            rows.extra.erase(key);
            state().owned.rollback(checkpoint,[&](Value model){c.destroy(model);});
        }
        throw;
    }
}
// A setup row picking one of several named options with the game's own left/right selector
// (TD_Timer_BasicSelector, as Allow Join and Invite look). Returns the index the player chose
// when it differs from `selected`, else -1.
int native_setup_choice_row(const Context& c,NativeSetupRows& rows,const std::string& key,const std::string& caption,
        const std::vector<std::string>& options,int selected) {
    const bool fresh=!rows.extra.contains(key);
    const auto checkpoint=state().owned.checkpoint();
    try {
    const auto row=native_setup_extra(c,rows,key,caption);
    const auto setting=ui_models(c,row,setting_schema.hash).front();
    plain_label(c,c.field(setting,0x58c9d354),caption);
    auto& choice=rows.choices[key];
    if(fresh) {
        const auto control=record_model(c,"TD_HostParameters_ContentResource/TD_Timer_BasicSelector",selector_schema);
        // The authored selector's two-way bindings belong to Skate Jam: own the options and only
        // read this selector's index.
        for(const auto field:{0x4256fd65U,0x85b8c3e6U,0xe88c0f54U})clear_value(c,c.field(control,field));
        c.set(c.field(control,0xeb92bf1c),Ref{});
        for(const auto field:{0x41b10c27U,0xd637e1f4U})c.set(c.field(control,field),Address{});
        const auto list=c.field(control,0x760fc12b);c.set(c.field(list,0x340cfb95),Address{});
        choice.options.clear();
        for(const auto& option:options) {
            auto item=make(c,selector_item_schema),text=make(c,label_schema);
            c.copy(text,c.address(c.field(setting,0x58c9d354)));
            label_styles(c,text,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_Black");
            plain_label(c,text,option);
            c.text(c.field(item,0x58c9d354),option);
            c.set(c.field(item,0x34c8bcc2),Ref{0,text.handle});choice.options.push_back(item.handle);
        }
        c.array(c.field(list,rows_field),std::vector<Ref>{});
        c.array(c.field(list,0xf2c90867),choice.options);
        c.set(c.field(control,0x369babfe),true);
        c.set(c.field(setting,content),Widget{asset(c,"UI/Foundations/Components/Settings/Widgets/BasicSelector_Widget"),{0,control.handle}});
        choice.selector=control;choice.shown=-1;
    }
    const auto list=c.field(choice.selector,0x760fc12b);
    // The selector's choice is the handle of its current item (0x783c4b49); its index field
    // (0x55511280) does not follow left/right.
    const auto current=read<Handle>(c.address(c.field(list,0x783c4b49)));
    const auto found=std::find(choice.options.begin(),choice.options.end(),current);
    const int index=found==choice.options.end()?-1:static_cast<int>(found-choice.options.begin());
    if(choice.shown<0 || (selected!=choice.shown && index==choice.shown)) {
        // First shown, or changed elsewhere (a console command): show the setting's choice.
        const auto at=std::clamp(selected,0,static_cast<int>(choice.options.size())-1);
        c.set(c.field(list,0x55511280),at);c.set(c.field(list,0x783c4b49),choice.options[static_cast<std::size_t>(at)]);
        choice.shown=at;return -1;
    }
    // A choice counts once it has stood for a quarter of a second while the page is open. skate.
    // resets the selector to its first item as the page closes on Confirm, and the page can stay up
    // a moment after: the first item counts only after standing 2 s with the page open (it closes
    // within a second of Confirm), so the reset never does.
    const auto now=GetTickCount64();
    if(index<0 || static_cast<std::size_t>(index)>=choice.options.size() || index==choice.shown) {choice.pending=-1;return -1;}
    if(index!=choice.pending) {choice.pending=index;choice.pending_at=now;return -1;}
    if(now-choice.pending_at<(index==0?2000u:250u) || !flag_plumbing::setup_page())return -1; // only while the page is open
    choice.shown=index;choice.pending=-1;
    return index!=selected?index:-1;
    } catch(...) {
        if(fresh) {
            rows.extra.erase(key);rows.choices.erase(key);
            state().owned.rollback(checkpoint,[&](Value model){c.destroy(model);});
        }
        throw;
    }
}
// A row that only shows text: the setting's caption and a label where its control would be.
void native_setup_info_row(const Context& c,NativeSetupRows& rows,const std::string& key,
        const std::string& caption,const std::string& text) {
    const bool fresh=!rows.extra.contains(key);
    const auto checkpoint=state().owned.checkpoint();
    try {
        const auto row=native_setup_extra(c,rows,key,caption);
        const auto setting=ui_models(c,row,setting_schema.hash).front();
        plain_label(c,c.field(setting,0x58c9d354),caption);
        const auto slot=c.field(setting,content);
        Value label;
        if(fresh) {
            label=make(c,label_schema);
            c.copy(label,c.address(c.field(setting,0x58c9d354)));
            label_styles(c,label,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_Black");
            c.set(c.field(label,0x3d8639d0),2);
            c.set(slot,Widget{asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,label.handle}});
            // No action: the tile only keeps controller focus moving through the list.
            for(const auto tile:ui_models(c,row,content_tile_schema.hash)) {
                const auto action=c.path(tile,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
                clear_value(c,c.field(action,0x9c76b86c));c.text(c.field(action,0xb1e14cd6),"");
            }
        } else label=reference(c,read<Widget>(c.address(slot)).data);
        plain_label(c,label,text);
    } catch(...) {
        if(fresh) {
            rows.extra.erase(key);
            state().owned.rollback(checkpoint,[&](Value model){c.destroy(model);});
        }
        throw;
    }
}
// The setup document's own rows after the stock ones it keeps: sliders cloned from the stock
// Timer row, choices with the game's left/right selector, buttons and text rows.
void native_setup_capabilities(const Context& c,Value panel,Value body_value,const kit::Setup& setup) {
    auto& s=state();auto [found,fresh]=s.official_setup_rows.try_emplace(panel.handle);
    if(fresh) {
        const auto checkpoint=s.owned.checkpoint();
        try {found->second=native_setup_rows(c,body_value);}
        catch(...) {
            s.official_setup_rows.erase(found);
            s.owned.rollback(checkpoint,[&](Value model){c.destroy(model);});
            throw;
        }
    }
    auto& rows=found->second;std::vector<Value> extra;
    for(const auto& row:setup.rows) {
        if(row.id.empty() || extra.size()>=12)continue;
        if(row.kind=="slider") {
            const auto value=native_setup_extra(c,rows,row.id,row.caption);
            for(const auto setting:ui_models(c,value,setting_schema.hash)) {
                const auto presentation=c.field(setting,content);
                if(!s.official_sliders.contains(presentation.handle))
                    s.official_sliders.emplace(presentation.handle,SetupSlider{{},row.id});
            }
            extra.push_back(value);
        } else if(row.kind=="choice" && !row.options.empty()) {
            const auto picked=native_setup_choice_row(c,rows,row.id,row.caption,row.options,row.selected);
            if(picked>=0)kit::post(setup.mode,"setup_row",{row.id,std::to_string(picked)});
            extra.push_back(rows.extra.at(row.id));
        } else if(row.kind=="button") {
            native_setup_action_row(c,rows,row.id,row.caption,row.label,"setup-button",setup.mode+"\n"+row.id);
            extra.push_back(rows.extra.at(row.id));
        } else if(row.kind=="info") {
            native_setup_info_row(c,rows,row.id,row.caption,row.label);
            extra.push_back(rows.extra.at(row.id));
        }
    }
    // Without a timer the stock Timer row stays the template for the rows above, unshown.
    const bool hide_timer=!setup.timer.present;
    // Preserve the native list's layout, scroll/focus state and item template;
    // publish its existing row representation, never an alternate details page.
    if(rows.owned_tiles || rows.stack) {
        const auto stride=rows.owned_tiles?content_tile_schema.size:stack_item.size;
        unsigned saved_count{},saved_stride{};
        const auto saved=c.array(rows.retained_rows,128,saved_count,saved_stride);
        require(saved_stride==stride,"Retained native setup row schema differs.");
        std::vector<std::byte> bytes;
        for(unsigned i=0;i<saved_count;++i)if(!hide_timer || i!=rows.timer_index)
            bytes.insert(bytes.end(),saved.begin()+i*stride,saved.begin()+(i+1)*stride);
        for(const auto row:extra) {
            const auto at=bytes.size();bytes.resize(at+stride);
            require(memory::peek_bytes(c.address(row),bytes.data()+at,stride),"Private native setup row is unavailable.");
        }
        const auto live_field=rows.owned_tiles?c.field(rows.list,0x345b75b1):c.field(rows.list,items);
        unsigned count{},live_stride{};
        const auto live=c.array(live_field,128,count,live_stride);
        require(live_stride==stride,"Private native setup row schema differs.");
        const auto offset=rows.owned_tiles?c.member(c.type(content_tile_schema),widget).offset:c.member(c.type(stack_item),content).offset;
        // The native list updates focus/interaction fields in these inline
        // rows. Remount only their Widget identities with the same item count.
        if(const auto next=remount_presenters<Widget>(live,bytes,stride,offset))
            c.array(live_field,*next,static_cast<unsigned>(bytes.size()/stride));
    } else if(!rows.records.empty()) {
        std::vector<Ref> records;
        for(const auto ref:rows.records)if(!hide_timer || ref.handle!=rows.timer_row.handle)records.push_back(ref);
        for(const auto row:extra)records.push_back({0,row.handle});
        unsigned count{},stride{};const auto live=c.array(c.field(rows.list,rows_field),128,count,stride);
        if(live!=std::vector<std::byte>(std::as_bytes(std::span(records)).begin(),std::as_bytes(std::span(records)).end()))c.array(c.field(rows.list,rows_field),records);
    } else {
        std::vector<Handle> handles;
        for(const auto handle:rows.handles)if(!hide_timer || handle!=rows.timer_row.handle)handles.push_back(handle);
        for(const auto row:extra)handles.push_back(row.handle);
        unsigned count{},stride{};const auto live=c.array(c.field(rows.list,0xf2c90867),128,count,stride);
        if(live!=std::vector<std::byte>(std::as_bytes(std::span(handles)).begin(),std::as_bytes(std::span(handles)).end()))c.array(c.field(rows.list,0xf2c90867),handles);
    }
}
// A slider's document row: the setup's players or timer, or one of its slider rows.
std::optional<kit::SetupSlider> setup_slider(const kit::Setup& setup,const std::string& id) {
    if(id=="players")return setup.players.present?std::optional{setup.players}:std::nullopt;
    if(id=="timer")return setup.timer.present?std::optional{setup.timer}:std::nullopt;
    for(const auto& row:setup.rows)if(row.id==id && row.kind=="slider")
        return kit::SetupSlider{true,row.caption,row.min,row.max,row.step,row.value};
    return {};
}
// The stock Spot Battle setup page while the native flag is at it, relabelled from the mode's
// setup document: its title, art and description, its players and timer sliders and its rows.
void official_setup(const Context& c) {
    auto& s=state();
    const auto flag=kit::flag::state();
    if(!flag_plumbing::setup_page() || !flag.owned || flag.mode.empty()) {
        s.setup_open.store(false);
        if(s.official_panels.empty()) {restore_official(c);s.official_restore_at=0;return;}
        // The page is still in its exit transition into flag placement: put the stock page back
        // once that is over, by when a retired page is skipped and a cached one sits idle.
        const auto now=GetTickCount64();
        if(!s.official_restore_at)s.official_restore_at=now+3000;
        if(now>=s.official_restore_at) {restore_official(c);s.official_restore_at=0;}
        return;
    }
    s.official_restore_at=0;
    if(s.official_game!=flag.game || s.official_mode!=flag.mode) {
        restore_official(c);s.official_game=flag.game;s.official_mode=flag.mode;
    }
    const auto* definition=throwdowns::registry().find(flag.mode);
    // The page shows the registration's own defaults until the plugin sends its setup document.
    auto setup=current_setup(flag.mode).value_or(kit::Setup{});
    // Players default to the registration's range; a document without a timer hides the stock one.
    if(setup.mode.empty()) {
        setup.mode=flag.mode;
        setup.timer={true,"Timer",10.f,120.f,10.f,120.f};
    }
    if(!setup.players.present && definition)
        setup.players={true,"Players",float(definition->min_players),float(definition->max_players),1.f,float(definition->players)};
    const auto title=!setup.title.empty()?setup.title:definition?definition->name:std::string{};
    const auto art=!setup.art.empty()?setup.art:definition?definition->logo:std::string{};
    const auto description=!setup.description.empty()?setup.description:definition?definition->description:std::string{};
    s.setup_open.store(true);
    for(const auto& root:c.roots({panel_schema.hash})) {
      try {
        const auto header_slot=c.field(root.model,0xc0f1b449);
        auto header=reference(c,read<Widget>(c.address(header_slot)).data);
        if(!header.handle || read<std::uint32_t>(read<Address>(header.type))!=header_schema.hash)continue;
        auto title_field=c.field(header,0x00f3b15e);
        const auto name=c.text(c.field(title_field,text_field));
        if(!s.official_panels.contains(root.model.handle) && name!="Spot Battle" && name!="SPOT BATTLE" && name!="ID_ACTIVITY_SPOTBATTLE_TITLE")continue;
        const bool fresh=!s.official_panels.contains(root.model.handle);
        if(fresh) {
            // Header/footer mount before the native body. Do not retain that
            // temporary empty body and then overwrite its populated controls.
            bool has_players{},has_timer{};
            for(const auto setting:ui_models(c,c.field(root.model,0x43967ece),setting_schema.hash)) {
                const auto role=native_setup_role(c.text(c.path(setting,{0x58c9d354,text_field})));
                has_players=has_players || role==NativeSetupRole::players;
                has_timer=has_timer || role==NativeSetupRole::timer;
            }
            if(!has_players || !has_timer)continue;
            // Finish the private graph before changing any live presenter. A
            // missing native dependency must leave the stock panel intact and
            // must not mark a half-built panel as a successful adaptation.
            PrivateUiGraph graph{c};
            std::vector<std::pair<Value,Value>> copies;
            for(const auto key:{0xc0f1b449U,0x43967eceU,0xbfc64535U}) {
                const auto slot=c.field(root.model,key);
                const auto copy=graph.clone(key==0x43967eceU?Ref{0,slot.handle}:read<Widget>(c.address(slot)).data);
                if(key==0x43967eceU)remove_native_rows(c,copy,setup.hide_native);
                copies.emplace_back(slot,copy);
                if(key==0xc0f1b449U)header=copy;
            }
            for(const auto& [slot,copy]:copies)remember_official(c,slot);
            try {
                for(const auto& [slot,copy]:copies) {
                    if(slot.type==copy.type)mount_setup_body(c,slot,copy);
                    else {
                        auto current=read<Widget>(c.address(slot));current.data={0,copy.handle};c.set(slot,current);
                    }
                    s.official_contents.emplace(slot.handle,copy);
                    s.official_slot_panels.emplace(slot.handle,root.model.handle);
                }
                s.official_headers.emplace(root.model.handle,header);
                s.official_panels.insert(root.model.handle);
            } catch(...) {
                for(const auto& [slot,copy]:copies) {
                    try {
                        const auto saved=s.official_originals.at(slot.handle).second;
                        if(slot.type==copy.type)mount_setup_body(c,slot,saved);
                        else c.copy(slot,c.address(saved));
                    }catch(...) {}
                    s.official_contents.erase(slot.handle);s.official_slot_panels.erase(slot.handle);
                    s.official_originals.erase(slot.handle);
                }
                s.official_headers.erase(root.model.handle);s.official_panels.erase(root.model.handle);
                throw;
            }
            title_field=c.field(header,0x00f3b15e);
        } else if(const auto saved=s.official_headers.find(root.model.handle);saved!=s.official_headers.end() && header.handle!=saved->second.handle) {
            auto mounted=read<Widget>(c.address(header_slot));mounted.data={0,saved->second.handle};
            c.set(header_slot,mounted);header=saved->second;title_field=c.field(header,0x00f3b15e);
        }
        if(fresh) logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: using original native Throwdown setup {:#x} for {}.",root.model.handle,flag.mode);
        // Native page reentry can republish Spot Battle's presenters. Keep our
        // slider models while preserving the live stack's animation and focus.
        const auto body_slot=c.field(root.model,0x43967ece);
        const auto private_body=s.official_contents.at(body_slot.handle);
        mount_setup_body(c,body_slot,private_body);
        for(const auto key:{0xbfc64535U}) {
            const auto slot=c.field(root.model,key);const auto copy=s.official_contents.at(slot.handle);
            auto current=read<Widget>(c.address(slot));
            if(current.data.handle!=copy.handle) {current.data={0,copy.handle};c.set(slot,current);}
        }
        plain_label(c,title_field,title);
        // The art is a RimeTexture, while ActivityDisplay's framed icon expects a
        // RimeImage. Use the native Texture widget for this slot; optional art
        // must never block the title or native controls.
        try {
            const auto blueprint=asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget");
            auto& badge=s.official_badge;
            if(!badge.handle) {
                badge=make(c,texture_schema);
                c.set(c.field(badge,0xf524c836),288.f);c.set(c.field(badge,0x6fbd254e),288.f);
                c.set(c.field(badge,0x81c94d8a),false);c.set(c.field(badge,0xd1997d0c),false);
                c.set(c.field(badge,0x190e039c),false);
            }
            mode_art(c,std::array{c.field(badge,0x6e469d2a)},art);
            const auto icon=c.field(header,content);
            const auto current=read<Widget>(c.address(icon));
            if(current.data.handle!=badge.handle)c.set(icon,Widget{blueprint,{0,badge.handle}});
        } catch(const std::exception& e) {
            if(fresh)logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown optional setup badge: {}",e.what());
        }
        // Republishing the rows rewrites the native list arrays. A page whose document adds no
        // rows (and has none of ours to take away) keeps the stock list untouched, as
        // SmokeyDawg58's code left his Deathrace page: that page can move straight on into flag
        // placement while the game is still tearing it down, and a list rewritten then crashed
        // the game's UI walk (2026-10-20).
        if(!setup.rows.empty() || s.official_setup_rows.contains(root.model.handle)) {
            native_setup_capabilities(c,root.model,private_body,setup);
            mount_setup_body(c,body_slot,private_body);
        }
        const auto settings=ui_models(c,c.field(root.model,0x43967ece),setting_schema.hash);
        const auto notices=ui_models(c,c.field(root.model,0x43967ece),notice_schema.hash);
        const auto buttons=ui_models(c,c.field(root.model,0xbfc64535),button.hash);
        unsigned players=static_cast<unsigned>(std::max(1.f,setup.players.value)),seconds=static_cast<unsigned>(std::max(0.f,setup.timer.value));
        for(const auto setting:settings) {
            const auto label=c.field(setting,0x58c9d354);
            const auto presentation=c.field(setting,content);
            auto known=s.official_sliders.find(presentation.handle);
            if(known==s.official_sliders.end()) {
                // A stock row seen for the first time, still under its stock caption.
                const auto id=native_setup_role_id(native_setup_role(c.text(c.field(label,text_field))));
                if(id!="players" && id!="timer")continue;
                known=s.official_sliders.emplace(presentation.handle,SetupSlider{{},std::string(id)}).first;
            }
            auto& adapted=known->second;
            const auto row=setup_slider(setup,adapted.id);
            if(!row)continue;
            auto slider=reference(c,read<Widget>(c.address(presentation)).data);
            if(!slider.handle || read<std::uint32_t>(read<Address>(slider.type))!=0xcdc749b2)continue;
            const bool first=!adapted.model.handle;
            if(first) {
                // The native widget still handles input, focus and rendering.
                // Its shared Spot Battle setting must not receive our values.
                clear_value(c,c.field(slider,2243478502U));
                clear_value(c,c.field(slider,655725074U));
                adapted.model=slider;
            } else if(slider.handle!=adapted.model.handle) {
                auto mounted=read<Widget>(c.address(presentation));mounted.data={0,adapted.model.handle};c.set(presentation,mounted);
                slider=adapted.model;
            }
            const auto range=c.field(slider,0x170bebf7),value=c.field(slider,0x5a547e87);
            const auto step=c.field(slider,0xb47e6cec);
            const int minimum=static_cast<int>(std::lround(row->min)),maximum=std::max(minimum,static_cast<int>(std::lround(row->max)));
            const int increment=std::max(1,static_cast<int>(std::lround(row->step)));
            if(read<std::array<float,2>>(c.address(range))!=std::array<float,2>{float(minimum),float(maximum)})
                c.set(range,std::array<float,2>{float(minimum),float(maximum)});
            if(read<float>(c.address(step))!=float(increment))c.set(step,float(increment));
            c.set(c.field(slider,264787447U),true); // TrimWholeValues
            c.set(c.field(slider,3516482810U),true); // RoundToNearestWhole
            // First shown, or the plugin changed its value: the slider shows the document's.
            if(first || row->value!=adapted.shown) {
                adapted.shown=row->value;
                c.set(value,native_slider_position(static_cast<int>(std::lround(row->value)),minimum,maximum));
            }
            const auto normalized=read<float>(c.address(value));
            const int selected=native_slider_choice(normalized,minimum,maximum,increment);
            const auto snapped=native_slider_position(selected,minimum,maximum);
            if(!std::isfinite(normalized) || std::abs(normalized-snapped)>.00001f)c.set(value,snapped);
            if(first)adapted.chosen=float(selected);
            else if(float(selected)!=adapted.chosen) {
                adapted.chosen=float(selected);
                kit::post(setup.mode,"setup_row",{adapted.id,std::to_string(selected)});
            }
            if(!row->caption.empty())plain_label(c,label,row->caption);
            if(adapted.id=="players")players=static_cast<unsigned>(std::max(1,selected));
            else if(adapted.id=="timer")seconds=static_cast<unsigned>(std::max(0,selected));
        }
        // The native Confirm reads the registration's limits.
        flag_plumbing::set_options(seconds,players);
        for(const auto notice:notices)plain_label(c,c.field(notice,0x94703efc),description);
        // Keep the native Confirm input and its location initializer. The
        // placement observer hands off to the flag as soon as it completes.
        for(const auto b:buttons) {
            const auto action=c.path(b,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
            const auto nav=c.text(c.field(action,0xb1e14cd6));
            if(nav=="ThrowdownPlacement" || nav=="Throwdown_SKATE_Start")continue;
            if(nav!="AdvancedParameters")continue;
            c.text(c.field(action,0xb1e14cd6),"ThrowdownerExit");
            plain_label(c,c.field(b,0x58c9d354),setup.back.empty()?std::string("BACK"):setup.back);
        }
      } catch(const std::exception& e) {
          static std::uint64_t report{};const auto now=GetTickCount64();
          if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown setup candidate {:#x}: {}",root.model.handle,e.what());}
      }
    }
}
void stock_solo_controls(const Context& c) {
    auto& s=state();
    // A plugin mode's flag is out: any Spot Battle page up is ours, closing or about to be put back.
    if(kit::flag::state().owned || s.official_restore_at)return;
    for(const auto& root:c.roots({panel_schema.hash})) {
        if(s.official_panels.contains(root.model.handle))continue;
        const auto header=reference(c,read<Widget>(c.address(c.field(root.model,0xc0f1b449))).data);
        if(!header.handle || read<std::uint32_t>(read<Address>(header.type))!=header_schema.hash)continue;
        const auto title=c.text(c.path(header,{0x00f3b15e,text_field}));
        const bool stock=title=="S.K.A.T.E." || title=="ID_ACTIVITY_SKATE_TITLE" ||
            title=="Spot Battle" || title=="SPOT BATTLE" || title=="ID_ACTIVITY_SPOTBATTLE_TITLE" ||
            title=="Skate Jam" || title=="SKATE JAM" || title=="ID_ACTIVITY_JAMSESSION_TITLE";
        if(!stock)continue;
        for(const auto setting:ui_models(c,c.field(root.model,0x43967ece),setting_schema.hash)) {
            if(c.text(c.path(setting,{0x58c9d354,text_field}))!="ID_SETTINGS_PLAYERS")continue;
            const auto slot=c.field(setting,content);
            auto slider=reference(c,read<Widget>(c.address(slot)).data);
            if(!slider.handle || read<std::uint32_t>(read<Address>(slider.type))!=0xcdc749b2)continue;
            if(const auto existing=s.solo_sliders.find(slot.handle);existing!=s.solo_sliders.end() && existing->second.handle==slider.handle)continue;
            const auto old_range=read<std::array<float,2>>(c.address(c.field(slider,0x170bebf7)));
            if(!std::isfinite(old_range[0]) || !std::isfinite(old_range[1]) || old_range[0]<1 || old_range[1]<2 || old_range[1]>32)continue;
            const auto maximum=static_cast<unsigned>(old_range[1]);
            const auto selected=native_slider_choice(read<float>(c.address(c.field(slider,0x5a547e87))),static_cast<unsigned>(old_range[0]),maximum,1);
            // This change is intentionally shared by all three stock modes.
            // Keep the original model identity so its authored backend binding
            // reads the same slider the native widget changes.
            c.set(c.field(slider,0x170bebf7),std::array<float,2>{1.f,old_range[1]});
            c.set(c.field(slider,0xb47e6cec),1.f);
            c.set(c.field(slider,264787447U),true);c.set(c.field(slider,3516482810U),true);
            c.set(c.field(slider,0x5a547e87),native_slider_position(selected,1,maximum));
            s.solo_sliders[slot.handle]=slider;
            logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: native {} Players slider supports 1-{}.",title,maximum);
        }
    }
}
void retire_solo_pending_queue(const Context& c) {
    if(!native_solo_throwdown_menu_cleanup_pending())return;
    unsigned found{};
    // Live capture of the locked wheel after confirmed Spot Battle solo Quit:
    // dependencies were entitlement=1, IsInQueueState=false,
    // IsPendingQueue=true, ThrowdownerDisabledByRule_RefCount=0.
    // Publish the actual dependency, rather than bypassing IsThrowdownerDisabled
    // or erasing unrelated activity/entitlement restrictions.
    for(const auto& root:c.roots({0x6ee228a8})) {
        if(c.text(c.path(root.model,{0x58c9d354,text_field}))!="ID_THROWDOWNS")continue;
        const auto dependencies=c.path(root.model,{0x67212c85,0x4c09193f,0x8a56e2ac});
        unsigned count{},stride{};c.array(dependencies,8,count,stride);
        if(count!=4 || stride!=sizeof(Ref))continue;
        const auto ref=read<Ref>(c.address(c.element(dependencies,2)));
        if(!ref.record || string(read<Address>(ref.record+0x28),128)!="TD_Page_ContentResources/IsPendingQueue")continue;
        const auto pending=reference(c,ref);
        if(!pending.handle || kind(pending.type)!=10 || size(pending.type)!=1)continue;
        const bool was_pending=read<bool>(c.address(pending));
        if(was_pending)c.set(pending,false);
        ++found;
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: confirmed solo exit published IsPendingQueue {} -> false on the native wheel (model {:#x}).",
            was_pending,pending.handle);
    }
    if(found)native_solo_throwdown_menu_cleaned();
}
void footer_action(const Context& c,Value button_value,const std::string& text,std::string command,std::string argument={}) {
    plain_label(c,c.field(button_value,0x58c9d354),text);
    auto action=c.path(button_value,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
    c.set(c.field(action,0x9c76b86c),callback(c,std::move(command),std::move(argument)));
    c.text(c.field(action,0xb1e14cd6),"");
}
void session_marker_controls(const Context& c,bool release) {
    auto& originals=state().marker_originals;
    if(release || !kit::match().markers) {
        for(const auto& [handle,saved]:originals)
            if(c.type_of(handle)==saved.first.type) c.set(saved.first,saved.second);
        originals.clear();
        return;
    }
    // SessionMarkerDataModel is the game's keyboard/menu binding. The
    // skater-intent predicates separately guard controller marker actions.
    for(const auto& root:c.roots({4200505362U}))
        for(const auto [hash,disabled]:std::array{
                std::pair{2627953652U,true},std::pair{753092917U,false},std::pair{1976179319U,false}}) {
            const auto field=c.field(root.model,hash);
            require(size(field.type)==sizeof(bool),"Native session marker field ABI differs.");
            const auto current=read<bool>(c.address(field));
            originals.try_emplace(field.handle,std::pair{field,current});
            if(current!=disabled)c.set(field,disabled);
        }
}
void waiting_hud(const Context& c) {
    auto& s=state();
    const auto stock_revision=native_throwdown_queue_revision();
    if(s.waiting_stock_revision!=stock_revision) {
        // Native queues can reuse the same notification model and HUD slot.
        // A real creation/join, rather than a timeout, releases suppression.
        for(const auto& [handle,presentation]:s.waiting_presentations) {
            const auto at=c.type_of(handle)==presentation.first.type?
                game::native_data().models.value(c.manager,handle,0,1):0;
            if(!at)continue;
            const auto current=read<Widget>(at).data.handle;
            const auto original=read<Widget>(c.address(presentation.second));
            const auto copy=s.waiting_originals.find(original.data.handle);
            if(copy!=s.waiting_originals.end() && owns_waiting_presentation(current,copy->second.handle,s.waiting_hidden_slots.contains(handle)))
                c.copy(presentation.first,c.address(presentation.second));
        }
        s.waiting_retired_notifications.clear();s.waiting_widgets.clear();s.waiting_originals.clear();
        s.waiting_presentations.clear();s.waiting_counts.clear();s.waiting_hidden_slots.clear();
        s.waiting_stock_revision=stock_revision;
    }
    const auto stock=native_queue_hud_state();
    if(s.stock_waiting_generation!=stock.generation) {
        for(const auto& [handle,slot]:s.stock_waiting_slots) {
            if(c.type_of(handle)!=slot.slot.type)continue;
            const auto at=game::native_data().models.value(c.manager,handle,0,1);
            if(at && !read<Widget>(at).data.handle && !read<Widget>(at).blueprint)
                c.copy(slot.slot,c.address(slot.saved));
        }
        s.stock_waiting_slots.clear();s.stock_waiting_generation=stock.generation;
    }
    // The native flag's registration and its mode's waiting document.
    const auto flag=kit::flag::state();
    const auto* definition=flag.mode.empty()?nullptr:throwdowns::registry().find(flag.mode);
    std::optional<kit::Waiting> waiting;
    if(!flag.mode.empty())if(const auto docs=kit::documents_for(flag.mode);docs && docs->waiting &&
        (docs->waiting->mode.empty() || full_mode(docs->plugin,docs->waiting->mode)==flag.mode))waiting=docs->waiting;
    const bool registration_owned=flag.owned;
    const bool retiring=flag_plumbing::retiring();
    // The card shows only while the flag's plugin publishes a waiting document: it leaves it out
    // while its lobby is elsewhere (an editor open over the placed flag), and the card hides.
    const bool lobby=!retiring && flag.waiting && waiting.has_value();
    // Keep retired custom slots hidden until the game removes/replaces them.
    // A backend teardown can lag the custom match by several UI ticks.
    // The game's queue creates and places this SecondaryNotification itself.
    // Keep its layout, transitions and input prompts; only supply the document's data.
    // The live HUD is in the gameplay asset domain, which need not be a parent
    // of the card's domain. Resolve its registered type directly.
    constexpr std::uint32_t hash=273922540U; // NotificationViewModel
    const std::string name=waiting && !waiting->title.empty()?waiting->title:definition?definition->name:std::string{};
    const std::string message=waiting?waiting->status:std::string{};
    const std::string art=waiting && !waiting->art.empty()?waiting->art:definition?definition->logo:std::string{};
    const unsigned enrolled=waiting?std::max(1u,waiting->enrolled):1u;
    const unsigned capacity=waiting && waiting->capacity?waiting->capacity:definition?static_cast<unsigned>(definition->max_players):6u;
    const bool can_start=lobby && waiting && waiting->can_start;
    const bool ticking=lobby && waiting && waiting->countdown;
    // Authored Notification defaults and other challenge queues are registered
    // roots too. Only adapt a notification actually mounted in the HUD stack.
    std::map<Handle,std::vector<Value>> mounted_notifications;
    for(const auto& root:c.roots({0xa842b4d0})) {
        const auto notifications=c.path(root.model,{0xd9f424d1,items});
        unsigned n{},stride{};c.array(notifications,128,n,stride);
        for(unsigned i=0;i<n;++i) {
            const auto presentation=c.field(c.element(notifications,i),content);
            const auto mounted=mounted_notification(presentation.handle,[&](Handle handle) {
                const Value slot{handle,c.type_of(handle)};
                const auto saved=s.waiting_presentations.find(handle);
                auto current=read<Widget>(c.address(slot));
                if(saved!=s.waiting_presentations.end()) {
                    const auto original=read<Widget>(c.address(saved->second.second));
                    const auto copy=s.waiting_originals.find(original.data.handle);
                    if(copy!=s.waiting_originals.end() && owns_waiting_presentation(
                            current.data.handle,copy->second.handle,s.waiting_hidden_slots.contains(handle)))
                        current=original;
                    else {
                        // Native remount: do not suppress a newly selected stock
                        // Spot Battle merely because it reused this HUD slot.
                        // Several mounted presenters can share this source.
                        // Retire this slot only; the other slots still use the
                        // private notification and participant model.
                        s.waiting_hidden_slots.erase(handle);
                        s.waiting_presentations.erase(saved);
                    }
                }
                if(const auto stock_slot=s.stock_waiting_slots.find(handle);stock_slot!=s.stock_waiting_slots.end()) {
                    const auto original=read<Widget>(c.address(stock_slot->second.saved));
                    if(!current.data.handle && !current.blueprint)current=original;
                    else if(current.data.handle!=original.data.handle || current.blueprint!=original.blueprint)
                        s.stock_waiting_slots.erase(stock_slot); // Native remount owns this slot now.
                }
                current.data.handle=waiting_notification_source(current.data.handle,s.waiting_originals);
                const auto type=current.data.handle?c.type_of(current.data.handle):0;
                if(!type)return NotificationMountNode{};
                const auto hash=read<std::uint32_t>(read<Address>(type));
                const auto inner=hash==0xffb83e48?c.field({current.data.handle,type},content).handle:0;
                return NotificationMountNode{current.data.handle,hash,inner};
            });
            if(mounted) {
                auto& slots=mounted_notifications[mounted->model];
                if(std::none_of(slots.begin(),slots.end(),[&](const auto& slot){return slot.handle==mounted->presentation;}))
                    slots.push_back({mounted->presentation,c.type_of(mounted->presentation)});
            }
        }
    }
    // Retire only exact mounted stock queue notices, preserving unrelated
    // notifications and every live waiting queue. The server/relay events own
    // this lifetime; no timer or "still registered" heuristic keeps it alive.
    if(stock.generation)for(const auto& [handle,presentations]:mounted_notifications) {
        const Value source{handle,c.type_of(handle)};
        if(!source.type || s.waiting_widgets.contains(handle))continue;
        const auto title=c.text(c.path(source,{0x1c1d36ea,0x44688629,text_field}));
        const bool queue_title=title=="Skate Jam" || title=="SKATE JAM" || title=="ID_ACTIVITY_JAMSESSION_TITLE" ||
            title=="Spot Battle" || title=="SPOT BATTLE" || title=="ID_ACTIVITY_SPOTBATTLE_TITLE" ||
            title=="S.K.A.T.E." || title=="ID_ACTIVITY_THROWDOWNSKATE_TITLE";
        if(!queue_title || c.text(c.path(source,{0xc13bcfcd,0x58c9d354,text_field}))!="ID_LEAVE")continue;
        for(const auto slot:presentations) {
            auto found=s.stock_waiting_slots.find(slot.handle);
            if(found==s.stock_waiting_slots.end()) {
                const auto saved=make(c,presenter,slot.type);c.copy(saved,c.address(slot));
                found=s.stock_waiting_slots.emplace(slot.handle,State::StockWaitingSlot{slot,saved,handle}).first;
            }
            const auto current=read<Widget>(c.address(slot));
            const auto original=read<Widget>(c.address(found->second.saved));
            if(!stock.waiting && (current.data.handle==handle || (!current.data.handle && !current.blueprint))) {
                if(current.blueprint || current.data.handle)c.set(slot,Widget{});
            } else if(stock.waiting && !current.data.handle && !current.blueprint)c.set(slot,original);
        }
    }
    const auto candidates=c.roots({hash});
    if(s.waiting_widgets.empty() && GetTickCount64()>=s.waiting_diagnostic) {
        s.waiting_diagnostic=GetTickCount64()+10000;
        logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: looking for native queue HUD type {:#x}, {} root models, {} mounted notifications.",hash,candidates.size(),mounted_notifications.size());
    }
    // The mounted graph is authoritative even when navigation changes the
    // source's root registration. Never depend on a second registry scan to
    // decide whether an already mounted private card needs fresh controls.
    for(const auto& [source_handle,presentations]:mounted_notifications) {
        const Value source{source_handle,c.type_of(source_handle)};
        if(!source.type || read<std::uint32_t>(read<Address>(source.type))!=hash)continue;
        // Save the native presenter before clone_widget_tree publishes its
        // replacement. The map can remount this same source in another slot.
        const auto save_presentations=[&] {
            for(const auto& presentation:presentations)if(!s.waiting_presentations.contains(presentation.handle)) {
                const auto original=make(c,presenter,presentation.type);
                auto native=read<Widget>(c.address(presentation));
                native.data={0,source.handle};
                c.set(original,native);
                s.waiting_presentations.emplace(presentation.handle,std::pair{presentation,original});
            }
        };
        if(!s.waiting_widgets.contains(source.handle)) {
            if(!registration_owned && !(retiring && s.waiting_retired_notifications.contains(source.handle)))continue;
            const auto title=c.text(c.path(source,{0x1c1d36ea,0x44688629,text_field}));
            if(title!="Spot Battle" && title!="SPOT BATTLE" && title!="ID_ACTIVITY_SPOTBATTLE_TITLE")continue;
            if(c.text(c.path(source,{0xd31e2918,0x58c9d354,text_field}))!="ID_START" ||
               c.text(c.path(source,{0xc13bcfcd,0x58c9d354,text_field}))!="ID_LEAVE")continue;
            save_presentations();
            const auto copy=clone_widget_tree(c,presentations.front());
            s.waiting_originals.emplace(source.handle,copy);
            s.waiting_widgets.insert(source.handle);
            logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: private native queue HUD bound, source {:#x}, model {:#x}.",source.handle,copy.handle);
        }
        save_presentations();
        if(retiring) {s.waiting_retired_notifications.insert(source.handle);continue;}
        const auto model=s.waiting_originals.at(source.handle);
        const auto start=c.field(model,0xd31e2918),leave=c.field(model,0xc13bcfcd);
        clear_value(c,c.field(start,0x67212c85));
        c.set_if_changed(c.field(start,0x659db23e),false);
        // The authored notification hides SecondaryButton when its label is
        // empty. Clear its action too: a guest has only Leave, including after
        // this model was previously mounted for a host.
        const auto start_action=c.path(start,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
        const auto start_input=can_start?asset(c,"Configurations/Input/UI/UI_ActivityNotificationSecondaryInteract"):0;
        c.set_if_changed(c.field(start_action,0x3d00de27),start_input);
        c.set_if_changed(c.field(start_action,0x0169c2db),start_input);
        const auto heading=c.field(model,0x1c1d36ea);
        plain_label(c,c.field(heading,0x44688629),name);
        plain_label(c,c.field(heading,0x00f3b15e),message);
        // Start/Leave must remain usable even if optional icon loading is late.
        // Keep the delegate identity stable when this native card is reused
        // for another mode. Its live flag selects the plugin at dispatch;
        // a retired/hidden card or a real Spot Battle fails the owner check.
        footer_action(c,leave,"LEAVE","flag-wait-leave",std::to_string(source.handle));
        if(can_start)footer_action(c,start,"START","flag-wait-start",std::to_string(source.handle));
        else {
            plain_label(c,c.field(start,0x58c9d354),"");
            clear_value(c,c.field(start_action,0x9c76b86c));
            c.text(c.field(start_action,0xb1e14cd6),"");
        }
        if(!s.waiting_badge.handle) {
            s.waiting_badge=make(c,texture_schema);
            c.set(c.field(s.waiting_badge,0xf524c836),96.f);
            c.set(c.field(s.waiting_badge,0x6fbd254e),96.f);
        }
        mode_art(c,std::array{c.field(s.waiting_badge,0x6e469d2a)},art);
        c.set(c.field(heading,content),Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,s.waiting_badge.handle}});
        // The stock queue's decorator displays its backend limit (10). Keep
        // the native green participant icon but bind a private count model to
        // the document's enrolled players and capacity.
        auto& participants=s.waiting_counts[source.handle];
        const auto decorator=c.field(model,0xe01931be);
        if(!participants.handle)participants=clone_widget_model(c,decorator);
        auto participant_widget=read<Widget>(c.address(decorator));
        if(participant_widget.data.handle!=participants.handle) {
            participant_widget.data={0,participants.handle};
            c.set(decorator,participant_widget);
        }
        plain_label(c,c.field(participants,0x58c9d354),std::to_string(enrolled)+"/"+std::to_string(capacity));
        // CountdownViewModel explicitly supports an outside time source when
        // DoCountdown is false. Keep the authored timer artwork/animation and
        // feed the document's countdown, never the native S.K.A.T.E. clock.
        if(!s.waiting_countdown.handle) {
            const auto original=read<Ref>(c.address(c.field(model,0x0750d51d)));
            const auto timer_type=original.handle?c.type_of(original.handle):(original.record?native_record_type(c,original.record):0);
            require(timer_type!=0,"Native waiting timer is not ready.");
            const auto data=original.handle?c.address({original.handle,timer_type}):read<Address>(original.record+0x20);
            s.waiting_countdown=make(c,{1783008579U,static_cast<std::uint16_t>(size(timer_type))},timer_type);
            c.copy(s.waiting_countdown,data);
        }
        const auto timer=s.waiting_countdown;
        c.set(c.field(timer,1883000211U),false); // DoCountdown: externally driven
        c.set(c.field(timer,376517051U),ticking); // ForceActive
        c.set(c.field(timer,1636171836U),ticking); // Running
        c.set(c.field(timer,3239084782U),true); // UpdateText
        native_timer_seconds(c,timer,1288039561U,waiting?waiting->total_ms:0);
        native_timer_seconds(c,timer,247769280U,ticking?waiting->remaining_ms:0);
        c.text(c.field(timer,378491941U),ticking?std::to_string((waiting->remaining_ms+999)/1000):"");
        c.set(c.field(model,0x0750d51d),Ref{0,timer.handle});
    }
    // The registration keeps the world flag alive. Its large waiting card is
    // only needed in the lobby. The native Throwdown Panel owns Quit during
    // play, so hide this presentation without popping its backend queue.
    const bool compact=!lobby;
    for(const auto& [notification,presentations]:mounted_notifications) {
        if(!s.waiting_widgets.contains(notification))continue;
        const auto model=s.waiting_originals.at(notification);
        for(const auto& presentation:presentations) {
            const auto saved=s.waiting_presentations.find(presentation.handle);
            if(saved==s.waiting_presentations.end())continue;
            const auto current=read<Widget>(c.address(presentation));
            auto replacement=read<Widget>(c.address(saved->second.second));
            replacement.data={0,model.handle};
            if(compact) {replacement={};s.waiting_hidden_slots.insert(presentation.handle);}
            else s.waiting_hidden_slots.erase(presentation.handle);
            if(current.blueprint!=replacement.blueprint || current.data.handle!=replacement.data.handle)c.set(presentation,replacement);
        }
    }
}
void mount_hud(const Context& c,Value model,Address blueprint,Value& anchor,Value& item,int id,bool center) {
    auto& s=state();
    const auto roots=c.roots({0xe4c44873}); // HudViewModel, including authored defaults
    // ModelManager's name lookup resolves this alias to the authored default,
    // not the instance mounted by the game. Compare typed storage rather than
    // assuming anything about handle generations or optional Compass/D-Pad.
    const auto defaults=c.type_of(0xe4c44873)?game::native_data().models.value(c.manager,0xe4c44873,0,1):0;
    Handle authored{};
    std::vector<HudRootCandidate> candidates;
    for(const auto& root:roots) {
        if(defaults && root.value==defaults)authored=root.model.handle;
        const auto stack=c.field(root.model,0x44736c13); // FullAspectRatioStack
        const bool active=read<bool>(c.address(c.field(stack,0x369babfe)));
        unsigned populated{};
        // Never let our own appended items make an unmounted template look
        // live. These are independent, game-owned gameplay HUD stacks.
        for(const auto field:{0xa7b6b40fU,0xee58682eU,0x0f3c11c2U}) {
            unsigned count{},stride{};
            c.array(c.path(root.model,{field,items}),64,count,stride);
            populated+=count!=0;
        }
        candidates.push_back({root.model.handle,active,populated});
    }
    const auto selected=mounted_hud_root(candidates,s.hud_root.handle,authored);
    require(selected!=0,"Native gameplay HUD is not mounted.");
    if(selected!=s.hud_root.handle || !s.hud_stack.handle) {
        // Detach from a retired root before rebinding.
        if(s.hud_stack.handle && c.type_of(s.hud_stack.handle)==s.hud_stack.type)publish_hud_surface(c,s.hud_stack,{},false);
        const auto root=std::find_if(roots.begin(),roots.end(),[&](const auto& r){return r.model.handle==selected;});
        s.hud_root=root->model;
        s.hud_stack=c.field(s.hud_root,0x44736c13);
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: attached to live gameplay HUD {:#x}; authored defaults excluded.",selected);
    }
        auto& slot=hud_surface_slot(c,s.hud_stack,id);
        if(anchor.handle==slot.anchor.handle && item.handle==slot.item.handle)return;
        const auto next_anchor=slot.anchor;
        slot.display=Widget{blueprint,{0,model.handle}};
        c.set(c.field(next_anchor,widget),Widget{});
        c.set(c.field(next_anchor,0x1cff7243),std::array<float,2>{center?700.f:1100.f,center?480.f:900.f});
        c.set(c.field(next_anchor,0x144aee01),false);
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            const auto layout=c.field(next_anchor,axis);
            const float at=center?(vertical?.38f:.5f):(vertical?native_hud_screen_anchor(.008f):native_hud_screen_anchor(.006f));
            const float offset=!center && !vertical?16.f:0.f;
            c.set(c.field(layout,0x99bbed5a),at);c.set(c.field(layout,0x49943bed),at);
            c.set(c.field(layout,0x40bfbff4),offset);c.set(c.field(layout,0x1d233266),offset);
            c.set(c.field(layout,0xfec5e0b1),center?.5f:0.f);c.set(c.field(layout,0xd01f4a21),1.f);
        }
        const auto next_item=slot.item;
        anchor=next_anchor;item=next_item;
}
#include "throwdown_match_panel.inl"
// Start/Leave on the native waiting card, checked against the card the press came from: only the
// registration's own visible card, in its lobby, speaks for it.
void waiting_card_press(const Context& c,const Action& action) {
    auto& s=state();
    const auto flag=kit::flag::state();
    const bool owned=flag.waiting;
    const bool retiring=flag_plumbing::retiring();
    // As waiting_hud: a card without its plugin's waiting document is hidden and speaks for nothing.
    const auto docs=flag.mode.empty()?std::optional<kit::Documents>{}:kit::documents_for(flag.mode);
    const bool lobby=!retiring && flag.waiting && docs && docs->waiting;
    bool mounted{};
    for(const auto& [source,copy]:s.waiting_originals) {
        if(action.argument!=std::to_string(source))continue;
        for(const auto& [handle,presentation]:s.waiting_presentations) {
            if(c.type_of(handle)!=presentation.first.type)continue;
            const auto at=game::native_data().models.value(c.manager,handle,0,1);
            if(at && owns_waiting_action(source,source,read<Widget>(at).data.handle,copy.handle,owned,retiring,lobby)) {
                mounted=true;break;
            }
        }
        break;
    }
    if(!mounted) {
        logging::log(logging::Level::debug,logging::Channel::ui,
            "Throwdown waiting control rejected: card {}, waiting {}, retiring {}, mode {}, game {}.",
            action.argument,owned,retiring,flag.mode,flag.game);
        return;
    }
    const bool start=action.command=="flag-wait-start";
    logging::log(logging::Level::info,logging::Channel::ui,
        "Throwdown waiting control: {} owns card {}, mode {}, game {}.",start?"Start":"Leave",action.argument,flag.mode,flag.game);
    // native_flag posts start / leave to the owner's plugin (and Leave ends the registration).
    if(!(start?native_flag::start():native_flag::leave()))
        logging::log(logging::Level::debug,logging::Channel::ui,"Throwdown waiting control: no waiting registration.");
}
// The controls that belong to a match rather than a picker generation: the waiting card's Start
// and Leave, the setup page's buttons and the Throwdown Panel's Quit and Open.
void match_control(const Context& c,const Action& action,std::uint64_t now) {
    auto& s=state();
    if(waiting_card_action(action.command)) {waiting_card_press(c,action);return;}
    if(action.command=="setup-button") {
        // "<mode>\n<row id>": a button row of that mode's setup page, while it is open.
        const auto split=action.argument.find('\n');
        if(split==std::string::npos)return;
        const auto mode=action.argument.substr(0,split);
        if(flag_plumbing::setup_page() && kit::flag::state().mode==mode)kit::post(mode,"setup_button",{action.argument.substr(split+1)});
        return;
    }
    const auto view=match_panel_view();
    if(!view.available || action.argument!=view.identity)return;
    if(action.command=="panel-quit") {
        // native_flag posts quit to the running match's plugin.
        s.match_panel.quitting=true;
        native_flag::quit_match();
        close_match_panel(c,now);
    } else if(action.command=="panel-open") {
        s.match_panel.requested_until=now+4000;
        s.match_panel.request_after=now;
    }
}
#include "throwdown_modes_ui.inl"
#include "throwdown_kit_hud.inl"
void gameplay_hud(const Context& c,std::uint64_t now) {
    auto& s=state();
    try {custom_join_details(c);}catch(const std::exception&) { /* Retry only this native card, preserving its controls. */ }
    try {throwdown_map_identity(c);}
    catch(const std::exception& e) {
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Custom Throwdown map identity: {}",e.what());}
    }
    try {custom_flag_interaction(c);}
    catch(const std::exception&) { /* An optional map card must not suppress the focused flag banner. */ }
    if(s.in_menu || s.match_panel_open.load()) {
        suspend_hud_surface_content(c);
        s.hud_publication.allow(s.hud_stack.handle,s.kit_hud.generation,false,now);
        s.kit_hud.publication.allow(s.kit_hud.stack.handle,s.kit_hud.generation,false,now);
        try {native_match_panel(c,now);}
        catch(const std::exception& e) {
            if(s.match_panel.requested_until && now>=s.match_panel.requested_until) {
                try {restore_match_panel(c);}catch(...) {}
                s.match_panel.requested_until=0;s.match_panel.request_after=0;
            }
            static std::uint64_t report{};
            if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Native challenge menu transition: {}",e.what());}
        }
        return;
    }
    try {waiting_hud(c);}
    catch(const std::exception& e) {
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown native waiting HUD: {}",e.what());}
    }
    try {kit_hud(c);}
    catch(const std::exception& e) {
        // A transition can temporarily hide the native parent or delay an
        // optional asset. Keep the last complete frame while the match
        // still owns it; tearing it down here produces a double flash.
        if(!kit::active())try {kit_hud_publish(c,{});}catch(...) {}
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Throwdown HUD: {}",e.what());}
    }
    try {native_match_panel(c,now);}
    catch(const std::exception& e) {
        try {restore_match_panel(c);}catch(...) {}
        s.match_panel.retry_after=now+5000;
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Native Throwdown Panel: {}",e.what());}
    }
    // The panel prompt can be created after the HUD published its items.
    // Reconcile now so it is visible on the first gameplay frame.
    publish_prompt(c);
}
#include "throwdown_map_identity.inl"
} // namespace
bool native_throwdown_setup_open() noexcept { return state().setup_open.load(std::memory_order_acquire); }
void observe_native_custom_join_details(std::uintptr_t vm) noexcept {
    try {
        const auto mmid=menu_data::native_join_mmid(vm,[](Address at,void* out,std::size_t count){return memory::peek_bytes(at,out,count);});
        if(mmid)state().join_mmid.store(*mmid,std::memory_order_release);
        // Said when it changes: a card whose MMID cannot be read is never branded.
        static std::atomic<std::uint64_t> last{~0ULL};
        const std::uint64_t value=mmid?*mmid:1ULL<<32;
        if(last.exchange(value)!=value) {
            if(mmid)logging::log(logging::Level::info,logging::Channel::ui,"Join card: the game filled the details of MMID {:#x}.",*mmid);
            else logging::log(logging::Level::info,logging::Channel::ui,"Join card: the game filled a throwdown's details; its MMID could not be read.");
        }
    } catch(...) {}
}
void observe_native_flag_interaction(std::uintptr_t vm) noexcept {
    try {
        if(const auto interaction=menu_data::native_flag_interaction(vm,[](Address at,void* out,std::size_t n){return memory::peek_bytes(at,out,n);})) {
            auto& s=state();std::lock_guard interaction_lock(s.interaction_mutex);s.interaction=*interaction;
        }
    } catch(...) {}
}
bool native_throwdown_hud_ready() noexcept { return state().hud_ready.load(std::memory_order_acquire); }
bool native_throwdown_match_panel_open() noexcept { return state().match_panel_open.load(std::memory_order_acquire); }
bool native_throwdown_gameplay_input_allowed() noexcept {
    auto& s=state();return s.gameplay_input_allowed.load(std::memory_order_acquire) && !s.match_panel_open.load(std::memory_order_acquire);
}
bool native_throwdown_match_active() noexcept { try {return match_panel_view().available;}catch(...) {return false;} }
void prepare_native_throwdown_match_panel() noexcept {
    // Native ModelWriteLock is recursive for the same thread (1933460);
    // completion can therefore also run inside an SDK model publication.
    static thread_local bool preparing{};
    if(preparing)return;
    auto& s=state();const auto now=GetTickCount64();
    if(!s.manager || !s.base || !s.match_panel.page.handle || !pending_match_panel_request(Context{s.base,s.manager},now))return;
    preparing=true;
    struct Reset {bool& value;~Reset(){value=false;}} reset{preparing};
    try {
        const auto view=match_panel_view();
        if(!view.available || view.identity!=s.match_panel.match_key)return;
        const auto ui=read<Address>(s.base+build::engine::ui_manager);
        if(!ui || read<Address>(ui+0x140)!=s.manager)return;
        const Context c{s.base,s.manager};game::ModelWriteLock lock{s.manager};
        prepare_match_panel_body(c,now);
    } catch(const std::exception& e) {
        logging::log(logging::Level::warning,logging::Channel::ui,
            "Throwdowns: native Details presentation preparation deferred: {}",e.what());
    }
}
bool release_native_throwdown_menu(std::uintptr_t base) noexcept {
    overlay::capture_challenge_menu_input(false);
    sync_custom_activity_ui_presence(false);
    auto& s=state(); if(s.owned.empty()) { s.owner.store(0); s.setup_open.store(false); return true; }
    try { const auto ui=read<Address>(base+build::engine::ui_manager); const auto manager=ui?read<Address>(ui+0x140):0;
        require(manager!=0,"Throwdowns UI manager unavailable during cleanup.");
        if(manager!=s.manager) { forget_manager(); return true; }
        const Context c(base,manager); game::ModelWriteLock lock(manager); release(c); s.generations.clear(); return true;
    } catch(const std::exception& e) { logging::log(logging::Level::warning,logging::Channel::ui,"Throwdowns native menu cleanup: {}",e.what()); return false; }
}
// A document prompt's input pressed while its prompt shows: the plugin hears panel_command.
void sample_panel_prompts(std::uint16_t buttons,bool allowed) {
    auto& s=state();
    const auto before=std::exchange(s.panel_buttons,buttons);
    if(!allowed || s.in_menu || s.match_panel_open.load() || !s.match_panel.prompt_visible)return;
    for(const auto& binding:s.panel_bindings)
        if((buttons&binding.mask) && !(before&binding.mask))kit::post_to(s.panel_plugin,"panel_command",s.panel_mode,{binding.command});
}
void tick_native_throwdown_menu(std::uintptr_t base,bool loading,bool in_menu) noexcept {
    state().gameplay_input_allowed.store(!loading && !in_menu,std::memory_order_release);
    auto& s=state();if(s.in_menu!=in_menu)s.next_tick=0;s.in_menu=in_menu;
    const auto now=GetTickCount64();
    try {
    // Poll at the caller's client tick rate, not the 200ms UI refresh rate.
    // This snapshot is normalized XInput for every supported controller and
    // contains only keyboard state from the foreground game window.
    const auto panel_view=match_panel_view();
    // Waiting/placement retains the registration's native controls. Only
    // competing gameplay or an explicit panel transition borrows UI presence.
    sync_custom_activity_ui_presence(!loading && panel_view.available &&
        (kit::match().markers || s.match_panel.requested_until || s.match_panel_open.load()));
    ControllerInput panel_input;
    DingoSDKOverlayReadControllerInput(&panel_input,true);
    // ControllerInput.keys contains bindable gameplay keys, which excludes
    // Escape. PromptKeys explicitly samples Escape from the foreground game.
    const bool escape=(DingoSDKOverlayReadPromptKeys() & 8u)!=0;
    s.panel_key_down=(panel_input.buttons & 0x0010u)!=0 || escape;
    DWORD foreground_process{};
    GetWindowThreadProcessId(GetForegroundWindow(),&foreground_process);
    const bool input_allowed=!loading && panel_view.available && foreground_process==GetCurrentProcessId() && !overlay::interface_open();
    overlay::capture_challenge_menu_input(!loading && panel_view.available,
        !loading && panel_view.available && s.match_panel_open.load(std::memory_order_acquire),s.panel_key_down);
    {
        std::lock_guard input_lock(s.panel_input_mutex);
        if(s.panel_start_input.sample(panel_input.buttons,escape,input_allowed,in_menu,s.match_panel_open.load())) {
            s.panel_start_request=panel_view.identity;
            s.next_tick=0;
        }
        if(!input_allowed)s.panel_start_request.clear();
    }
    sample_panel_prompts(static_cast<std::uint16_t>(panel_input.buttons),input_allowed);
    if(loading)return;
    if(now<s.next_tick && !kit::flag::state().placing) {
        // Retry presentation at frame rate if native model publication lands
        // after its script callback. This never rebuilds/restarts HUD stacks.
        if(s.match_panel.requested_until)prepare_native_throwdown_match_panel();
        // Only ticking clocks, timers and the compass advance between full HUD ticks.
        if(s.hud_doc && kit_hud_ticks(*s.hud_doc)) {
            const auto ui=read<Address>(base+build::engine::ui_manager);
            const auto manager=ui?read<Address>(ui+0x140):0;
            if(manager && manager==s.manager) {
                try {const Context c(base,manager);game::ModelWriteLock lock(manager);kit_hud_motion(c,now);}
                catch(...) { /* A retiring clock must not retire the whole HUD. */ }
            }
        }
        return;
    }
    s.next_tick=now+200;
        const auto ui=read<Address>(base+build::engine::ui_manager); const auto manager=ui?read<Address>(ui+0x140):0; if(!manager)return;
        const Context c(base,manager); game::ModelWriteLock lock(manager);
        if(s.manager && s.manager!=manager)forget_manager();
        s.manager=manager; s.base=base;
        std::erase_if(s.doomed,[&](const auto& entry) {
            if(now<entry.second)return false;
            if(c.type_of(entry.first.handle)==entry.first.type)c.destroy(entry.first);
            return true;
        });
        // The native menu lifetime releases models BEFORE a level unload and
        // rebuilds each new page below. Multiplayer world/session rebinding is
        // independent of level loading and must not erase a mounted picker/HUD.
        retire_solo_pending_queue(c);
        flag_plumbing::tick(c);
        flag_cursor_height(c);
        // A native page can retire before its Quit/Start delegate is pumped.
        // Match controls belong to the match, not the picker generation.
        std::vector<Action> match_controls,picker;
        {std::lock_guard pending_lock(s.mutex);
            for(const auto& action:std::exchange(s.pending,{}))
                (action.command=="mode-open"?picker:match_controls).push_back(action);
        }
        // Native publication can dispatch a button callback. Never hold the
        // callback queue's mutex while publishing navigation or presenters.
        for(const auto& action:match_controls)match_control(c,action,now);
        ++s.pass;
        session_marker_controls(c);
        if(s.page.handle && c.type_of(s.page.handle)!=s.page.type) release(c,true);
        // The game creates new page/list generations when Throwdowns reopens.
        // Find fresh instances rather than leaving the cards on a retired page.
        for(const auto& p:c.roots({page.hash})) if(const auto list=card_list(c,p.model)) {
            if(s.page.handle && (p.model.handle!=s.page.handle || list->handle!=s.cards.handle) && !s.generations.contains({p.model.handle,list->handle})) release(c,true);
            if(!s.page.handle && !s.generations.contains({p.model.handle,list->handle})) { initialize(c,p.model,*list); break; }
        }
        if(!s.page.handle) {
            // The native page is retired when skating resumes. Gameplay HUDs
            // must keep updating without a Throwdowns menu generation.
            if(!s.owner.load())s.owner.store(manager);
            // Native navigation retires the card page before setup mounts.
            // Continue adapting the independent native panel in that interval.
            official_setup(c);
            stock_solo_controls(c);
            gameplay_hud(c,now);
            return;
        }
        // Keep the retained models and delegates through empty native page
        // transitions. Rebuilding/releasing them here disables custom tiles
        // and imposes the exception retry delay on every subsequent reopen.
        if(!mounted_body(c,s.page)) {
            official_setup(c);stock_solo_controls(c);gameplay_hud(c,now);return;
        }
        mode_cards(c);
        logo_picker(c);
        for(const auto& a:picker)if(a.generation==s.owner.load())mode_open(a.argument);
        try { official_setup(c); }
        catch(const std::exception& e) {
            static std::uint64_t setup_retry_log{};
            if(now>=setup_retry_log) {
                setup_retry_log=now+10000;
                logging::log(logging::Level::warning,logging::Channel::ui,"Throwdowns official setup: {}",e.what());
            }
        }
        stock_solo_controls(c);
        gameplay_hud(c,now);
    } catch(const std::exception& e) {
        logging::log(logging::Level::warning,logging::Channel::ui,"Throwdowns native menu: {}",e.what());
        s.next_tick=now+5000; release_native_throwdown_menu(base);
    }
}
} // namespace dingosdk::multiplayer
