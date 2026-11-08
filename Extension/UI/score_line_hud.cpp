#include "score_line_hud.h"
#include "hud_corner.h"
#include "hud_stack.h"
#include "own_ui_writes.h"
#include "Extension/Skater/trick_feed.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/hud_corner.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include "Engine/Game/Build/20260929/reference_lists.h"
#include "Engine/Game/Build/20260929/score_hud.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <span>
#include <utility>
#include <vector>

namespace dingosdk::score_line {
namespace {
namespace corner = addr::hud_corner;
namespace score = addr::score_hud;
namespace menu_data = multiplayer::menu_data;
using Address = std::uintptr_t;
using menu_data::Context;
using menu_data::read;
using menu_data::Ref;
using menu_data::require;
using menu_data::Value;
using menu_data::Widget;

// Keys and ids of ours (not game addresses), apart from the game's keys (0x8000....) and hud_corner's
// cover ("RSHC").
constexpr std::uint32_t score_key = 0x52534853; // "RSHS": our score HUD item in DpadScoringStack
constexpr std::uint32_t row_key = 0x52535200;   // "RSR" + n: a row of ours in our model's trick list
constexpr std::uint64_t model_id = 0x5253485300000000ULL;
constexpr std::string_view model_name = "ReSkate.ScoreLine";
constexpr std::size_t transition_size = corner::item_transition_size; // a stack item's Transition, copied from the d-pad's item
constexpr double look_again_ms = 1000;     // what is not found is looked for at most this often
constexpr double destroy_after_ms = 10000; // our model is kept this long after a line, for the next
// A reading of the game's root (observe) counts for this tick only; plugin_runtime.cpp reads it each
// tick, just before tick, while wanted.
constexpr double fresh_reading_ms = 250;

double now_ms() noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<std::uint64_t> own_root{};

struct Fields {
    Value hud_active, extra_info, sequence_state, action_active, is_slam;
    Value line_score, multiplier, multiplier_text, ring_on, line_on, timer, fill, points, rows;
};
struct Row {
    std::string name;
    Value model, item;
};
// Client thread only.
struct Hud {
    Address manager{};
    // The game's: its root, the d-pad's stack and what our items are made from.
    Value game_root, stack;
    Address score_blueprint{}, row_blueprint{};
    std::array<std::byte, transition_size> transition{};
    std::uint16_t key_at{}, content_at{};
    double next_find{};
    Address row_type{};
    double next_row_look{};
    // A row could not be made in this registry: lines show without their trick names until the next.
    bool rows_off{};
    // Ours.
    Value model, item;
    Fields fields;
    std::vector<Row> rows; // newest first
    LineStart line_start;  // when a line's rows start again from an empty list
    std::string points_text, multiplier_text;
    bool texts_set{};
    ItemPresence presence;
    double out_since{}; // when our item was last found out, 0 while it is in
    std::uint32_t next_row{};
    std::uint64_t next_id{};
    // skate.'s own line, and this tick's reading of its root.
    GameLine game;
    bool hud_read{};
    GameSignals signals;
    std::optional<double> read_at;
    std::string status; // what the last tick found, for the next ask
    bool unsupported{}, warned{};
    unsigned failed_makes{}; // make_model failures in a row (it is tried again until three)
    bool said_made{}, said_in{}, said_out{}, said_yield{}, said_busy{}, said_rows{}, said_rows_off{};
};
Hud& hud() {
    static auto* value = new Hud;
    return *value;
}
LineOwner& owner() {
    static auto* value = new LineOwner;
    return *value;
}
bool alive(const Context& c, const Value& value) { return value.handle && c.type_of(value.handle) == value.type; }
unsigned count_of(const Context& c, const Value& array) {
    const auto at = read<Address>(c.address(array));
    if (!at) return 0;
    require(at >= addr::reference_lists::array_count_before, "a score HUD array is unavailable");
    return read<std::uint32_t>(at - addr::reference_lists::array_count_before) & ~addr::reference_lists::array_count_flag;
}
void destroy_quietly(const Context& c, Value& value) noexcept {
    try {
        c.destroy(value);
    } catch (...) {}
    value = {};
}

// ---- What a new registry or a level load leaves: nothing of ours (it went with the old registry,
// and nothing is destroyed through another), and nothing found of the game's.
void forget(Hud& h, Address manager) noexcept {
    h.manager = manager;
    h.game_root = h.stack = {};
    h.score_blueprint = h.row_blueprint = h.row_type = 0;
    h.next_find = h.next_row_look = 0;
    h.rows_off = false;
    h.model = h.item = {};
    h.fields = {};
    h.rows.clear();
    h.line_start.out();
    h.texts_set = false;
    h.presence.registry_changed();
    h.out_since = 0;
    h.game.reset();
    own_root.store(0, std::memory_order_release);
    tricks::feed::own({});
}

void destroy_ours(Hud& h, const Context& c) noexcept;

// ---- The game's side: the d-pad's stack (kept while it lives, so a take-out always has it), its root
// and the widgets. Looked for at most once a second while any is missing.
void find_game(Hud& h, const Context& c, double now) {
    if (h.stack.handle && !alive(c, h.stack)) {
        h.stack = {};
        h.presence.scanned(false); // our item went with that stack
        // Our item and rows show that HUD's widgets, which the next HUD may not keep: ours are made
        // again with the next one's, never put back as they were.
        destroy_ours(h, c);
        h.score_blueprint = h.row_blueprint = 0;
    }
    if (h.game_root.handle && !alive(c, h.game_root)) h.game_root = {};
    if (h.stack.handle && h.game_root.handle) return;
    if (now < h.next_find) return;
    h.next_find = now + look_again_ms;
    if (!h.stack.handle) {
        const auto stack = hud_stack::find_dpad_stack(c);
        if (!stack.handle) return;
        const auto item_type = c.type(menu_data::stack_item);
        const auto key = c.member(item_type, corner::item_key);
        const auto content = c.member(item_type, corner::item_content);
        const auto transition = c.member(item_type, corner::item_transition);
        if (menu_data::size(key.type) != sizeof(std::uint32_t) || menu_data::size(content.type) != sizeof(Widget) ||
            menu_data::size(transition.type) != transition_size) {
            if (!std::exchange(h.unsupported, true))
                logging::write(logging::Level::warning, logging::Channel::ui, "Score line: skate.'s HUD stack items differ on this game build.");
            return;
        }
        // The d-pad's own item: the HUD's asset domain, and the Transition a push gives an item.
        unsigned count{}, stride{};
        const auto items = c.array(hud_stack::items_of(c, stack), hud_stack::most_items, count, stride);
        require(!count || stride == menu_data::stack_item.size, "the stack's items differ");
        Address dpad{};
        for (unsigned index = 0; index < count && !dpad; ++index) {
            const auto* at = items.data() + std::size_t{index} * stride;
            Widget widget{};
            std::memcpy(&widget, at + content.offset, sizeof widget);
            if (hud_stack::asset_name(widget.blueprint) != corner::dpad_widget) continue;
            dpad = widget.blueprint;
            std::memcpy(h.transition.data(), at + transition.offset, transition_size);
        }
        if (!dpad) return;
        const auto blueprint = [&](std::string_view name) -> Address {
            const auto asset = menu_data::named_asset(c, dpad, std::string(name).c_str());
            if (!asset || read<Address>(asset + addr::reference_lists::asset_type) != c.base + addr::native_menu::widget_blueprint_type ||
                hud_stack::asset_name(asset) != name)
                return 0;
            return asset;
        };
        const auto score_widget = blueprint(score::score_widget);
        if (!score_widget) return;
        h.score_blueprint = score_widget;
        h.row_blueprint = blueprint(score::trick_row_widget);
        h.key_at = key.offset;
        h.content_at = content.offset;
        h.stack = stack;
    }
    if (!h.game_root.handle) {
        const auto roots = c.roots({corner::score_view_schema});
        if (const auto* root = pick_game_root(roots, own_root.load(std::memory_order_acquire))) h.game_root = root->model;
    }
}

// What the d-pad's stack holds now: ours, and the game's own score item (its ScoringHUD_Widget bound
// to any model but ours, so any other score item counts too: it would draw over ours just the same).
// Read as far as a take-out reads, so a stack that grew while ours was in still lets it out.
void scan_stack(const Hud& h, const Context& c, bool& ours, bool& game) {
    unsigned count{}, stride{};
    const auto items = c.array(hud_stack::items_of(c, h.stack), hud_stack::most_items_out, count, stride);
    require(!count || stride == menu_data::stack_item.size, "the stack's items differ");
    for (unsigned index = 0; index < count; ++index) {
        const auto* at = items.data() + std::size_t{index} * stride;
        std::uint32_t key{};
        std::memcpy(&key, at + h.key_at, sizeof key);
        Widget widget{};
        std::memcpy(&widget, at + h.content_at, sizeof widget);
        if (key == score_key) ours = true;
        else if (widget.blueprint && widget.blueprint == h.score_blueprint && widget.data.handle != h.model.handle) game = true;
    }
}

// ---- Models of ours.
// A model of ours of `type`. Whatever create made is destroyed again when a later step fails, also a
// model of another type, so nothing of ours is left in the registry with no handle kept.
Value create(Hud& h, const Context& c, Address type) {
    const auto handle =
        game::native_data().models.create(c.manager, type, model_id + ++h.next_id, game::native_name_hash(model_name), false, 2);
    require(handle != 0, "a model of ours was not made");
    Value value{handle, c.type_of(handle)};
    try {
        require(value.type == type, "a model of ours was made of another type");
        c.address(value);
        c.initialize_text(value);
    } catch (...) {
        if (value.type) destroy_quietly(c, value); // (a handle that names no model is never destroyed)
        throw;
    }
    return value;
}
std::vector<std::byte> bytes_of(const Context& c, const Value& value) {
    std::vector<std::byte> bytes(menu_data::size(value.type));
    require(memory::read_bytes(c.address(value), bytes.data(), bytes.size()), "a model of ours is unavailable");
    return bytes;
}
// A stack item made as the game's own are (P1's dumps): key, priority, the d-pad item's Transition,
// Active, the unknown float 0, and the widget it shows bound to a model of ours.
Value make_item(Hud& h, const Context& c, std::uint32_t key, std::int32_t priority, Widget content) {
    auto item = create(h, c, c.type(menu_data::stack_item));
    try {
        c.set(c.field(item, corner::item_key), key);
        c.set(c.field(item, corner::item_priority), priority);
        const auto transition = c.field(item, corner::item_transition);
        require(menu_data::size(transition.type) == transition_size, "a stack item's Transition differs");
        c.publish(transition, h.transition.data());
        c.set(c.field(item, corner::item_unknown_float), 0.f);
        c.set(c.field(item, corner::item_active), true);
        c.set(c.field(item, corner::item_content), content);
    } catch (...) {
        destroy_quietly(c, item);
        throw;
    }
    return item;
}
Value sized(const Context& c, const Value& model, std::uint32_t hash, std::size_t size) {
    const auto value = c.field(model, hash);
    require(menu_data::size(value.type) == size, "a score HUD field's size differs");
    return value;
}
Value label_text(const Context& c, const Value& model, std::uint32_t label) {
    const auto value = c.path(model, {label, score::label_text});
    require(menu_data::kind(value.type) == 7, "a score HUD label differs");
    return value;
}
void bind(Hud& h, const Context& c) {
    const auto& m = h.model;
    auto& f = h.fields;
    f.hud_active = sized(c, m, corner::hud_widget_active, 1);
    f.extra_info = sized(c, m, corner::extra_info_style, 4);
    f.sequence_state = sized(c, m, corner::sequence_state, 4);
    f.action_active = sized(c, m, score::action_active, 1);
    f.is_slam = sized(c, m, score::is_slam, 1);
    f.line_score = sized(c, m, score::line_score, 4);
    f.multiplier = sized(c, m, score::multiplier, 4);
    f.ring_on = sized(c, m, score::ring_on, 1);
    f.line_on = sized(c, m, score::line_on, 1);
    f.timer = sized(c, m, score::line_timer, 4);
    f.fill = sized(c, m, score::action_fill, 4);
    f.multiplier_text = label_text(c, m, score::multiplier_text);
    f.points = label_text(c, m, score::action_points);
    f.rows = c.path(m, {score::trick_list, corner::stack_items});
    require(menu_data::kind(f.rows.type) == 4, "the score HUD's trick list differs");
}

// Our ScoringHUDViewModel: the game root's type, the authored values copied in (ModelManager's alias of
// the schema, mount_hud's way; there was none in scorep2, so the game's root is copied, only while
// nothing of its own line is in it: step checks root_busy first), and nothing of the game's live line:
// its trick list and second stack emptied, its ScoringData Ref put back to what create gave ours
// ({0, 0}), its banner and tier word "". False when this build's model is not the one mapped.
bool make_model(Hud& h, const Context& c) {
    const auto type = h.game_root.type;
    if (menu_data::size(type) != score::score_view_size) {
        if (!std::exchange(h.unsupported, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Score line: ScoringHUDViewModel is {} bytes on this game build, not {}.",
                menu_data::size(type), score::score_view_size);
        return false;
    }
    h.model = create(h, c, type);
    own_root.store(h.model.handle, std::memory_order_release);
    try {
        const auto data = sized(c, h.model, score::scoring_data, sizeof(Ref));
        const auto made = read<Ref>(c.address(data));
        const auto alias = c.type_of(corner::score_view_schema) == type
            ? game::native_data().models.value(c.manager, corner::score_view_schema, 0, 1) : 0;
        c.copy(h.model, alias ? alias : c.address(h.game_root));
        for (const auto list : {score::trick_list, score::second_stack}) {
            const auto items = c.path(h.model, {list, corner::stack_items});
            if (count_of(c, items)) c.array(items, std::span<const std::byte>{}, 0);
        }
        const auto copied = read<Ref>(c.address(data));
        if (copied.record != made.record || copied.handle != made.handle) c.set(data, made);
        bind(h, c);
        // The HUD up with the LINE label, no banner and no slam look, whatever the copy held.
        c.set_if_changed(h.fields.extra_info, score::extra_info_line_score);
        c.set_if_changed(h.fields.sequence_state, corner::sequence_down);
        c.set_if_changed(h.fields.action_active, false);
        c.set_if_changed(h.fields.is_slam, false);
        // The banner and the red tier word go too: the copy may hold the game's last ("OUCH", from
        // ID_SCORING_SLAM_TIER_1, stayed between actions with SequenceState down), and nothing of
        // ours writes them.
        for (const auto label : {score::sequence_header, score::sequence_descriptor}) c.text(label_text(c, h.model, label), "");
        h.item = make_item(h, c, score_key, corner::score_priority, Widget{h.score_blueprint, {0, h.model.handle}});
    } catch (const std::exception& error) {
        own_root.store(0, std::memory_order_release);
        destroy_quietly(c, h.model);
        h.fields = {};
        // A layout that differs fails every time; a create or a read that fails once (the HUD still
        // loading, say) is tried again on a later tick. Three in a row are taken as the layout.
        if (++h.failed_makes >= 3 && !std::exchange(h.unsupported, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Score line: skate.'s score HUD differs on this game build: {}.", error.what());
        return false;
    }
    h.failed_makes = 0;
    h.texts_set = false;
    if (!std::exchange(h.said_made, true))
        logging::log(logging::Level::info, logging::Channel::ui, "Score line: the SDK's own score HUD model was made (handle 0x{:x}).", h.model.handle);
    return true;
}

void destroy_ours(Hud& h, const Context& c) noexcept {
    own_root.store(0, std::memory_order_release);
    tricks::feed::own({});
    // The model first (its trick list holds the rows' items), then its item and the rows.
    destroy_quietly(c, h.model);
    destroy_quietly(c, h.item);
    for (auto& row : h.rows) {
        destroy_quietly(c, row.item);
        destroy_quietly(c, row.model);
    }
    h.rows.clear();
    h.line_start.out();
    h.fields = {};
    h.texts_set = false;
    h.out_since = 0;
}

// ---- Rows.
// The row type, or 0 for none yet: the registry's, which lists it once the game has made a row model
// of its own, at its first scored action in the level (scorep2: "Native menu schema has not loaded
// yet" for rows before any game row; rows went in after one). Until then a line shows without its
// trick names. (A row type found on the heap before that is not used: no launch has shown that
// models.create takes one, and one left behind by the last level would pass any check of its bytes.)
Address row_type_now(Hud& h, const Context& c, double now) {
    if (h.row_type) return h.row_type;
    if (now < h.next_row_look) return 0;
    h.next_row_look = now + look_again_ms;
    try {
        h.row_type = c.type({score::trick_row_schema, score::trick_row_size});
    } catch (const std::exception&) {} // not loaded yet
    return h.row_type;
}
Row make_row(Hud& h, const Context& c, Address type, const std::string& name) {
    Row row{name, create(h, c, type), {}};
    try {
        // The row's handle back into a score model keeps what create gave it, never the game's.
        c.text(label_text(c, row.model, score::row_name), name);
        row.item = make_item(h, c, row_key + (h.next_row++ & 0xffU), 0, Widget{h.row_blueprint, {0, row.model.handle}});
    } catch (...) {
        destroy_quietly(c, row.model);
        throw;
    }
    return row;
}
// The trick list as `wanted` (newest first): only the rows that are new are made, at the front, as
// the game's list takes a new row, and those that fall off the end are destroyed once the list no
// longer holds them.
void show_rows(Hud& h, const Context& c, const std::vector<std::string>& wanted, double now) {
    std::vector<std::string> shown;
    for (const auto& row : h.rows) shown.push_back(row.name);
    if (shown == wanted) return;
    const auto fresh = fresh_rows(shown, wanted);
    const auto type = fresh ? row_type_now(h, c, now) : h.row_type;
    if (fresh && (!type || !h.row_blueprint)) {
        if (!std::exchange(h.said_rows, true))
            logging::write(logging::Level::info, logging::Channel::ui,
                h.row_blueprint ? "Score line: skate.'s trick list rows are not loaded yet; a line shows without its trick names until they are."
                                : "Score line: skate.'s trick list row widget is not loaded; a line shows without its trick names.");
        return;
    }
    std::vector<Row> next;
    try {
        for (std::size_t index = 0; index < fresh; ++index) next.push_back(make_row(h, c, type, wanted[index]));
    } catch (...) {
        for (auto& row : next) {
            destroy_quietly(c, row.item);
            destroy_quietly(c, row.model);
        }
        throw;
    }
    const auto made = next.size();
    const auto kept = wanted.size() - fresh;
    for (std::size_t index = 0; index < kept; ++index) next.push_back(h.rows[index]);
    std::vector<std::byte> bytes;
    try {
        for (const auto& row : next) {
            const auto item = bytes_of(c, row.item);
            bytes.insert(bytes.end(), item.begin(), item.end());
        }
        c.array(h.fields.rows, bytes, static_cast<unsigned>(next.size()));
    } catch (...) {
        for (std::size_t index = 0; index < made; ++index) {
            destroy_quietly(c, next[index].item);
            destroy_quietly(c, next[index].model);
        }
        throw;
    }
    for (std::size_t index = kept; index < h.rows.size(); ++index) {
        destroy_quietly(c, h.rows[index].item);
        destroy_quietly(c, h.rows[index].model);
    }
    h.rows = std::move(next);
}

// Our trick list emptied, then its rows destroyed. Rows the list still holds are not destroyed (they
// go with the model) when it could not be emptied.
void clear_rows(Hud& h, const Context& c) noexcept {
    if (h.rows.empty()) return;
    try {
        if (count_of(c, h.fields.rows)) c.array(h.fields.rows, std::span<const std::byte>{}, 0);
    } catch (...) {
        return;
    }
    for (auto& row : h.rows) {
        destroy_quietly(c, row.item);
        destroy_quietly(c, row.model);
    }
    h.rows.clear();
}
// The rows are the line's least part: one that cannot be made (none has been made from this build's
// row type outside the scorep2 launch) leaves this registry's lines without their names, and never
// takes the points, ring or LINE down with it.
void show_rows_or_none(Hud& h, const Context& c, const std::vector<std::string>& wanted, double now) {
    if (h.rows_off) return;
    std::string why;
    try {
        show_rows(h, c, wanted, now);
        return;
    } catch (const std::exception& error) {
        why = error.what();
    } catch (...) {
        why = "unknown";
    }
    h.rows_off = true;
    h.row_type = 0;
    clear_rows(h, c);
    if (!std::exchange(h.said_rows_off, true))
        logging::log(logging::Level::warning, logging::Channel::ui,
            "Score line: a trick list row could not be made ({}); lines show without their trick names until the next level.", why);
}

// ---- Our line up: the model's values as the game's own line looks, then our item in.
void publish(Hud& h, const Context& c, const Look& look) {
    const auto& f = h.fields;
    c.set_if_changed(f.line_score, look.line_score);
    c.set_if_changed(f.multiplier, look.multiplier);
    c.set_if_changed(f.timer, look.timer);
    c.set_if_changed(f.fill, look.fill);
    c.set_if_changed(f.ring_on, look.ring_on);
    c.set_if_changed(f.line_on, look.line_on);
    // Texts only when they differ from what we last wrote: each write makes an engine string.
    if (!h.texts_set || look.points != h.points_text) c.text(f.points, look.points);
    h.points_text = look.points;
    if (!h.texts_set || look.multiplier_text != h.multiplier_text) c.text(f.multiplier_text, look.multiplier_text);
    h.multiplier_text = look.multiplier_text;
    h.texts_set = true;
    c.set_if_changed(f.hud_active, look.hud_active);
}
void show(Hud& h, const Context& c, const Line& line, bool ours_in, double now) {
    if (h.model.handle && (!alive(c, h.model) || !alive(c, h.item))) {
        // Gone under us: an item of ours still in the stack is bound to the old model, so it goes too.
        if (ours_in) ours_in = !hud_stack::remove_key(c, h.stack, score_key);
        destroy_ours(h, c);
    }
    if (!h.model.handle && !make_model(h, c)) return;
    // A new line's rows come in as the game's do, into an empty list.
    if (h.line_start.shown(line.closed)) clear_rows(h, c);
    const auto look = look_of(line, score::action_fill_points);
    publish(h, c, look);
    show_rows_or_none(h, c, look.rows, now);
    // No write to our models is ever a trick's, whoever makes it (the widget writes ours too).
    std::vector<std::uint64_t> entries{h.model.handle >> 16};
    for (const auto& row : h.rows) entries.push_back(row.model.handle >> 16);
    tricks::feed::own(entries);
    if (!ours_in) {
        h.presence.inserting(); // from the try: the take-out looks whatever happens
        hud_stack::insert_item(c, h.stack, h.item);
        if (!std::exchange(h.said_in, true))
            logging::write(logging::Level::info, logging::Channel::ui, "Score line: a plugin's line went into skate.'s score HUD.");
    }
    h.out_since = 0;
}
// Ours out of the stack; done once a read of the stack finds none of ours.
void take_out(Hud& h, const Context& c, bool ours_in, double now) {
    if (ours_in && hud_stack::remove_key(c, h.stack, score_key) && !std::exchange(h.said_out, true))
        logging::write(logging::Level::info, logging::Channel::ui, "Score line: a plugin's line left skate.'s score HUD.");
    h.presence.scanned(hud_stack::find_key(c, h.stack, score_key).has_value());
    if (!h.presence.must_scan() && !h.out_since) h.out_since = now;
}

// What of skate.'s own line is in its root now (root_busy says why that matters), `item` its score
// item in the stack.
GameRoot root_of(const Context& c, const Value& root, bool item) {
    GameRoot r;
    r.hud_active = read<std::uint8_t>(c.address(sized(c, root, corner::hud_widget_active, 1))) != 0;
    r.line_timer = read<float>(c.address(sized(c, root, score::line_timer, 4)));
    r.trick_rows = count_of(c, c.path(root, {score::trick_list, corner::stack_items}));
    r.second_rows = count_of(c, c.path(root, {score::second_stack, corner::stack_items}));
    r.item = item;
    return r;
}

void step(Hud& h, const Context& c, const Line* line, std::string why, double now) {
    if (h.manager != c.manager) forget(h, c.manager);
    find_game(h, c, now);
    bool ours_in{}, game_item{};
    if (h.stack.handle) scan_stack(h, c, ours_in, game_item);
    // skate.'s own line, from this tick's reading of its root and its item in the stack.
    const bool fresh = h.read_at && now - *h.read_at <= fresh_reading_ms;
    auto signals = fresh && h.hud_read ? h.signals : GameSignals{};
    signals.item = game_item;
    h.game.observe(signals, now);
    const bool game_up = h.game.running(now);
    bool wait{};
    if (why.empty() && line) {
        if (game_up) {
            why = "skate.'s own line is showing";
            if (!std::exchange(h.said_yield, true))
                logging::log(logging::Level::info, logging::Channel::ui, "Score line: a plugin's line yields to skate.'s own ({}).", h.game.why());
        } else if ((fresh && !h.hud_read) || !h.stack.handle || !h.game_root.handle) {
            why = "skate.'s score HUD is not loaded";
        } else if (!fresh) {
            // The first tick of a line has no reading yet (it comes on the next); ours never stays up without one.
            if (ours_in || h.presence.must_scan()) why = "skate.'s score HUD is unavailable now";
            else wait = true;
        } else if (!h.model.handle || !alive(c, h.model) || !alive(c, h.item)) {
            // Ours is about to be made as a copy of the game's root: not while its own line is in it.
            try {
                if (const auto busy = root_busy(root_of(c, h.game_root, game_item)); !busy.empty()) {
                    why = "skate.'s own line is showing";
                    if (!std::exchange(h.said_busy, true))
                        logging::log(logging::Level::info, logging::Channel::ui, "Score line: a plugin's line waits for skate.'s own to leave its root ({}).",
                            busy);
                }
            } catch (const std::exception& error) {
                why = "skate.'s score HUD is unavailable now";
                if (!std::exchange(h.said_busy, true))
                    logging::log(logging::Level::warning, logging::Channel::ui, "Score line: skate.'s score HUD root could not be read: {}.", error.what());
            }
        }
    }
    if (!line || !why.empty() || wait) {
        h.line_start.out();
        if (h.stack.handle && (ours_in || h.presence.must_scan())) take_out(h, c, ours_in, now);
        if (!h.presence.must_scan() && !h.out_since) h.out_since = now;
        if (!wait) h.status = line ? why : std::string{};
        if (h.model.handle && !h.presence.must_scan() && now - h.out_since >= destroy_after_ms) destroy_ours(h, c);
        return;
    }
    try {
        show(h, c, *line, ours_in, now);
    } catch (...) {
        if (h.stack.handle) take_out(h, c, hud_stack::find_key(c, h.stack, score_key).has_value(), now);
        throw;
    }
    h.status = h.unsupported && !h.model.handle ? "skate.'s score HUD cannot be shown on this game build" : "";
}
} // namespace

std::string ask(std::string_view who, std::optional<Line> line) {
    const bool clearing = !line;
    auto why = owner().ask(who, std::move(line), now_ms());
    if (!why.empty() || clearing) return why;
    return hud().status;
}

void release(std::string_view who) { owner().release(who); }

bool wanted() noexcept {
    const auto& h = hud();
    return !owner().owner().empty() || h.model.handle || h.presence.must_scan();
}

void observe(const std::optional<tricks::ScoreHud>& reading) noexcept {
    auto& h = hud();
    h.read_at = now_ms();
    h.hud_read = reading.has_value();
    h.signals = reading ? GameSignals{reading->action, reading->multiplier, reading->line_score, reading->line_shown, false} : GameSignals{};
}

void tick(std::uintptr_t base, std::string_view refusal) noexcept {
    auto& h = hud();
    try {
        const auto now = now_ms();
        const Line* line = owner().current(now);
        // A hidden corner comes first, then a refusal; skate.'s own line is told apart in step.
        std::string why;
        if (hud_corner::hidden_by_plugin()) why = "skate.'s HUD corner is hidden";
        else if (!refusal.empty()) why = std::string(refusal);
        else if (h.unsupported) why = "skate.'s score HUD cannot be shown on this game build";
        if (!line && !h.model.handle && !h.presence.must_scan()) {
            h.status.clear();
            return;
        }
        const ui::OwnUiWrites own_writes; // the trick feed ignores what we write
        if (!hud_stack::with_ui_models(base, [&](const Context& c) { step(h, c, line, why, now); }))
            h.status = line ? std::string("skate.'s score HUD is not loaded") : std::string{};
        else h.warned = false;
    } catch (const std::exception& error) {
        h.status = "skate.'s score HUD is unavailable now";
        if (!std::exchange(h.warned, true))
            logging::log(logging::Level::warning, logging::Channel::ui, "Score line: a plugin's line could not be shown: {}.", error.what());
    } catch (...) {
        h.status = "skate.'s score HUD is unavailable now";
    }
}

void before_level_load(std::uintptr_t base) noexcept {
    auto& h = hud();
    bool gone = true; // nothing of ours is left in the game's UI
    if (h.model.handle || h.presence.must_scan()) {
        gone = false;
        try {
            const ui::OwnUiWrites own_writes;
            const bool registry = hud_stack::with_ui_models(base, [&](const Context& c) {
                if (c.manager != h.manager) { // a registry of its own: ours went with the old one
                    gone = true;
                    return;
                }
                if (h.stack.handle && alive(c, h.stack)) {
                    (void)hud_stack::remove_key(c, h.stack, score_key);
                    h.presence.scanned(hud_stack::find_key(c, h.stack, score_key).has_value());
                } else h.presence.scanned(false); // the stack went, and our item with it
                destroy_ours(h, c);
                gone = !h.presence.must_scan();
            });
            if (!registry) gone = true; // no registry, and ours with it
        } catch (const std::exception& error) {
            logging::log(logging::Level::warning, logging::Channel::ui, "Score line: a plugin's line could not be taken out before a level load: {}.",
                error.what());
        } catch (...) {}
    }
    // The next level's HUD is found again. What could not be taken out keeps what its take-out needs, so
    // the next tick tries again: a ReSkate load can still be cancelled after this, and its HUD stays.
    if (gone) forget(h, 0);
    h.status.clear();
}

void before_level_transition(std::uintptr_t base, unsigned next) noexcept {
    // The world ends (as plugin_effects.cpp and placement_physics.cpp take it): 3, 14, 22, 24 and 25.
    if (next == 3 || next == 14 || next == 22 || next == 24 || next == 25) before_level_load(base);
}

std::uint64_t own_model() noexcept { return own_root.load(std::memory_order_acquire); }
} // namespace dingosdk::score_line
