#pragma once
#include "native_menu_data.h"
#include "native_menu_lifetime.h"
#include "native_menu_view.h"
#include "native_tools_view.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pause-menu page state shared by the native_menu*.cpp files.
namespace dingosdk::multiplayer::native_menu_detail {
using menu_data::Address, menu_data::Context, menu_data::OwnedMenuModels, menu_data::Ref, menu_data::Schema,
    menu_data::Value;
constexpr std::uint32_t content = 0x716496c8, items = 0x61742cb4,
    tab_data = 0x0791c4f7, label = 0x58c9d354, text_field = 0x4d8e01b9,
    button_field = 0x0fb0d794, value_field = 0x71f0e0b6, key_field = 0x1e95752c,
    our_key = 0x52534d50;
constexpr std::size_t max_actions = 1024;
enum class Section { browser, host, join, session, voice };
// Most sections on any owned page: Multiplayer and Mod Options both have five.
constexpr unsigned section_count = 5;
static_assert(native_tools::sections.size() <= section_count);
constexpr float main_width = 1800.f, side_width = 1080.f;
constexpr Schema horizontal_panel{0xf0b42cc6, 104}, vertical_panel{0x4b60a61d, 96};
constexpr const char* anchored_widget = "UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget";
constexpr const char* horizontal_widget = "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_LinearFocus_Widget";
constexpr const char* vertical_widget = "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_LinearFocus_Widget";
enum class RowKind { button, input, text };
using Action = menu_data::MenuAction;
struct NativeAction {
    // An unbound native FunctionTypeInfo reference. The native delegate copier
    // borrows untagged references, so descriptors live for the process lifetime.
    alignas(8) std::array<std::byte, 0x70> descriptor{};
    Address info{};
    Action action;
};
struct Row { Value model, presenter; RowKind kind{}; std::string last_text; Address callback{}; bool primary{}; Ref dark_text_style; float width{}, height{}; };
struct State {
    unsigned slot{};
    unsigned section_total = section_count;
    unsigned failures{};
    native_tools::State tools;
    overlay::Model tools_model;
    overlay::CallbacksV3 tools_callbacks{};
    std::mutex mutex;
    std::string status = "Native multiplayer menu: waiting for the pause menu.";
    std::deque<Action> pending;
    std::array<NativeAction, max_actions> actions;
    std::atomic<unsigned> action_count{};
    std::atomic<std::uint64_t> generation{};
    std::uint64_t pass{};  // render passes so far; see menu_data::action_slot
    Address base{}, manager{};
    Value core, page, list, menu_item;
    OwnedMenuModels<Value> owned_models;
    std::array<Value, section_count> lists, side_lists, bodies;
    std::array<std::vector<Value>, section_count * 2> fixed_rows;
    std::string selected_player;
    bool copy_code_requested{};
    Value menu_style, primary_style;
    Ref white_text_style;
    float row_width = main_width;
    std::map<std::string, Address, std::less<>> assets;
    std::map<std::string, Row, std::less<>> rows;
    std::array<std::vector<std::string>, section_count * 2> displayed;
    Section section{Section::browser};
    menu_view::BrowserOptions browser;
    std::string protected_lobby;
    unsigned capacity = multiplayer_lobby_player_limit;
    unsigned tps = multiplayer_default_tps;
    // A voice change sent but not yet reflected in the model, so a second
    // press before the model catches up builds on the first.
    std::optional<VoiceSettings> voice_pending;
    std::uint64_t voice_pending_until{};
    bool public_lobby{true}, was_active{}, host_seeded{};
    bool browsing{};  // the lobby browser was on screen at the last render
    std::uint64_t next_id = 1, next_scan{}, next_retry{}, next_update{}, next_render{}, owner{}, feedback_until{};
    std::string feedback, feedback_status;
};
State& page_state(unsigned slot);
State& state();
Address action(const Context& context, std::string command, std::string argument = {});
Address blueprint(std::string_view name);
Value make(const Context& context, Schema schema);
// native_menu_rows.cpp
void add_text(const Context& context, std::vector<std::string>& visible, const std::string& id, const std::string& title, float height = 60.f);
void add_button(const Context& context, std::vector<std::string>& visible, const std::string& id,
                std::string title, std::string command = {}, std::string argument = {}, bool primary = false, float row_height = 0.f);
void add_input(const Context& context, std::vector<std::string>& visible, const std::string& id,
               const std::string& title, const std::string& initial = {});
std::string input_text(const Context& context, const char* id);
void clear_input(const Context& context, const char* id);
bool scrolling_list(unsigned page_slot, unsigned slot);
void publish_rows(const Context& context, Value list, unsigned slot, const std::vector<std::string>& visible);
// native_menu_multiplayer.cpp
void process_clipboard() noexcept;
void process_actions(const Context& context, const MultiplayerModel& model);
void render_section(const Context& context, const MultiplayerModel& model, Section section);
} // namespace dingosdk::multiplayer::native_menu_detail
