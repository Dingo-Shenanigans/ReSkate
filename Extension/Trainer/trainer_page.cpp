#include "trainer_page.h"
#include "trainer.h"
#include "trainer_classes.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <format>
#include <span>
#include <string>
#include <vector>

// The TRAINER page and the skating HUD. Presentation thread: reads the trainer's snapshots
// and queues `trainer ...` console commands; it never touches the game.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
    int tab{}; // opens on TUNE: the dials and switches
    std::array<char, 96> search{};
    // The Tune tab is one page of sections. A function that draws several cards is told which of them a section
    // takes (`pick`; empty: all), and card_end() has to know whether the last one was left out.
    std::vector<std::string_view> pick;
    bool card_skipped{};
    int scroll_to{-1};   // a section `trainer open` asked for: opened and scrolled to, once
    bool folds_seeded{}; // the saved folds were handed to the menu
    bool manage{};       // the preset row's YOUR PRESETS card is open
    int preset_pick{-1}; // which of the player's presets the row's box shows
    // Rows a card shows by name (left out of their section's FINE TUNING), and each section's named rows by rank.
    std::vector<char> placed;
    std::uint64_t placed_revision{~0ull};
    std::array<std::vector<std::size_t>, trainer::topic_count> topic_rows;
    int group{1}; // EVERY VALUE: 1: every group, 2..: one of the view's groups
    bool only_changed{}, graph_points{}, show_unused{};
    std::size_t hidden_unused{}; // rows the filters would show but for "no use found"
    // The rows the filters leave, rebuilt when the snapshot or a filter changes.
    std::vector<std::size_t> shown;
    std::uint64_t shown_revision{};
    std::string shown_key;
    // The value under the pointer while it is dragged: the snapshot lags a frame or two.
    std::string active;
    double active_value{};
    std::array<char, 49> preset_name{};
    std::array<float, 3> teleport{};
    double speed_edit{-1}, speed_until{};
    float manual_edit{1}, hippy_edit{1}, nocomply_edit{1}, boneless_edit{1}, offboard_edit{1}, flip_edit{1}, pump_edit{1}, underflip_edit{1};
    std::array<float, trainer::flip_tricks.size()> trick_edit{}; // the speed of each flip trick
    std::array<bool, trainer::flip_tricks.size()> trick_editing{};
    float grind_side_edit{1}, grind_push_edit{};
    float grind_range_edit{};
    bool grind_range_editing{};
    bool grind_push_editing{};
    bool grind_side_editing{}, grind_watching{};
    float revert_edit{};
    bool revert_editing{};
    // Rows of the value table that cards show by name, for one snapshot (place_rows).
    std::array<std::vector<std::size_t>, 4> trick_rows;
    std::array<std::vector<std::size_t>, 6> transition_rows;
    bool manual_editing{}, hippy_editing{}, nocomply_editing{}, boneless_editing{}, offboard_editing{}, flip_editing{}, pump_editing{}, underflip_editing{};
    std::uint64_t open_serial{}; // the last `trainer open` acted on
    std::uint64_t share_serial{}; // the last `preset export` put on the clipboard
    bool share_seen{};
    bool show_page{}, show_cheats{};
    float gravity_edit{1}, gravity_landing_edit{1}, gravity_timing_edit{1};
    bool gravity_editing{}, gravity_landing_editing{}, gravity_timing_editing{};
    // QUICK SETTINGS: what is changed (kind 0 a section's line, 1 a dial, 2 one of the trainer's settings, 3 a value).
    struct QuickItem {
        std::uint8_t kind{}, topic{};
        std::size_t index{};
    };
    std::vector<QuickItem> quick_all;
    std::uint64_t quick_revision{~0ull};
    bool in_quick{};     // drawing that list: a row there is not the one "Go to" points at
    // "Go to": the value to point at (tinted for a few seconds, scrolled to once), and the section whose cards open.
    std::string flash;
    double flash_until{};
    bool flash_scroll{};
    float flash_y{-1};
    int open_cards{-1}, current_topic{-1};
};
void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const std::string &words, int part);
void trickline_section(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view);
void transition_section(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view);
Page &page() {
    static auto *value = new Page;
    return *value;
}
// A card of this page: it folds, and a section can leave it out (Page::pick). card_end() follows either way.
bool card(SkateMenu &menu, Page &p, const char *id, const char *title, const char *subtitle = nullptr) {
    p.card_skipped = !p.pick.empty() && std::ranges::find(p.pick, std::string_view(id)) == p.pick.end();
    if (!p.card_skipped && p.open_cards > 0 && p.open_cards == p.current_topic) set_fold(id, true); // "Go to" shows what it points at
    return !p.card_skipped && fold_card(menu, id, title, subtitle);
}
void card_end(Page &p) {
    if (!p.card_skipped) end_card();
    p.card_skipped = false;
}
// A slider shows what the player is dragging until the trainer's own number catches up; after a reset there is nothing
// to catch up with.
void clear_edits(Page &p) {
    p.manual_editing = p.hippy_editing = p.nocomply_editing = p.boneless_editing = p.offboard_editing = p.flip_editing = p.pump_editing = p.revert_editing =
        p.underflip_editing = false;
    p.grind_side_editing = p.grind_push_editing = p.grind_range_editing = p.gravity_editing = p.gravity_landing_editing =
        p.gravity_timing_editing = false;
    p.trick_editing = {};
    p.active.clear();
}
// A tooltip that wraps at a readable width instead of running off the screen as one line.
void wrapped_tooltip(const char *format, ...) {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    va_list arguments;
    va_start(arguments, format);
    ImGui::TextV(format, arguments);
    va_end(arguments);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
// What the control just drawn does, shown while the pointer (or the controller's focus) is on it.
void tip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapped_tooltip("%s", text);
}
std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
bool contains_words(const std::string &haystack, const std::string &words) {
    std::size_t start = 0;
    while (start < words.size()) {
        auto end = words.find(' ', start);
        if (end == std::string::npos) end = words.size();
        if (end > start && haystack.find(words.substr(start, end - start)) == std::string::npos) return false;
        start = end + 1;
    }
    return true;
}
std::string number(double value) { return std::format("{:.6g}", value); }
void trainer_command(SkateMenu &menu, const CallbacksV3 &callbacks, const std::string &text) {
    send_console(menu, callbacks, "trainer " + text);
}
// A name the player typed, as one console argument: quoted, with its quotes and backslashes escaped (a preset called
// Andy's would otherwise open a quote the console never sees closed, and nothing was saved).
std::string console_arg(std::string_view name) {
    std::string out = "\"";
    for (const char c : name) {
        if (c == '"' || c == '\\' || c == '\'') out += '\\';
        out += c;
    }
    return out + '"';
}

// The rows EVERY VALUE and a search show.
void filter_rows(Page &p, const trainer::View &view) {
    const auto words = lower(p.search.data());
    const auto key = std::format("{}|{}|{}|{}|{}", words, p.group, p.only_changed, p.graph_points, p.show_unused);
    if (p.shown_revision == view.revision && p.shown_key == key) return;
    p.shown_revision = view.revision;
    p.shown_key = key;
    p.shown.clear();
    p.hidden_unused = 0;
    const std::string *group = p.group > 1 && static_cast<std::size_t>(p.group) <= view.groups.size() + 1 ? &view.groups[static_cast<std::size_t>(p.group) - 2] : nullptr;
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const auto &row = view.rows[i];
        if (row.detail && !p.graph_points && !row.touched) continue;
        if (p.only_changed && !row.touched && !row.frozen) continue;
        // Searching looks through every group.
        if (words.empty() ? (group && row.group != *group) : !contains_words(lower(row.id + " " + row.label + " " + row.friendly), words))
            continue;
        // A value nothing in the game reads would be a slider that does nothing.
        if (!row.used && !p.show_unused && !row.touched && !row.frozen) {
            ++p.hidden_unused;
            continue;
        }
        p.shown.push_back(i);
    }
}
// Rows of the value table that cards show by name.
constexpr const char *trick_groups[]{
    "onboard_powerslide.frictionscalar_revert onboard_powerslide.frictionscalar_autorevert "
    "onboard_powerslide.powerslide_forwardforcescalar_revert onboard_powerslide.powerslide_forwardforcescalar_autorevert",
    "physicsmode.jumpmaxheight physicsmode.jumpminheight physicsmode.jumpminheightmanual physicsonboardjumptuning.jumpadjustvelocityboostlimitground",
    "physicsairstates.maxspinspeed physicsmode.maxautobodyspinspeed physicsmode.easybodyspins physicsreckoning.flipscalar "
    "physicsreckoning.flipmaxspeed heldflip.straightflipcatchtime heldflip.shuvmincatchtime heldflip.varialmincatchtime heldflip.bigflipmincatchtime",
    "physicsmanual.manualscalar_p physicsmanual.manualscalar_i physicsmanual.manualscalar_d physicsmanual.maxtiltangle physicsfriction.noinputtime "
    "physicssteering.generalscalar physicssteering.damping",
};
constexpr std::string_view coping_rows[]{"physicsgrind.copingexitstarttime", "physicsgrind.fairlysteepexitstarttime", "physicsgrind.curbexitstarttime",
                                         "physicsgrind.slowgrindexitspeed", "physicsgrind.nonpopexitangledeg", "physicsgrind.grindreckoningsmoothnessonexit",
                                         "physicsgrind.grindreckoningsmoothnessdropin"};
constexpr std::string_view reentry_rows[]{"physicstrajectory.scoregrind", "physicsgrindsair.maxdist50_50", "physicsgrindsair.maxdist5_0",
                                          "physicsgrindsair.maxdisttipslide", "physicsgrindsair.maxdistboardslide", "physicstrajectory.grindlandingmaxangle"};
constexpr std::string_view plant_rows[]{"physicshandplant.exitanglerangestart", "physicshandplant.exitanglerangeend", "physicshandplant.minnormalvel",
                                        "physicshandplant.maxnormalvel", "physicshandplant.maxparallelvel", "physicshandplant.maxverticalvel",
                                        "physicshandplant.minlipclearance", "physicshandplant.maxlipclearance", "physicshandplant.maxangleofapproach",
                                        "physicshandplant.waitforanimtime", "physicshandplant.bstofsanglethreshold", "physicshandplant.fstobsanglethreshold",
                                        "physicshandplant.linesegmentoffset", "physicshandplant.linesegmentpad", "physicshandplant.segmentcastheightpad"};
constexpr std::string_view lip_rows[]{"physicsliptrick.liptrickcomapexheight", "physicsliptrick.timetoapexvsspeed x", "physicsliptrick.skateboardrotationslerpvstime x",
                                      "physicsliptrick.maxrotationpersecond", "physicsliptrick.lipquerymaxlipdistance",
                                      "physicsliptrick.lipquerymaxdirectionchangedegrees", "physicsliptrick.launchgroundslopethreshold",
                                      "physicsliptrick.lipquerylipslopethreshold", "physicsliptrick.lipquerygroundslopethreshold", "physicsrollin.maxrollintime",
                                      "physicsrollin.skateboardrollinduration", "physicsrollin.minlipbashdownwardspeed", "physicsrollin.undershootamount"};
constexpr std::string_view vert_rows[]{"physicsreckoning.vertjumpalignmaxgroundnormaly", "physicsreckoning.vertjumpalignmaxangle",
                                       "physicsreckoning.vertjumpalignminjumpdiry", "physicsreckoning.vertjumpalignfactor",
                                       "physicstrajectory.naturalairoffvertsscalar", "physicsairstates.naturalairmaxdist", "physicstrajectory.scoretransition",
                                       "physicstrajectory.mintimeafterapexfortransition", "physicstrajectory.scoremiddlebonus",
                                       "physicstrajectory.scorelandingdirection", "physicstrajectory.maxtrajectoryadjust", "physicstrajectory.coneanglex",
                                       "physicstrajectory.coneanglez", "physicstrajectory.mintimefortrajectorytobevalid"};
constexpr std::string_view pump_rows[]{"physicsmode.pumpeffectfactor", "physicsmode.pumpmaxacceleration", "physicsmode.pumpmaxdeceleration",
                                       "physicsmode.unintentionalpumpscalar", "physicspumping.pumpangle", "physicspumping.pumpminfactor",
                                       "physicspumping.pumpoutoftransitionscalar", "physicspumping.pumpoutoftransitioncheckdistancescalar",
                                       "physicsantipumping.maxdownwardangledeg", "physicsantipumping.maxdownwardangledeg_nobanks"};
constexpr std::span<const std::string_view> transition_lists[]{coping_rows, reentry_rows, plant_rows, lip_rows, vert_rows, pump_rows};
// Once per snapshot: where each of those rows is in the table, and each section's other named rows in their order.
void place_rows(Page &p, const trainer::View &view) {
    if (p.placed_revision == view.revision) return;
    p.placed_revision = view.revision;
    p.placed.assign(view.rows.size(), 0);
    std::vector<std::string> ids(view.rows.size());
    for (std::size_t i = 0; i < view.rows.size(); ++i) ids[i] = lower(view.rows[i].id);
    const auto find = [&](std::string_view key, std::vector<std::size_t> &into) {
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == key) {
                into.push_back(i);
                p.placed[i] = 1;
                return;
            }
    };
    for (std::size_t g = 0; g < std::size(trick_groups); ++g) {
        p.trick_rows[g].clear();
        const std::string_view keys = trick_groups[g];
        for (std::size_t start = 0; start < keys.size();) {
            auto end = keys.find(' ', start);
            if (end == std::string_view::npos) end = keys.size();
            if (end > start) find(keys.substr(start, end - start), p.trick_rows[g]);
            start = end + 1;
        }
    }
    for (std::size_t g = 0; g < std::size(transition_lists); ++g) {
        p.transition_rows[g].clear();
        for (const auto key : transition_lists[g]) find(key, p.transition_rows[g]);
    }
    for (auto &rows : p.topic_rows) rows.clear();
    for (std::size_t i = 0; i < view.rows.size(); ++i)
        if (const auto &row = view.rows[i]; row.topic && row.topic < trainer::topic_count && !row.friendly.empty() && !p.placed[i]) p.topic_rows[row.topic].push_back(i);
    for (auto &rows : p.topic_rows) std::ranges::sort(rows, {}, [&](std::size_t i) { return view.rows[i].rank; });
}
// A row belongs on a section when it does something: the game was found reading it, or it has been changed or locked
// (so it can be put back). The rest are on EVERY VALUE only.
bool row_works(const trainer::Row &row) { return row.used || row.touched || row.frozen; }

void value_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::Row &row,
               bool with_group, bool friendly) {
    if (console_style()) {
        // Full screen: a switch, or the value nudged a step at a time (a fiftieth of its size).
        ImGui::PushID(row.id.c_str());
        const auto label = friendly && !row.friendly.empty() ? row.friendly : with_group ? row.group + " / " + row.label : row.label;
        const auto hint = std::format("{}{}{}. The game's own is {}.{}", row.help, row.help.empty() ? "" : " ", row.id, number(row.stock),
                                      row.used ? "" : " The game was not found reading it, so changing it may do nothing.");
        const bool can = view.editable && callbacks.queue_console_command;
        if (row.kind == trainer::Kind::flag) {
            bool on = row.value != 0;
            if (toggle_row(menu, label.c_str(), hint.c_str(), on, can)) // (a label that changes would lose the focus)
                trainer_command(menu, callbacks, std::format("set {} {}", row.id, on ? 1 : 0));
        } else {
            field(menu, (row.touched ? label + "  *" : label).c_str(), hint.c_str());
            const bool scale = row.kind == trainer::Kind::curve || row.kind == trainer::Kind::graph;
            const bool whole = row.kind == trainer::Kind::integer;
            double value = row.value;
            set_console_step(whole ? 1.0 : scale ? 0.05 : std::max({std::abs(row.stock), std::abs(value), 0.01}) * 0.02);
            // The bar spans a sensible range around the game's own value (sliding covers it); the box beside it, or
            // stepping on past the end, takes any number.
            const double size = std::max({std::abs(row.stock), whole ? 5.0 : 0.01});
            const double low = scale ? 0.0 : row.stock >= 0 ? (row.stock == 0 ? -size * 10 : 0.0) : row.stock * 4,
                         high = scale ? 4.0 : row.stock <= 0 ? (row.stock == 0 ? size * 10 : 0.0) : row.stock * 4;
            ImGui::BeginDisabled(!can);
            if (slider_double("##value", &value, low, high, whole ? "%.0f" : scale ? "x %.2f" : "%.4g"))
                trainer_command(menu, callbacks, std::format("set {} {}", row.id, number(value)));
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        return;
    }
    ImGui::PushID(row.id.c_str());
    if (!p.in_quick && !p.flash.empty() && p.flash == row.id) {
        // "Go to" pointed here.
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x - px(4), at.y - px(1)), ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + ImGui::GetFrameHeight() + px(1)),
                                                  IM_COL32(60, 130, 230, 80));
        p.flash_y = at.y;
    }
    bool frozen = row.frozen;
    if (ImGui::Checkbox("##freeze", &frozen)) trainer_command(menu, callbacks, std::format("freeze {} {}", row.id, frozen ? 1 : 0));
    if (ImGui::IsItemHovered()) wrapped_tooltip("Lock: presets and Reset values leave this value alone.\nReset everything (top of the page) clears locks too.\nYou do not need it for a change to apply.");
    ImGui::SameLine();
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.52f;
    const auto label = friendly && !row.friendly.empty() ? row.friendly : with_group ? row.group + " / " + row.label : row.label;
    const bool tinted = row.touched || !row.used;
    if (tinted) ImGui::PushStyleColor(ImGuiCol_Text, row.used ? skate_theme::blue : IM_COL32(150, 150, 150, 255));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.used ? label.c_str() : (label + "  (no use found)").c_str());
    if (tinted) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        wrapped_tooltip("%s\n%s\nStock: %s%s",
            row.help.empty() ? std::format("{} / {}: one of the game's own tuning values, under the game's own name. No plain explanation "
                                           "is written for it yet; the name says what it belongs to.", row.group, row.label).c_str() : row.help.c_str(),
            row.id.c_str(), number(row.stock).c_str(),
            row.used ? "" : "\nThe game was not found (or seen) reading this value, so changing it may do nothing.");
    ImGui::SameLine(column);
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
    double value = p.active == row.id ? p.active_value : row.value;
    bool edited = false;
    if (row.kind == trainer::Kind::flag) {
        bool on = value != 0;
        if (ImGui::Checkbox("##value", &on)) {
            value = on ? 1 : 0;
            edited = true;
        }
    } else {
        ImGui::SetNextItemWidth(std::max(px(90), ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x));
        const bool scale = row.kind == trainer::Kind::curve || row.kind == trainer::Kind::graph;
        const bool whole = row.kind == trainer::Kind::integer;
        const double low = scale ? -1.0e9 : -1.0e12, high = scale ? 1.0e9 : 1.0e12; // none of the trainer's own
        const auto speed = whole ? 0.1f : scale ? 0.01f
            : static_cast<float>(std::max({std::abs(row.stock), std::abs(value), 0.01}) * 0.004);
        edited = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, speed, &low, &high, whole ? "%.0f" : scale ? "x %.2f" : "%.4g",
            ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) {
            p.active = row.id;
            p.active_value = value;
        } else if (p.active == row.id) {
            p.active.clear();
        }
    }
    if (!ImGui::IsItemActive())
        tip(row.kind == trainer::Kind::flag ? "Tick to switch this on, untick to switch it off."
                                            : "Drag left or right to change it, or double-click to type a number. Blue means changed from the game's own.");
    if (edited) trainer_command(menu, callbacks, std::format("set {} {}", row.id, number(value)));
    if (row.touched && row.kind != trainer::Kind::flag) {
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(reset, 0))) trainer_command(menu, callbacks, "reset " + row.id);
        tip("Puts this one value back to the game's own.");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// A built-in preset as a switch: blue while every value it sets holds what it sets.
void preset_switch(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::PresetRow &preset) {
    if (preset.active) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button(preset.name.c_str());
    if (preset.active) ImGui::PopStyleColor();
    if (pressed) trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + console_arg(preset.name));
    if (ImGui::IsItemHovered() && !preset.note.empty()) wrapped_tooltip("%s", preset.note.c_str());
}
// A built-in preset as a dial: one multiplier that moves everything the preset moves.
void dial_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::PresetRow &preset) {
    if (console_style()) {
        ImGui::PushID(preset.name.c_str());
        const auto hint = std::format("{} 1 is the game's own; \"{}\" is x {}.", preset.note, preset.name, number(preset.amount));
        field(menu, std::abs(preset.factor - 1.0) > 1e-4 ? (preset.title + "  *").c_str() : preset.title.c_str(), hint.c_str());
        double value = preset.factor;
        // From 0: Grind lock-on at 0 is no lock-on (the owner's ask, 2026-10-19).
        const double low = 0.0, high = preset.amount < 1 ? 3.0 : 10.0;
        if (slider_double("##dial", &value, low, high, "x %.2f", ImGuiSliderFlags_Logarithmic) && value >= 0)
            trainer_command(menu, callbacks, std::format("dial {} {}", number(value), preset.name));
        ImGui::PopID();
        (void)p;
        return;
    }
    ImGui::PushID(preset.name.c_str());
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.40f;
    const bool moved = std::abs(preset.factor - 1.0) > 1e-4;
    if (moved) ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(preset.title.c_str());
    if (moved) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        wrapped_tooltip("%s\n1 is the game's own; \"%s\" is x %s.", preset.note.c_str(), preset.name.c_str(), number(preset.amount).c_str());
    ImGui::SameLine(column);
    const float button = ImGui::CalcTextSize("Easy Body Flips  ").x + ImGui::GetStyle().FramePadding.x * 2;
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    const auto id = "dial:" + preset.name;
    double value = p.active == id ? p.active_value : preset.factor;
    // A preset that turns its value down gets a dial with less room above 1.
    const double low = 0.0, high = preset.amount < 1 ? 3.0 : 10.0; // from 0, as the console menu's dial
    const float box = px(64);
    ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - button - reset - ImGui::GetStyle().ItemSpacing.x * 3));
    if (ImGui::SliderScalar("##dial", ImGuiDataType_Double, &value, &low, &high, "x %.2f", ImGuiSliderFlags_Logarithmic) && value >= 0)
        trainer_command(menu, callbacks, std::format("dial {} {}", number(value), preset.name));
    if (ImGui::IsItemActive()) {
        p.active = id;
        p.active_value = value;
    } else if (p.active == id) {
        p.active.clear();
    }
    if (!ImGui::IsItemActive() && ImGui::IsItemHovered()) wrapped_tooltip("%s 1 is the game's own.", preset.note.c_str());
    ImGui::SameLine();
    // Any number at all: the slider only covers the sensible range.
    double typed = preset.factor;
    ImGui::SetNextItemWidth(box);
    if (ImGui::InputDouble("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && typed >= 0)
        trainer_command(menu, callbacks, std::format("dial {} {}", number(typed), preset.name));
    if (ImGui::IsItemHovered()) wrapped_tooltip("Type any multiplier and press Enter: the slider's ends are not a limit. 0 turns what it moves to 0.");
    ImGui::SameLine();
    if (preset.active) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button(preset.name.c_str(), ImVec2(button, 0));
    if (preset.active) ImGui::PopStyleColor();
    if (pressed) trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + console_arg(preset.name));
    if (ImGui::IsItemHovered()) wrapped_tooltip("%s", preset.note.c_str());
    if (moved) {
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(reset, 0))) trainer_command(menu, callbacks, "dial 1 " + preset.name);
        tip("Puts this dial back to 1, the game's own.");
    }
    ImGui::PopID();
}

// The player's own presets, above the tabs: pick one and load it, save what is set now under a name. "Manage" opens
// the list: share one as a line of text, import one that was shared, delete, tie one to this map.
void preset_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    std::vector<const trainer::PresetRow *> own;
    for (const auto &preset : view.presets)
        if (!preset.builtin) own.push_back(&preset);
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
    if (p.preset_pick < 0 || p.preset_pick >= static_cast<int>(own.size())) {
        p.preset_pick = 0;
        for (std::size_t i = 0; i < own.size(); ++i)
            if (own[i]->active) p.preset_pick = static_cast<int>(i);
    }
    const trainer::PresetRow *picked = own.empty() ? nullptr : own[static_cast<std::size_t>(p.preset_pick)];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(190));
    if (ImGui::BeginCombo("##preset", picked ? picked->name.c_str() : "none saved yet")) {
        for (std::size_t i = 0; i < own.size(); ++i) {
            const auto label = own[i]->active ? own[i]->name + "   (on)" : own[i]->name;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == p.preset_pick)) p.preset_pick = static_cast<int>(i);
            ImGui::PopID();
        }
        if (own.empty()) ImGui::TextDisabled("Set the game up the way you like it, give it a name, press Save.");
        ImGui::EndCombo();
    }
    tip("Your saved setups. Pick one, then Load: it sets every value and setting saved in it on top of what is set now.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!picked || !can_edit);
    if (picked && picked->active) {
        ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
        if (ImGui::Button("Turn off")) {
            clear_edits(p);
            trainer_command(menu, callbacks, "preset remove " + console_arg(picked->name));
        }
        ImGui::PopStyleColor();
        tip("Puts everything this preset set back to the game's own.");
    } else {
        if (ImGui::Button("Load") && picked) {
            clear_edits(p);
            trainer_command(menu, callbacks, "preset apply " + console_arg(picked->name));
        }
        tip("Sets every value and setting saved in this preset. Press RESET EVERYTHING first to get exactly the preset and nothing else.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, px(18));
    ImGui::SetNextItemWidth(px(140));
    ImGui::InputTextWithHint("##preset-name", "new preset name", p.preset_name.data(), p.preset_name.size());
    tip("A name for the preset you are about to save.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!p.preset_name[0] || view.stock);
    if (ImGui::Button("Save")) trainer_command(menu, callbacks, std::string("preset save ") + console_arg(p.preset_name.data()));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        wrapped_tooltip("%s", view.stock ? "Nothing is changed, so there is nothing to save yet."
                                         : "Saves everything that is changed right now under this name: tuning values, trick, grind, transition and bending "
                                           "settings. Not saved: locks, No Bail, Noclip, Dark Pop, the game speed and your game's own settings.");
    ImGui::SameLine();
    if (p.manage) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button("Manage");
    if (p.manage) ImGui::PopStyleColor();
    if (pressed) p.manage = !p.manage;
    tip("Share a preset as a line of text, import one, delete one, or have one load by itself on this map.");
    if (!p.manage) return;
    begin_card(menu, "preset-manage", "YOUR PRESETS", "Share, import, delete, load by itself on this map");
    ImGui::BeginDisabled(!can_edit);
    const float button = ImGui::CalcTextSize("Turn off").x + ImGui::GetStyle().FramePadding.x * 2;
    for (const auto *preset : own) {
        ImGui::PushID(preset->name.c_str());
        if (preset->active) {
            ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
            if (ImGui::Button("Turn off", ImVec2(button, 0))) trainer_command(menu, callbacks, "preset remove " + console_arg(preset->name));
            ImGui::PopStyleColor();
        } else if (ImGui::Button("Load", ImVec2(button, 0))) {
            trainer_command(menu, callbacks, "preset apply " + console_arg(preset->name));
        }
        tip("Load sets every value and setting saved in this preset. Turn off puts those back to the game's own.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(preset->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", preset->note.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - px(205));
        if (ImGui::SmallButton("This map")) trainer_command(menu, callbacks, "profile set " + preset->name);
        tip("Loads this preset by itself every time this map loads.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Share")) trainer_command(menu, callbacks, "preset export " + console_arg(preset->name));
        if (ImGui::IsItemHovered()) wrapped_tooltip("Copies this preset to the clipboard as one line of text. Paste it in a chat;\nanyone with the trainer can import it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) trainer_command(menu, callbacks, "preset delete " + console_arg(preset->name));
        tip("Removes this preset from your list.");
        ImGui::PopID();
    }
    if (own.empty()) note("No preset saved yet: set the game up, type a name above and press Save.");
    ImGui::EndDisabled();
    // Sharing: a preset travels as one line of text on the clipboard.
    ImGui::BeginDisabled(view.stock);
    if (ImGui::Button("Share what is changed now")) trainer_command(menu, callbacks, "preset export current");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapped_tooltip("Copies your current setup to the clipboard as one line of text.");
    ImGui::SameLine();
    if (ImGui::Button("Import from clipboard")) {
        const char *text = ImGui::GetClipboardText();
        if (text && trainer::stage_import(text)) trainer_command(menu, callbacks, "preset import");
        else feedback(menu, "The clipboard holds no text.");
    }
    if (ImGui::IsItemHovered()) wrapped_tooltip("Copy a preset line someone shared (it starts with RST1:), then press this.\nIt is added to your presets; nothing changes until you load it.");
    if (!view.map.empty()) {
        info(menu, "This map loads", view.profile_preset.empty() ? "nothing by itself" : view.profile_preset);
        if (!view.profile_preset.empty() && ImGui::Button("Stop loading it on this map")) trainer_command(menu, callbacks, "profile clear");
    }
    end_card();
}

// ---- the first section: every kind of preset, and everything that is changed in one list ---------
// One of the trainer's own settings (a trick height, a grind switch, a bending rule ...) as a row that changes it.
void setup_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::SetupRow &row) {
    if (console_style()) {
        // Full screen: the controls the sections use (a mouse-only drag box here could not be changed from a pad).
        ImGui::PushID(row.key.c_str());
        const bool can = view.editable && callbacks.queue_console_command;
        const auto stock = row.kind == trainer::setup_mode ? std::string("left to the game") : row.kind == trainer::setup_switch ? std::string(row.stock != 0 ? "on" : "off")
                                                                                                                                  : number(row.stock);
        const auto hint = std::format("One of the trainer's own settings ({}). The game's own: {}. Go to shows it where it lives, with its explanation.", row.key, stock);
        if (row.kind == trainer::setup_switch) {
            bool on = row.value != 0;
            if (toggle_row(menu, row.title.c_str(), hint.c_str(), on, can)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, on ? 1 : 0));
        } else if (row.kind == trainer::setup_mode) {
            static constexpr const char *modes[]{"The game's own", "Held off", "Held on"};
            int mode = row.value < 0 ? 0 : row.value != 0 ? 2 : 1;
            field(menu, row.title.c_str(), hint.c_str());
            ImGui::BeginDisabled(!can);
            if (ImGui::Combo("##value", &mode, modes, 3)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, mode - 1));
            ImGui::EndDisabled();
        } else {
            field(menu, (row.value != row.stock ? row.title + "  *" : row.title).c_str(), hint.c_str());
            double value = row.value;
            const double size = std::max({std::abs(row.stock), std::abs(row.value), 0.05});
            set_console_step(size * 0.02);
            // The bar spans a sensible range around the game's own (stepping on past its ends, or the box beside it, goes further).
            const double low = std::max(row.low, row.stock > 0 ? 0.0 : -size * 4), high = std::min(row.high, row.stock > 0 ? row.stock * 4 : size * 4);
            ImGui::BeginDisabled(!can);
            if (slider_double("##value", &value, low, std::max(high, low + 1e-6), "%.4g")) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, number(value)));
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        return;
    }
    ImGui::PushID(row.key.c_str());
    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())); // where a value's lock box is
    ImGui::SameLine();
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.52f;
    ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.title.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        wrapped_tooltip("One of the trainer's own settings (%s). The game's own: %s. Go to shows it where it lives, with its explanation.", row.key.c_str(),
                        row.kind == trainer::setup_mode ? "left to the game" : row.kind == trainer::setup_switch ? (row.stock != 0 ? "on" : "off") : number(row.stock).c_str());
    ImGui::SameLine(column);
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command);
    const auto id = "setup:" + row.key;
    double value = p.active == id ? p.active_value : row.value;
    const float width = std::max(px(90), ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x);
    if (row.kind == trainer::setup_switch) {
        bool on = value != 0;
        if (ImGui::Checkbox("##value", &on)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, on ? 1 : 0));
    } else if (row.kind == trainer::setup_mode) {
        static constexpr const char *modes[]{"The game's own", "Held off", "Held on"};
        int mode = value < 0 ? 0 : value != 0 ? 2 : 1;
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("##value", &mode, modes, 3)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, mode - 1));
    } else {
        ImGui::SetNextItemWidth(width);
        const auto speed = static_cast<float>(std::max({std::abs(row.stock), std::abs(value), 0.05}) * 0.006);
        if (ImGui::DragScalar("##value", ImGuiDataType_Double, &value, speed, &row.low, &row.high, "%.4g", ImGuiSliderFlags_AlwaysClamp))
            trainer_command(menu, callbacks, std::format("setup {} {}", row.key, number(value)));
        if (ImGui::IsItemActive()) {
            p.active = id;
            p.active_value = value;
        } else if (p.active == id) {
            p.active.clear();
        }
        if (!ImGui::IsItemActive()) tip("Drag left or right to change it, or double-click to type a number.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(reset, 0))) {
        clear_edits(p);
        trainer_command(menu, callbacks, std::format("setup {} {}", row.key, number(row.stock)));
    }
    tip("Puts this one setting back to the game's own.");
    ImGui::EndDisabled();
    ImGui::PopID();
}
// Which feel the grind and the transition settings add up to: 0 skate.'s own, 1 Skate 3's, 2 a mix of the player's.
int grind_feel(const trainer::View &view) {
    bool like_skate = true, like_skate3 = true;
    for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
        if (view.grind_mode[i] >= 0) like_skate = false;
        if (view.grind_mode[i] != trainer::skate3_grind_mode(i)) like_skate3 = false;
    }
    for (std::size_t i = 0; i < trainer::grind_settings.size(); ++i) {
        if (view.grind_setting[i] >= 0) like_skate = false;
        if (view.grind_setting[i] != trainer::grind_settings[i].skate3) like_skate3 = false;
    }
    if (view.grind_push != 0.0f || view.grind_range != 0.0f) like_skate = false;
    if (std::abs(view.grind_push - 1.8f) > 0.005f || view.grind_range == 0.0f) like_skate3 = false;
    return like_skate ? 0 : like_skate3 ? 1 : 2;
}
int transition_feel(const trainer::View &view) {
    const bool preset_on = std::ranges::any_of(view.presets, [](const trainer::PresetRow &row) { return row.builtin && row.active && row.name == "Transition: Skate 3"; });
    bool like_skate = !preset_on, like_skate3 = preset_on;
    for (std::size_t i = 0; i < trainer::transition_settings.size(); ++i) {
        if (view.tr_setting[i] >= 0) like_skate = false;
        if (view.tr_setting[i] != trainer::transition_settings[i].skate3) like_skate3 = false;
    }
    if (view.tr_lean != 0.0f || view.tr_carry != 0.0f || view.tr_unstick != 0.0f || view.tr_lockout != 0.0f || view.tr_spine || view.tr_plant != 0.0f) like_skate = false;
    if (view.tr_lean == 0.0f || view.tr_carry == 0.0f || view.tr_unstick == 0.0f || view.tr_lockout == 0.0f || view.tr_plant == 0.0f) like_skate3 = false;
    return like_skate ? 0 : like_skate3 ? 1 : 2;
}
// Every kind of preset in one card: the whole-game feels, the grind and transition feels, the tricklining extras,
// the built-in styles and the player's own. Whatever one sets shows up in QUICK SETTINGS below it.
void presets_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, bool can_edit) {
    if (card(menu, p, "presets", "PRESETS", "A whole setup in one press. Blue = on. What it changes is listed in QUICK SETTINGS below.")) {
        ImGui::BeginDisabled(!can_edit);
        // Buttons in a row that wraps under where the row started.
        float column{};
        bool first = true;
        const auto begin = [&](const char *label, const char *about) {
            field(menu, label, about);
            column = ImGui::GetCursorPosX();
            first = true;
        };
        const auto button = [&](const char *label, bool on, const char *about) {
            const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
            if (!first) {
                if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x) ImGui::SameLine();
                else ImGui::SetCursorPosX(column);
            }
            first = false;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
            const bool pressed = ImGui::Button(label);
            if (on) ImGui::PopStyleColor();
            if (about && *about && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapped_tooltip("%s", about);
            if (pressed) clear_edits(p);
            return pressed;
        };
        const auto active = [&](std::string_view name) {
            return std::ranges::any_of(view.presets, [&](const trainer::PresetRow &row) { return row.builtin && row.active && row.name == name; });
        };
        begin("Whole game", "Skate 3's own numbers for everything the two games share, on one of its difficulties.");
        const bool any = active("Skate 3 Easy") || active("Skate 3") || active("Skate 3 Hardcore");
        if (button("skate.", !any, "The game's own tuning: takes a Skate 3 choice off again.")) trainer_command(menu, callbacks, "feel stock");
        if (button("Skate 3 Easy", active("Skate 3 Easy"), "Skate 3 on Easy: full-height pops every time, strong pushes, generous grind lock-on, easy spins.")) trainer_command(menu, callbacks, "feel easy");
        if (button("Skate 3", active("Skate 3"), "Skate 3 on Normal: its pop, grind pops, pushing, pumping, steering, manuals, spins, flips and bails.")) trainer_command(menu, callbacks, "feel normal");
        if (button("Skate 3 Hardcore", active("Skate 3 Hardcore"), "Skate 3 on Hardcore: lower pops, weaker pushes, tight grind lock-on, slow auto spins.")) trainer_command(menu, callbacks, "feel hardcore");
        const int grinds = grind_feel(view), transition = transition_feel(view);
        begin("Grinds", "How a grind is ridden and left. GRINDS has every row behind it.");
        if (button("skate.##grind", grinds == 0, "Every grind setting and switch is left to the game.")) trainer_command(menu, callbacks, "grind feel skate");
        if (button("Skate 3##grind", grinds == 1, "The left stick alone turns the board on a one-truck grind, the right stick tips and pops, a pop goes sideways the way the left stick is held, "
                                                   "and what skate. added to leaving a grind is off.")) trainer_command(menu, callbacks, "grind feel skate3");
        if (grinds == 2) {
            ImGui::SameLine();
            ImGui::TextDisabled("your own mix");
        }
        begin("Transition", "Bowls, quarter pipes, vert and spines. TRANSITION has every row behind it.");
        if (button("skate.##transition", transition == 0, "Everything on transition is left to the game.")) trainer_command(menu, callbacks, "transition feel skate");
        if (button("Skate 3##transition", transition == 1, "Lean into the ramp on a coping grind, roll off the coping at your angle, no hang-ups coming back in, handplants that come back in "
                                                            "high on the wall, with Skate 3's coping timers.")) trainer_command(menu, callbacks, "transition feel skate3");
        if (transition == 2) {
            ImGui::SameLine();
            ImGui::TextDisabled("your own mix");
        }
        begin("Tricklining", "What Skate 3's tricklining had beyond its tuning. TRICKLINING has every row behind it.");
        if (button("Skate 3's extras", view.revert_boost > 0, "The board bending boost at x 1, heavier revert and powerslide friction and higher footplants."))
            trainer_command(menu, callbacks, view.revert_boost > 0 ? "trickline extras off" : "trickline extras on");
        begin("Styles", "The built-in presets. Press one to put it on, press it again to take it off. Each also has a dial in its section.");
        for (const auto &preset : view.presets) {
            if (!preset.builtin || preset.name == "Stock" || preset.name.starts_with("Skate 3") || preset.name.starts_with("Transition:")) continue;
            if (button(preset.name.c_str(), preset.active, preset.note.c_str()))
                trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + console_arg(preset.name));
        }
        begin("Yours", "The presets you saved or imported. Save, share and delete them in the row above the tabs.");
        bool own = false;
        for (const auto &preset : view.presets) {
            if (preset.builtin) continue;
            own = true;
            ImGui::PushID(preset.name.c_str());
            if (button(preset.name.c_str(), preset.active, preset.active ? "On. Press to put what it set back to the game's own." : "Press to load it on top of what is set now."))
                trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + console_arg(preset.name));
            ImGui::PopID();
        }
        if (!own) ImGui::TextDisabled("none yet: set the game up, then name it and Save in the row above the tabs");
        ImGui::EndDisabled();
    }
    card_end(p);
}
// The cheats that change how the skater plays (Skater > Cheats): listed in QUICK SETTINGS, named on the MODIFIED line and
// turned back by RESET EVERYTHING like the trainer's own values.
struct CheatLine {
    std::string name, value; // value: what it is set to, empty for a plain switch
    int addon{-1};           // where it lives: an Addons page, else Skater > Cheats
    DebugRequest off{};      // how Reset puts it back: a debug request, or (with `console`) a command
    std::string console;
};
std::vector<CheatLine> cheat_lines(const Model &model) {
    std::vector<CheatLine> lines;
    const auto &debug = model.debug;
    if (debug.noclip) lines.push_back({"Noclip", {}, -1, {DebugAction::set_noclip, false}});
    if (debug.no_bail) lines.push_back({"No Bail", {}, -1, {DebugAction::set_no_bail, false}});
    if (debug.dark_pop) lines.push_back({"Dark Pop", {}, -1, {DebugAction::set_dark_pop_enabled, false}});
    // A boost's stock speed is 0: off (InteractiveDebug).
    const auto boost = [&](const char *name, float speed, DebugAction action) {
        if (speed > 1e-3f) lines.push_back({name, std::format("+{:.1f}", speed), -1, {action, false, 0.0f}});
    };
    boost("Forward boost", debug.forward_velocity_speed, DebugAction::set_forward_velocity_speed);
    boost("Up boost", debug.up_velocity_speed, DebugAction::set_up_velocity_speed);
    boost("Off-board up boost", debug.offboard_up_velocity_speed, DebugAction::set_offboard_up_velocity_speed);
    if (debug.revert_boost)
        lines.push_back({"Revert Boost", std::format("x{:.1f}", debug.revert_boost_speed), -1, {DebugAction::set_revert_boost_enabled, false}});
    // (Grind exits and Board Break are plugins now: their own pages say what they are set to.)
    return lines;
}
void cheat_off(SkateMenu &menu, const CallbacksV3 &callbacks, const CheatLine &line) {
    if (!line.console.empty()) {
        if (callbacks.queue_console_command) send_console(menu, callbacks, line.console);
    } else if (callbacks.queue_debug) debug_request(menu, callbacks, line.off);
}
// The cheats and addons rows at the top of QUICK SETTINGS: what each is set to, where it lives, and Reset.
void cheat_rows(SkateMenu &menu, const CallbacksV3 &callbacks, const std::vector<CheatLine> &lines) {
    if (lines.empty()) return;
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(menu.bold);
    ImGui::TextUnformatted("CHEATS AND ADDONS");
    ImGui::PopFont();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        ImGui::PushID(line.name.c_str());
        if (ImGui::Button("Go to")) {
            if (line.addon >= 0) open_page(menu, page_addons, line.addon);
            else {
                open_page(menu, page_skater);
                menu.skater_tab = 2;
            }
        }
        tip(line.addon >= 0 ? "Opens the addon's page." : "Opens Skater > Cheats.");
        ImGui::SameLine();
        ImGui::BeginDisabled(line.console.empty() ? !callbacks.queue_debug : !callbacks.queue_console_command);
        if (ImGui::Button("Reset")) cheat_off(menu, callbacks, line);
        ImGui::EndDisabled();
        tip(line.off.action == DebugAction::set_forward_velocity_speed || line.off.action == DebugAction::set_up_velocity_speed ||
                    line.off.action == DebugAction::set_offboard_up_velocity_speed
                ? "Back to 0: off, as the game shipped."
                : "Turns it off.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (line.value.empty()) ImGui::Text("%s: on", line.name.c_str());
        else ImGui::Text("%s: %s", line.name.c_str(), line.value.c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
}
std::string quick_id(std::string_view id) {
    std::string result = "qh:" + std::string(id);
    for (auto &c : result)
        if (c == ' ') c = '_';
    return result;
}
// Everything that is changed right now, in one list: change it here, hide a row that is done with, or go to where it lives.
void quick_card(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, bool can_edit) {
    constexpr std::uint8_t order[]{trainer::topic_game, trainer::topic_pop, trainer::topic_speed, trainer::topic_bails, trainer::topic_grinds, trainer::topic_trickline,
                                   trainer::topic_transition, trainer::topic_pumping, trainer::topic_foot, trainer::topic_none};
    // Once per snapshot: what is changed, section by section.
    if (p.quick_revision != view.revision) {
        p.quick_revision = view.revision;
        p.quick_all.clear();
        std::vector<std::string> ids;
        for (const auto topic : order) {
            for (std::size_t i = 0; i < view.presets.size(); ++i)
                if (const auto &preset = view.presets[i]; preset.builtin && preset.dial && preset.topic == topic && std::abs(preset.factor - 1.0) > 1e-4) {
                    p.quick_all.push_back({1, topic, i});
                    ids.push_back(quick_id("dial:" + preset.name));
                }
            for (std::size_t i = 0; i < view.setups.size(); ++i)
                if (view.setups[i].topic == topic) {
                    p.quick_all.push_back({2, topic, i});
                    ids.push_back(quick_id(view.setups[i].key));
                }
            for (std::size_t i = 0; i < view.rows.size(); ++i)
                if (const auto &row = view.rows[i]; row.touched && (row.topic < trainer::topic_count ? row.topic : 0) == topic) {
                    p.quick_all.push_back({3, topic, i});
                    ids.push_back(quick_id(row.id));
                }
        }
        // A hidden row that has gone back to the game's own is forgotten: changed again, it shows again.
        for (const auto &[id, hidden] : view.folds)
            if (hidden && id.starts_with("qh:") && std::ranges::find(ids, id) == ids.end()) {
                set_fold(id, false);
                trainer_command(menu, callbacks, std::format("fold {} 0", id));
            }
    }
    const auto id_of = [&](const Page::QuickItem &item) {
        return item.kind == 1 ? quick_id("dial:" + view.presets[item.index].name) : item.kind == 2 ? quick_id(view.setups[item.index].key) : quick_id(view.rows[item.index].id);
    };
    const auto hide = [&](const std::string &id, bool hidden) {
        set_fold(id, hidden);
        trainer_command(menu, callbacks, std::format("fold {} {}", id, hidden ? 1 : 0));
    };
    // What is shown: the rows not hidden, each section under a line of its own.
    std::vector<Page::QuickItem> shown;
    std::size_t hidden{};
    int last = -1;
    for (const auto &item : p.quick_all) {
        if (item.index >= (item.kind == 1 ? view.presets.size() : item.kind == 2 ? view.setups.size() : view.rows.size())) continue;
        if (fold_open(id_of(item), false)) {
            ++hidden;
            continue;
        }
        if (last != item.topic) shown.push_back({0, item.topic, 0});
        last = item.topic;
        shown.push_back(item);
    }
    // Counted as the section's bar counts: a dial's row stands for values that have rows of their own.
    const auto changed = static_cast<std::size_t>(std::ranges::count_if(p.quick_all, [](const Page::QuickItem &item) { return item.kind != 1; }));
    const auto cheats = cheat_lines(model);
    const auto subtitle = p.quick_all.empty() && cheats.empty()
        ? std::string("Nothing is changed")
        : std::format("{} changed{}", changed + cheats.size(), hidden ? std::format(", {} hidden", hidden) : std::string{});
    if (card(menu, p, "quick", "QUICK SETTINGS", subtitle.c_str())) {
        cheat_rows(menu, callbacks, cheats);
        if (p.quick_all.empty() && !cheats.empty()) {
            // Only cheats and addons are on: their rows above are the whole list.
        } else if (p.quick_all.empty()) {
            note("Everything you change shows up here, whatever section it is in: pick a preset above, move a dial or set a value below. Change it again here, hide the "
                 "rows you are done with, or press Go to for where a row lives and what is around it.");
        } else {
            const auto go = [&](const Page::QuickItem &item) {
                p.scroll_to = item.topic;
                p.open_cards = item.topic;
                p.flash.clear();
                if (item.kind == 3) {
                    const auto &row = view.rows[item.index];
                    p.flash = row.id;
                    p.flash_until = ImGui::GetTime() + 4.0;
                    p.flash_scroll = true;
                    if (!item.topic) {
                        // EVERY VALUE: its group, so the values beside it are the related ones.
                        p.only_changed = false;
                        const auto found = std::ranges::find(view.groups, row.group);
                        p.group = found == view.groups.end() ? 1 : static_cast<int>(found - view.groups.begin()) + 2;
                    }
                }
            };
            if (hidden) {
                if (ImGui::SmallButton(std::format("Show the {} hidden", hidden).c_str()))
                    for (const auto &item : p.quick_all)
                        if (const auto id = id_of(item); fold_open(id, false)) hide(id, false);
                tip("Brings back every row you hid.");
                ImGui::SameLine();
            }
            if (!shown.empty()) {
                if (ImGui::SmallButton("Hide all")) {
                    for (const auto &item : p.quick_all)
                        if (const auto id = id_of(item); !fold_open(id, false)) hide(id, true);
                }
                tip("Hides every row here. Nothing is changed by hiding: the rows stay set, and each comes back by itself the next time it changes from the game's own.");
                ImGui::SameLine();
            }
            ImGui::TextDisabled("Hide: off this list, still set.  Go to: where it lives.  Reset: back to the game's own.");
            const float hide_width = ImGui::CalcTextSize("Hide").x + ImGui::GetStyle().FramePadding.x * 2, go_width = ImGui::CalcTextSize("Go to").x + ImGui::GetStyle().FramePadding.x * 2;
            p.in_quick = true;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()), ImGui::GetFrameHeightWithSpacing());
            while (clipper.Step())
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const auto &item = shown[static_cast<std::size_t>(n)];
                    ImGui::PushID(n);
                    if (item.kind == 0) {
                        const auto &topic = trainer::topics[item.topic];
                        ImGui::AlignTextToFramePadding();
                        ImGui::PushFont(menu.bold);
                        if (item.topic) ImGui::TextUnformatted(topic.title.data(), topic.title.data() + topic.title.size());
                        else ImGui::TextUnformatted("OTHER VALUES");
                        ImGui::PopFont();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Open section")) {
                            p.scroll_to = item.topic;
                            p.flash.clear();
                        }
                        tip("Opens this section and scrolls to it.");
                    } else {
                        if (ImGui::Button("Hide", ImVec2(hide_width, 0))) hide(id_of(item), true);
                        tip("Takes this row off the list. It stays set. It comes back by itself the next time it changes from the game's own, or with \"Show hidden\".");
                        ImGui::SameLine();
                        if (ImGui::Button("Go to", ImVec2(go_width, 0))) go(item);
                        tip("Opens the section this lives in and scrolls to it, with the related settings around it.");
                        ImGui::SameLine();
                        if (item.kind == 1) {
                            ImGui::BeginDisabled(!can_edit);
                            dial_row(menu, callbacks, p, view.presets[item.index]);
                            ImGui::EndDisabled();
                        } else if (item.kind == 2) {
                            setup_row(menu, callbacks, p, view, view.setups[item.index]);
                        } else {
                            value_row(menu, callbacks, p, view, view.rows[item.index], !item.topic, true);
                        }
                    }
                    ImGui::PopID();
                }
            p.in_quick = false;
            if (shown.empty()) note("Every changed row is hidden.");
        }
    }
    card_end(p);
}

// ---- the Tune tab: one page of sections ----------------------------------------------------------
// The bar that opens and closes one section; `summary` says what is changed inside.
bool topic_header(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, std::uint8_t topic, const std::string &summary, bool open_by_default) {
    const auto &one = trainer::topics[topic];
    const std::string id = "section-" + std::string(one.key);
    bool open = fold_open(id, open_by_default);
    if (p.scroll_to == topic) {
        if (!open) set_fold(id, open = true);
        ImGui::SetScrollHereY(0.0f);
        p.scroll_to = -1;
    }
    const auto at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight() + px(6), width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id.c_str());
    const bool pressed = ImGui::InvisibleButton("##section", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed) {
        set_fold(id, open = !open);
        trainer_command(menu, callbacks, std::format("fold {} {}", id, open ? 1 : 0));
    }
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), hovered ? skate_theme::tile_light : skate_theme::tile_grey);
    draw->AddRectFilled(at, ImVec2(at.x + px(4), at.y + height), skate_theme::blue);
    fold_arrow(ImVec2(at.x + px(10), at.y), height, open, IM_COL32(255, 255, 255, 255));
    const float text_y = at.y + (height - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(16) + ImGui::GetFontSize() * 0.9f, text_y));
    ImGui::PushFont(menu.bold);
    ImGui::TextUnformatted(one.title.data(), one.title.data() + one.title.size());
    ImGui::PopFont();
    const float summary_width = summary.empty() ? 0.0f : ImGui::CalcTextSize(summary.c_str()).x + px(20);
    ImGui::SameLine(0, px(12));
    if (ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(one.about.data(), one.about.data() + one.about.size()).x < at.x + width - summary_width)
        ImGui::TextDisabled("%.*s", static_cast<int>(one.about.size()), one.about.data());
    else ImGui::NewLine();
    if (!summary.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + width - summary_width + px(10), text_y));
        ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue);
        ImGui::TextUnformatted(summary.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
    ImGui::Dummy(ImVec2(0, px(open ? 8.0f : 4.0f)));
    return open;
}
// The built-in presets of one section: a dial each, then the ones that are plain switches.
void dials_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, std::uint8_t topic, bool can_edit) {
    const auto shown = [&](const trainer::PresetRow &preset) {
        return preset.builtin && preset.topic == topic && preset.name != "Stock" && !preset.name.starts_with("Skate 3") && !preset.name.starts_with("Transition:");
    };
    if (std::ranges::none_of(view.presets, shown)) return;
    const std::string id = "dials-" + std::string(trainer::topics[topic].key);
    if (card(menu, p, id.c_str(), "DIALS AND SWITCHES", "1 is the game's own. A dial moves every value its preset moves.")) {
        ImGui::BeginDisabled(!can_edit);
        for (const auto &preset : view.presets)
            if (shown(preset) && preset.dial) dial_row(menu, callbacks, p, preset);
        bool first = true;
        for (const auto &preset : view.presets) {
            if (!shown(preset) || preset.dial) continue;
            if (!first && ImGui::GetItemRectMax().x + ImGui::CalcTextSize(preset.name.c_str()).x + ImGui::GetStyle().FramePadding.x * 4 <
                              ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x)
                ImGui::SameLine();
            first = false;
            preset_switch(menu, callbacks, preset);
        }
        ImGui::EndDisabled();
    }
    card_end(p);
}
// A section's named values that no other card of it shows, one at a time.
void fine_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, std::uint8_t topic) {
    const auto &rows = p.topic_rows[topic];
    if (std::ranges::none_of(rows, [&](std::size_t i) { return row_works(view.rows[i]); })) return;
    const std::string id = "fine-" + std::string(trainer::topics[topic].key);
    if (card(menu, p, id.c_str(), "FINE TUNING", "One value at a time. Blue = changed. The box on the left locks a value.")) {
        for (const auto i : rows)
            if (row_works(view.rows[i])) value_row(menu, callbacks, p, view, view.rows[i], false, true);
    }
    card_end(p);
}
// The whole table: every tuning value the trainer knows, by the game's own groups.
void every_value(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    // The list includes the game's tuning classes, which the trainer has to find in memory
    // before an edit to one does anything: with the list open, it looks now.
    trainer::note_class_list_shown();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
    if (p.group < 1) p.group = 1;
    const char *current = p.group > 1 && static_cast<std::size_t>(p.group) <= view.groups.size() + 1
        ? view.groups[static_cast<std::size_t>(p.group) - 2].c_str() : "Every group";
    if (ImGui::BeginCombo("##group", current)) {
        if (ImGui::Selectable("Every group", p.group == 1)) p.group = 1;
        for (std::size_t i = 0; i < view.groups.size(); ++i)
            if (ImGui::Selectable(view.groups[i].c_str(), p.group == static_cast<int>(i) + 2)) p.group = static_cast<int>(i) + 2;
        ImGui::EndCombo();
    }
    tip("Shows one group of tuning values at a time.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!view.editable || !callbacks.queue_console_command || !view.touched);
    if (ImGui::Button("Reset values")) {
        clear_edits(p);
        trainer_command(menu, callbacks, "reset all");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapped_tooltip("Every tuning value back to the game's own. Locked values stay, and so do the trick, grind and transition settings.");
    ImGui::EndDisabled();
    ImGui::Checkbox("Only what I changed", &p.only_changed);
    tip("Lists only the values that differ from the game's own.");
    ImGui::SameLine();
    ImGui::Checkbox("Graph points", &p.graph_points);
    if (ImGui::IsItemHovered()) wrapped_tooltip("Show every point of the tuning graphs, not just one multiplier per graph.");
    ImGui::SameLine();
    ImGui::Checkbox("Values with no use found", &p.show_unused);
    if (ImGui::IsItemHovered())
        wrapped_tooltip("Also list the tuning values that the game was never found or seen reading.\nChanging those will probably do nothing.");
    filter_rows(p, view);
    ImGui::SameLine();
    if (p.hidden_unused) ImGui::TextDisabled("%zu shown, %zu changed, %zu hidden", p.shown.size(), view.touched, p.hidden_unused);
    else ImGui::TextDisabled("%zu shown, %zu changed", p.shown.size(), view.touched);
    const bool with_group = p.group <= 1 || p.only_changed;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(p.shown.size()));
    if (p.flash_scroll && !p.flash.empty())
        for (std::size_t i = 0; i < p.shown.size(); ++i)
            if (view.rows[p.shown[i]].id == p.flash) clipper.IncludeItemByIndex(static_cast<int>(i));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            value_row(menu, callbacks, p, view, view.rows[p.shown[static_cast<std::size_t>(i)]], with_group, false);
    if (p.shown.empty()) note("Nothing matches.");
}
// What one section holds. Cards that live in the two long functions are picked out of them by id.
void topic_body(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, std::uint8_t topic, bool can_edit) {
    const auto from_trickline = [&](std::initializer_list<std::string_view> ids) {
        p.pick.assign(ids);
        trickline_section(menu, model, callbacks, p, view);
        p.pick.clear();
    };
    const auto from_transition = [&](std::initializer_list<std::string_view> ids) {
        p.pick.assign(ids);
        transition_section(menu, callbacks, p, view);
        p.pick.clear();
    };
    switch (topic) {
    case trainer::topic_game:
        presets_card(menu, callbacks, p, view, can_edit);
        quick_card(menu, model, callbacks, p, view, can_edit);
        return; // its rows are the other sections'
    case trainer::topic_pop:
        dials_card(menu, callbacks, p, view, topic, can_edit);
        ImGui::BeginDisabled(!can_edit);
        trick_heights(menu, callbacks, p, view, std::string{}, 1);
        ImGui::EndDisabled();
        from_trickline({"trick-pops", "trick-spins"});
        break;
    case trainer::topic_grinds: {
        // Grind lock-on is the game's own Grind Assist: the same number as its Settings menu, changed from either place. Never
        // MODIFIED, in no preset or reset, and the player's own in any session (the owner's ask, 2026-10-24).
        ImGui::PushID("grind-assist");
        const char *help = "Your game's own Grind Assist, the same number as in its Settings menu: change it here or there. How much the game "
                           "helps the board onto a rail or ledge. It is your setting, so it never counts as MODIFIED and no preset or reset "
                           "changes it. The lock-on distance, the reach from the air and the lock-on speeds and angle are the game's tuning "
                           "values, in the rows of this section.";
        field(menu, "Grind lock-on (GAME SETTING: Grind Assist)", help);
        if (view.grind_assist < 0) {
            ImGui::TextDisabled("not saved yet: set it once in the game's Settings");
        } else {
            double assist = view.grind_assist;
            ImGui::BeginDisabled(!callbacks.queue_console_command);
            if (slider_double("##assist", &assist, 0.0, 100.0, "%.0f")) trainer_command(menu, callbacks, std::format("grind assist {:.0f}", assist));
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapped_tooltip("%s", help);
        ImGui::PopID();
        from_trickline({"grind-feel", "grind-stick", "grind-exits"});
        dials_card(menu, callbacks, p, view, topic, can_edit);
        break;
    }
    case trainer::topic_trickline:
        from_trickline({"trickline", "trick-revert"});
        // Dark Pop is ReSkate's own switch (not a tuning value), so it is live even where the tuning is locked.
        dark_pop_controls(menu, model, callbacks);
        from_trickline({"trick-manuals"});
        dials_card(menu, callbacks, p, view, topic, can_edit);
        break;
    case trainer::topic_transition:
        from_transition({"transition-feel", "transition-coping", "transition-reentry", "transition-plant", "transition-lip", "transition-vert"});
        break;
    case trainer::topic_pumping:
        from_transition({"transition-pump"});
        break;
    case trainer::topic_foot:
        dials_card(menu, callbacks, p, view, topic, can_edit);
        ImGui::BeginDisabled(!can_edit);
        trick_heights(menu, callbacks, p, view, std::string{}, 2);
        ImGui::EndDisabled();
        break;
    default:
        dials_card(menu, callbacks, p, view, topic, can_edit);
        break;
    }
    fine_card(menu, callbacks, p, view, topic);
}
void tune_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    if (!view.ready) {
        begin_card(menu, "tune-wait", "PHYSICS TUNING");
        note(view.status.c_str());
        note("The values appear once a level is loaded.");
        end_card();
        return;
    }
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
    constexpr std::uint8_t order[]{trainer::topic_game, trainer::topic_pop, trainer::topic_speed, trainer::topic_bails, trainer::topic_grinds, trainer::topic_trickline,
                                   trainer::topic_transition, trainer::topic_pumping, trainer::topic_foot, trainer::topic_none};
    // One search for the whole tab: dials, switches, trick sliders and every value.
    {
        const float open_all = ImGui::CalcTextSize("Open all").x + ImGui::GetStyle().FramePadding.x * 2, fold_all = ImGui::CalcTextSize("Fold all").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - open_all - fold_all - ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##search", "Search: dials, tricks and every value...", p.search.data(), p.search.size());
        tip("Type a word to find dials, trick sliders and tuning values in every section at once. Clear the box to go back to the sections.");
        const auto all = [&](bool open) {
            for (const auto topic : order) {
                const std::string id = "section-" + std::string(trainer::topics[topic].key);
                set_fold(id, open);
                trainer_command(menu, callbacks, std::format("fold {} {}", id, open ? 1 : 0));
            }
        };
        ImGui::SameLine();
        if (ImGui::Button("Open all", ImVec2(open_all, 0))) all(true);
        tip("Opens every section.");
        ImGui::SameLine();
        if (ImGui::Button("Fold all", ImVec2(fold_all, 0))) all(false);
        tip("Closes every section. A click on a section's bar opens it; a click on a card's title folds that card.");
    }
    const auto words = lower(p.search.data());
    place_rows(p, view);
    if (!words.empty()) {
        // Searching looks through everything at once, whatever section it is in.
        const auto listed = [&](const trainer::PresetRow &preset) {
            return preset.builtin && preset.name != "Stock" && !preset.name.starts_with("Skate 3") && !preset.name.starts_with("Transition:") &&
                   contains_words(lower(preset.title + " " + preset.name + " " + preset.note), words);
        };
        if (std::ranges::any_of(view.presets, listed)) {
            begin_card(menu, "dials-found", "DIALS AND SWITCHES", "1 is the game's own. A dial moves every value its preset moves.");
            ImGui::BeginDisabled(!can_edit);
            for (const auto &preset : view.presets)
                if (listed(preset) && preset.dial) dial_row(menu, callbacks, p, preset);
            bool first = true;
            for (const auto &preset : view.presets) {
                if (!listed(preset) || preset.dial) continue;
                if (!first) ImGui::SameLine();
                first = false;
                preset_switch(menu, callbacks, preset);
            }
            ImGui::EndDisabled();
            end_card();
        }
        ImGui::BeginDisabled(!can_edit);
        trick_heights(menu, callbacks, p, view, words, 0);
        ImGui::EndDisabled();
        trainer::note_class_list_shown();
        filter_rows(p, view);
        ImGui::TextDisabled("%zu values match", p.shown.size());
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(p.shown.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                value_row(menu, callbacks, p, view, view.rows[p.shown[static_cast<std::size_t>(i)]], true, false);
        if (p.shown.empty()) note("No value matches. Grind, transition and bending settings are in their sections: clear the box to see them.");
        return;
    }
    if (!p.flash.empty() && ImGui::GetTime() > p.flash_until) p.flash.clear();
    p.flash_y = -1;
    for (const auto topic : order) {
        const auto &one = trainer::topics[topic];
        const std::size_t all = view.touched + view.setups_changed;
        const std::size_t count = topic == trainer::topic_game ? all : topic ? view.changed[topic] : view.touched;
        const auto summary = !count ? std::string{} : topic == trainer::topic_game ? std::format("{} changed in all", count) : topic ? std::format("{} changed", count)
                                                                                                                           : std::format("{} values changed in all", count);
        p.current_topic = topic;
        // A section with something changed in it starts open, so nothing that is on can hide. The first one is the way in.
        if (!topic_header(menu, callbacks, p, topic, summary, topic == trainer::topic_game || (topic != trainer::topic_none && count > 0))) continue;
        ImGui::Indent(px(10));
        ImGui::PushID(static_cast<int>(topic));
        if (!topic) {
            every_value(menu, callbacks, p, view);
        } else {
            topic_body(menu, model, callbacks, p, view, topic, can_edit);
            if (topic != trainer::topic_game) {
                ImGui::BeginDisabled(!can_edit || !view.changed[topic]);
                if (ImGui::SmallButton(std::format("Reset {}", std::string(one.title)).c_str())) {
                    clear_edits(p);
                    trainer_command(menu, callbacks, std::format("reset section {}", one.key));
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    wrapped_tooltip("%s", "Puts everything this section shows back to the game's own: its values, dials and settings. Locked values stay. "
                                          "Nothing in the other sections is touched.");
            }
        }
        ImGui::PopID();
        ImGui::Unindent(px(10));
        ImGui::Dummy(ImVec2(0, px(6)));
    }
    p.current_topic = p.open_cards = -1;
    // "Go to" a value: once it has been drawn, the page scrolls to it (it is inside a card, which does not scroll).
    if (p.flash_scroll && p.flash_y >= 0) {
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() + p.flash_y - ImGui::GetWindowPos().y - ImGui::GetWindowHeight() * 0.3f));
        p.flash_scroll = false;
    }
}

// The trick sliders. `part` 0: every one that matches the search words; 1: the TRICKS card of POP AND TRICKS; 2: the
// jump on foot (ON FOOT). Pump power is on PUMPING and the board bending boost on TRICKLINING.
void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const std::string &words, int part) {
    static constexpr const char *advanced_label = "Advanced trick speed (a speed for each flip trick: kickflip, heelflip, 360 flip, varial, shuvit, hardflip, laser flip, impossible)";
    static constexpr const char *labels[]{"Gravity", "Flip trick speed", "Underflip speed", "Late flip speed", advanced_label, "Pump power (pumping transitions)", "Manual pop height", "No comply height", "Boneless height", "Hippy jump height", "Off-board jump height",
                                           "Board bending boost (revert speed)"};
    const auto wanted = [&](const char *label) {
        const std::string_view name(label);
        if (part == 2) return name == "Off-board jump height";
        if (part == 1) return name != "Off-board jump height" && !name.starts_with("Pump power") && !name.starts_with("Board bending");
        return contains_words(lower(std::string(label) + " tricks"), words);
    };
    if (std::ranges::none_of(labels, wanted)) return;
    if (card(menu, p, part == 2 ? "trick-foot" : part == 1 ? "trick-heights" : "trick-found", part == 2 ? "JUMP" : "TRICKS", "1.0 is the game's own")) {
    // `limited`: the game itself stops at the slider's end, so a typed number is held to it too.
    const auto slider = [&](const char *label, const char *option, float value, float &edit, bool &editing, const char *help, float high = 50.0f, bool limited = false, bool always = false) {
        if (!always && !wanted(label)) return false;
        // A value that is not the game's own (1) is marked, like a moved dial.
        const auto shown_label = std::abs(value - 1.0f) > 1e-4f ? std::string(label) + "  *" : std::string(label);
        field(menu, shown_label.c_str(), help);
        ImGui::PushID(option);
        if (console_style()) {
            float shown = editing ? edit : value;
            if (slider_float("##height", &shown, 0.1f, high, "x %.2f", (limited ? ImGuiSliderFlags_AlwaysClamp : 0) | ImGuiSliderFlags_Logarithmic)) {
                edit = shown;
                editing = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option {} {:.2f}", option, edit));
            if (!ImGui::IsItemActive() && editing && std::abs(value - edit) < 0.005f * std::max(1.0f, std::abs(edit))) editing = false;
            ImGui::PopID();
            return true;
        }
        float shown = editing ? edit : value;
        const float box = px(64);
        ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::SliderFloat("##height", &shown, 0.1f, high, "x %.2f", (limited ? ImGuiSliderFlags_AlwaysClamp : 0) | ImGuiSliderFlags_Logarithmic)) {
            edit = shown;
            editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option {} {:.2f}", option, edit));
        if (!ImGui::IsItemActive() && editing && std::abs(value - edit) < 0.005f * std::max(1.0f, std::abs(edit))) editing = false;
        if (!ImGui::IsItemActive()) tip(help);
        ImGui::SameLine();
        float typed = value;
        ImGui::SetNextItemWidth(box);
        if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && typed > 0) {
            editing = false;
            trainer_command(menu, callbacks, std::format("option {} {}", option, typed));
        }
        if (ImGui::IsItemHovered())
            wrapped_tooltip(limited ? "Type a multiplier and press Enter." : "Type any multiplier and press Enter: the slider's end is not a limit.");
        ImGui::PopID();
        return true;
    };
    const bool gravity_shown = slider("Gravity", "gravity", view.gravity, p.gravity_edit, p.gravity_editing,
               "Gravity in the air, on the board and on foot. 1 is the game's own.\n\n"
               "At 0.5 the same pop goes twice as high and hangs twice as long; at 2 you drop twice as fast.\n\n"
               "It acts in the air only: rolling down a hill, grinds and a bailed skater are the game's own. The game aims an air off a "
               "ramp back at the ramp with its own gravity, so with another gravity a vert air can come down somewhere else.\n\n"
               "It takes hold at once. Advanced gravity below times the tricks and landings for it.",
               3.0f);
    if (gravity_shown && view.gravity != 1.0f && !view.gravity_live)
        ImGui::TextDisabled("On foot this game build keeps its own gravity: the setting acts on the board only.");
    if (gravity_shown && ImGui::TreeNodeEx("Advanced gravity", ImGuiTreeNodeFlags_SpanAvailWidth |
                                           (view.gravity_landing_own || view.gravity_timing != 1.0f ? ImGuiTreeNodeFlags_DefaultOpen : 0))) {
        // Animation timing: 0 .. 1, not a multiplier.
        field(menu, view.gravity_timing != 1.0f ? "Animation timing  *" : "Animation timing",
              "How the skater's tricks and landings are timed in the air when Gravity is not 1.\n\n"
              "1 (the default): the skater's animations play faster by just as much as the flight is shorter (slower when it is "
              "longer), so tricks, catches and landings look as they do at the game's own gravity. Flips turn faster with it.\n\n"
              "0: the game's own timing. It fits a trick into the time to land but cannot hurry a catch, so with more gravity a "
              "catch can still be playing when the wheels touch and the landing stutters.\n\n"
              "In between: part of the way.");
        ImGui::PushID("gravity_timing");
        float timing = p.gravity_timing_editing ? p.gravity_timing_edit : view.gravity_timing;
        const bool moved = console_style() ? slider_float("##timing", &timing, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)
                                           : ImGui::SliderFloat("##timing", &timing, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (moved) {
            p.gravity_timing_edit = timing;
            p.gravity_timing_editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option gravity_timing {:.2f}", p.gravity_timing_edit));
        if (!ImGui::IsItemActive() && p.gravity_timing_editing && std::abs(view.gravity_timing - p.gravity_timing_edit) < 0.005f)
            p.gravity_timing_editing = false;
        ImGui::PopID();
        bool own = view.gravity_landing_own;
        if (ImGui::Checkbox("Landing gravity of its own", &own))
            trainer_command(menu, callbacks, std::format("option gravity_landing_own {}", own ? 1 : 0));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "The game plans every landing (where you come down, when, and the time its animations count down to) "
                                  "with a gravity.\n\n"
                                  "Off: with Gravity, so the plan matches the flight.\n\n"
                                  "On: with Landing gravity below. A plan with less gravity than the flight lands late (the board goes "
                                  "into the ground first); with more it reaches for the ground early.");
        if (own)
            slider("Landing gravity", "gravity_landing", view.gravity_landing, p.gravity_landing_edit, p.gravity_landing_editing,
                   "The gravity the game plans landings with, as a multiple of its own.\n\n"
                   "Set it to Gravity for landings that match the flight.",
                   3.0f, false, true);
        ImGui::TreePop();
    }
    slider("Manual pop height", "manual_height", view.manual_height, p.manual_edit, p.manual_editing,
           "How high you pop out of a manual or a nose manual. 1 is the game's own (lower than a riding ollie). Ollie height raises both.");
    slider("Flip trick speed", "flip_speed", view.flip_speed, p.flip_edit, p.flip_editing,
           "How fast the board turns in every flip trick. It does not change how high you pop. Below 1 flips are slower, above 1 faster. A slow "
           "flip is slow from the start and is not hurried round before you land: with too little air you land on a board that is still turning. "
           "It takes about 15 seconds to take hold after every level load: the trainer has to find the game's flip animations again first.",
           3.0f);
    if (slider("Underflip speed", "underflip_speed", view.underflip_speed, p.underflip_edit, p.underflip_editing,
               "How fast the board turns in an underflip (the kickflip, heelflip, shuvit and impossible underflips, the hospital and flower "
               "flips). You flick and trigger the underflip as always. 1 is the game's own. By itself it sets the whole underflip, its flip "
               "too, and Flip trick speed does not reach it; with 'Underflip part only' below the flip turns at Flip trick speed and this "
               "sets the part where the board turns back.",
               3.0f)) {
        bool split = view.underflip_split;
        if (ImGui::Checkbox("Underflip part only (the flip at Flip trick speed)", &split))
            trainer_command(menu, callbacks, std::format("option underflip_split {}", split ? 1 : 0));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "Off: Underflip speed sets the whole underflip. On: the underflip's flip turns at Flip trick speed (and the "
                                  "trick's own speed under Advanced trick speed), and Underflip speed takes over where the board starts to "
                                  "turn back. The trainer sees that from the board itself, so it starts a tick or two after the turn back.");
    }
    {
        constexpr std::size_t late = trainer::late_flip_slot;
        slider("Late flip speed", "flip.lateflip", view.flip_trick[late], p.trick_edit[late], p.trick_editing[late],
               "How fast the board turns in a late flip (a flick after the pop, in the air), times the flip trick speed. 1 is the game's own. "
               "It counts with or without Advanced trick speed.",
               3.0f);
    }
    if (!view.flip_live && (view.flip_speed != 1.0f || view.flip_advanced || view.flip_flick) && wanted("Flip trick speed"))
        ImGui::TextDisabled("Finding the flip animations: flip speeds take hold about 15 seconds after a level loads.");
    if (wanted("Flip trick speed") || wanted("Slow flips pop higher") || wanted("Flick tricks at full speed") || wanted("Flip landing limit")) {
        bool pop = view.flip_pop;
        if (ImGui::Checkbox("Slow flips pop higher, so they land clean", &pop)) trainer_command(menu, callbacks, std::format("option flip_pop {}", pop ? 1 : 0));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "A slowed flip needs more time than a normal pop gives: without this the game holds the landing back and the "
                                  "skater sinks before riding away. With it every pop into a slowed flip trick (riding, out of a manual, out of a "
                                  "grind or slide) goes at least as high as the trick needs: about 0.9 m at x 0.5 and 2.9 m at x 0.25, where the "
                                  "game's own pop is 0.4 m. Ollies, nollies and boned ollies are slowed and popped the same way while this is on "
                                  "(without it the flip trick speed leaves them alone).");
        bool flick = view.flip_flick;
        if (ImGui::Checkbox("Flick tricks at full speed", &flick)) trainer_command(menu, callbacks, std::format("option flip_flick {}", flick ? 1 : 0));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "The game turns the board slower the slower the stick was flicked, so tricks with a long stick motion (front foot "
                                  "and nollie impossibles, 360 pop shuvits) turn slower than a kickflip at the same flip trick speed and land "
                                  "half way round. With this every flip trick turns as if the flick was a full one: with 'Slow flips pop higher' "
                                  "they all get the same pop and all come round in it. It works at every flip trick speed. With it on a slow "
                                  "flick no longer gives a slow flip.");
        bool limit = view.flip_limit;
        if (ImGui::Checkbox("Flip landing limit", &limit)) trainer_command(menu, callbacks, std::format("option flip_limit {}", limit ? 1 : 0));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "The game's own rule: a flip trick still turning just before the landing (about 1/6 s) is hurried "
                                  "round so it lands bolts. Shuvits and 360 shuvits lean on it most, since they turn slowly. Ticked is the "
                                  "game's own. Untick it to let slowed flips stay slow right to the ground: a trick that is not round "
                                  "by then lands as it is, so pair it with 'Slow flips pop higher'. Unticked counts as modified.");
    }
    if (wanted(advanced_label)) {
        bool on = view.flip_advanced;
        if (ImGui::Checkbox("Advanced trick speed", &on)) trainer_command(menu, callbacks, std::format("option flip_advanced {}", on ? 1 : 0));
        tip("Gives every flip trick a speed of its own, on top of the flip trick speed above: a kickflip at 1.0 and a 360 flip at 0.6, say. "
            "A trick turns at the speed above times its own. Nollie tricks use their regular trick's speed (the nollie impossibles have their own). Each underflip has a speed of its own at the bottom, times Underflip speed above. The ollie's speed only acts "
            "with 'Slow flips pop higher' on. Pop height is not changed. "
            "Unticked, the speeds below are kept but not used. Like the speed above, they take hold about 15 seconds after every level load.");
        if (view.flip_advanced_blocked) ImGui::TextDisabled("Off in this multiplayer game: the host's rules apply here.");
        if (on && ImGui::TreeNodeEx("Speed of each flip trick", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen)) {
            for (std::size_t i = 0; i < trainer::late_flip_slot; ++i) { // the Late flip has its slider above
                const auto &trick = trainer::flip_tricks[i];
                const std::string label(trick.label), option(trick.key);
                slider(label.c_str(), option.c_str(), view.flip_trick[i], p.trick_edit[i], p.trick_editing[i],
                       "How fast the board turns in this trick, times the flip trick speed above. 1 leaves it at that speed.", 3.0f, false, true);
            }
            ImGui::SeparatorText("Underflips");
            for (std::size_t i = trainer::first_underflip_slot; i < trainer::flip_tricks.size(); ++i) {
                const auto &trick = trainer::flip_tricks[i];
                const std::string label(trick.label), option(trick.key);
                slider(label.c_str(), option.c_str(), view.flip_trick[i], p.trick_edit[i], p.trick_editing[i],
                       "How fast the board turns in this underflip, times Underflip speed above (with 'Underflip part only', the part where "
                       "the board turns back). Nollie underflips use their regular trick's speed, but for the nollie impossibles. 1 leaves "
                       "it at Underflip speed.",
                       3.0f, false, true);
            }
            ImGui::SeparatorText("Late flips");
            slider("Late flip", "flip.lateflip", view.flip_trick[trainer::late_flip_slot], p.trick_edit[trainer::late_flip_slot],
                   p.trick_editing[trainer::late_flip_slot],
                   "How fast the board turns in a late flip, times the flip trick speed above: the same setting as Late flip speed above. "
                   "The game tells every late flip apart only as 'a late flip', so they share this one speed.",
                   3.0f, false, true);
            ImGui::TreePop();
        }
    }
    slider("No comply height", "nocomply_height", view.nocomply_height, p.nocomply_edit, p.nocomply_editing, "How high a no comply pops. 1 is the game's own.");
    slider("Boneless height", "boneless_height", view.boneless_height, p.boneless_edit, p.boneless_editing, "How high a boneless pops. 1 is the game's own.");
    slider("Hippy jump height", "hippy_height", view.hippy_height, p.hippy_edit, p.hippy_editing, "How high you jump off the board in a hippy jump. 1 is the game's own.");
    slider("Off-board jump height", "offboard_height", view.offboard_height, p.offboard_edit, p.offboard_editing, "How high you jump on foot. 1 is the game's own.");
    if (slider("Pump power (pumping transitions)", "pump_power", view.pump_power, p.pump_edit, p.pump_editing,
               "How much speed pumping a transition gives. 1 is the game's own; 2 gives about one more of the game's pumps on top of each "
               "of yours, 10 about nine more, and under 1 takes speed away. The game still decides when you are pumping: the extra comes "
               "for as long as it counts you as pumping. It is found in the game about 15 seconds after a level loads.")) {
        if (view.pump_blocked) ImGui::TextDisabled("Off in this multiplayer game: the host's rules apply here.");
        else if (view.pump_power != 1.0f && !view.pump_live) ImGui::TextDisabled("Finding the game's pumping in memory: it acts about 15 seconds after a level loads.");
    }
    if (wanted("Board bending boost (revert speed)")) {
        // Not a multiplier: 0 is the game's own (no boost).
        field(menu, "Board bending boost");
        ImGui::PushID("revert_boost");
        float shown = p.revert_editing ? p.revert_edit : view.revert_boost;
        const float box = px(64);
        ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::SliderFloat("##strength", &shown, 0.0f, 5.0f, shown <= 0.0f ? "off" : "x %.2f")) {
            p.revert_edit = shown;
            p.revert_editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option revert_boost {:.2f}", p.revert_edit));
        if (!ImGui::IsItemActive() && p.revert_editing && std::abs(view.revert_boost - p.revert_edit) < 0.006f) p.revert_editing = false;
        if (ImGui::IsItemHovered())
            wrapped_tooltip("Board bending: land with the board turned out of line and the game snaps it straight. With this on, that snap\n"
                              "throws you forward, as it did in Skate 3: chain bends to keep and build speed without pushing.\n"
                              "x 1 gives about +1.5 to +2.6 m/s a landing. 0 is off, the game's own. TRICKLINING has the details.\n"
                              "From AutoRevertBoost by Sivaes, jaq and OVM.");
        ImGui::SameLine();
        float typed = view.revert_boost;
        ImGui::SetNextItemWidth(box);
        if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && typed >= 0) {
            p.revert_editing = false;
            trainer_command(menu, callbacks, std::format("option revert_boost {}", typed));
        }
        if (ImGui::IsItemHovered()) wrapped_tooltip("Type any strength and press Enter: the slider's end is not a limit.");
        ImGui::PopID();
    }
    if (part == 1) {
        if (ImGui::Button("Reset tricks")) {
            clear_edits(p);
            trainer_command(menu, callbacks, "reset tricks");
        }
        tip("Puts every slider and switch on this card back to the game's own.");
    }
    note("Not tuning values: the game sets these in its trick scripts and flip curves. Type in the box to go past a slider's end.");
    }
    card_end(p);
}

void practice_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    if (card(menu, p, "speed", "GAME SPEED", "Slow motion for learning a line")) {
    {
        const NamedSettingModel *setting = nullptr;
        for (const auto &row : model.engine_settings)
            if (row.name == "SimulationTime.TimeScale") {
                setting = &row;
                break;
            }
        if (!setting || !setting->available) {
            note(setting && !setting->reason.empty() ? setting->reason.c_str() : "Game speed is unavailable right now.");
            // The engine resolves a setting the first time it is asked for: ask.
            if (setting && callbacks.queue_console_command && ImGui::GetTime() >= p.speed_until) {
                p.speed_until = ImGui::GetTime() + 2.0;
                p.speed_edit = -1;
                std::array<char, 256> ignored{};
                callbacks.queue_console_command(callbacks.user, "reset SimulationTime.TimeScale", ignored.data(), ignored.size());
            }
        } else {
            double current = 1;
            try {
                const auto first = setting->value.find_first_of("-.0123456789");
                if (first != std::string::npos) current = std::stod(setting->value.substr(first));
            } catch (...) {}
            if (p.speed_edit >= 0 && ImGui::GetTime() < p.speed_until) current = p.speed_edit;
            const auto set = [&](double speed) {
                p.speed_edit = speed;
                p.speed_until = ImGui::GetTime() + 1.0;
                send_console(menu, callbacks, speed == 1.0 ? "reset SimulationTime.TimeScale" : "SimulationTime.TimeScale " + number(speed));
            };
            field(menu, "Speed", "How fast the whole game runs: 1x is normal, 0.5x half speed. Type a number for anything past the slider.");
            auto value = static_cast<float>(current);
            if (::dingosdk::overlay::menu::slider_float("##game-speed", &value, 0.05f, 2.0f, current == 0.0 ? "paused" : "%.2fx") && value >= 0)
                set(value);
            const int pressed = button_row(menu, "speeds", {{current == 0.0 ? "RESUME" : "PAUSE", true, current == 0.0}, {"0.1x", true, current == 0.1},
                {"0.25x", true, current == 0.25}, {"0.5x", true, current == 0.5}, {"0.75x", true, current == 0.75}, {"1x", true, current == 1.0}});
            constexpr std::array<double, 5> quick{0.1, 0.25, 0.5, 0.75, 1.0};
            if (pressed == 0) set(current == 0.0 ? 1.0 : 0.0);
            else if (pressed > 0) set(quick[static_cast<std::size_t>(pressed - 1)]);
            note("The jump read-out measures real time, so it reads low while the game is slowed.");
        }
    }
    }
    card_end(p);

    if (card(menu, p, "markers", "MARKERS", "Saved for each map")) {
    int slot = view.slot;
    if (choice(menu, "marker-slot", slot, {"1", "2", "3", "4", "5"})) trainer_command(menu, callbacks, std::format("slot {}", slot + 1));
    const auto &marker = view.markers[static_cast<std::size_t>(std::clamp(view.slot, 0, static_cast<int>(trainer::marker_slots) - 1))];
    info(menu, "Selected", marker.set ? std::format("{:.1f}, {:.1f}, {:.1f}", marker.position[0], marker.position[1], marker.position[2]) : "empty");
    {
        const bool map = !view.map.empty();
        const int pressed = button_row(menu, "marker-actions", {{"SAVE HERE", map && telemetry.skater, false, "Put the selected marker where you are."},
            {"GO", map && marker.set, false, "Go to the selected marker."}, {"CLEAR", map && marker.set, false, "Empty the selected marker."}});
        if (pressed == 0) trainer_command(menu, callbacks, "marker save");
        if (pressed == 1) trainer_command(menu, callbacks, "marker go");
        if (pressed == 2) trainer_command(menu, callbacks, "marker clear");
    }
    bool auto_return = view.auto_return;
    if (toggle_row(menu, "Return after a bail", "Go back to the selected marker when the skater wipes out.", auto_return))
        trainer_command(menu, callbacks, std::format("option auto_return {}", auto_return ? 1 : 0));
    field(menu, "Return delay", "How long after a bail before you are put back at the marker, in seconds.");
    float delay = view.return_delay;
    ::dingosdk::overlay::menu::slider_float("##return-delay", &delay, 0.0f, 10.0f, "%.1f s");
    if (!ImGui::IsItemActive()) tip("How long after a bail before you are put back at the marker. Type a number for longer than the slider.");
    if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option return_delay {:.2f}", std::max(0.0f, delay)));
    bool pad = view.pad_shortcuts;
    if (toggle_row(menu, "Controller shortcuts", "Hold LB + RB, then D-pad: up saves, down goes, left / right pick the slot.", pad))
        trainer_command(menu, callbacks, std::format("option pad {}", pad ? 1 : 0));
    }
    card_end(p);

    if (card(menu, p, "teleport", "TELEPORT")) {
    // The waypoint line is why the map's registry is read at all: only while this is drawn.
    trainer::note_teleport_card_shown();
    info(menu, "Map waypoint", view.waypoint_set ? std::format("{:.1f}, {:.1f}, {:.1f}", view.waypoint[0], view.waypoint[1], view.waypoint[2])
                                                 : "none - place one on the pause map");
    if (button_row(menu, "waypoint", {{"GO TO MAP WAYPOINT", view.waypoint_set && telemetry.skater, false, "The waypoint you placed on the pause map."}}) == 0)
        trainer_command(menu, callbacks, "waypoint");
    if (telemetry.skater)
        info(menu, "You are at", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
    if (console_style()) {
        // Typing a position wants a keyboard: full screen keeps one, set from where you are.
        info(menu, "Saved spot", std::format("{:.2f}, {:.2f}, {:.2f}", p.teleport[0], p.teleport[1], p.teleport[2]));
        const int pressed = button_row(menu, "teleport-actions", {{"SET IT TO HERE", telemetry.skater, false, "Keep where you are now."},
            {"GO THERE", telemetry.skater, false, "Teleport to the saved spot."}});
        if (pressed == 0) p.teleport = telemetry.position;
        if (pressed == 1) trainer_command(menu, callbacks, std::format("tp {:.3f} {:.3f} {:.3f}", p.teleport[0], p.teleport[1], p.teleport[2]));
        end_card();
        return;
    }
    field(menu, "X, Y, Z");
    ImGui::InputFloat3("##teleport", p.teleport.data(), "%.2f");
    tip("A position in the game's own coordinates: X, height, Z.");
    ImGui::BeginDisabled(!telemetry.skater);
    if (ImGui::Button("Here")) p.teleport = telemetry.position;
    tip("Fills the boxes with where you are now.");
    ImGui::SameLine();
    if (ImGui::Button("Go##teleport"))
        trainer_command(menu, callbacks, std::format("tp {:.3f} {:.3f} {:.3f}", p.teleport[0], p.teleport[1], p.teleport[2]));
        tip("Moves you to the position in the boxes. You arrive on foot.");
    ImGui::SameLine();
    if (ImGui::Button("Copy position"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]).c_str());
        tip("Copies where you are to the clipboard as X, Y, Z.");
    ImGui::SameLine();
    if (ImGui::Button("Copy for Blender"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], -telemetry.position[2], telemetry.position[1]).c_str());
        tip("Copies where you are in Blender's axes (Z up), for map makers.");
    ImGui::EndDisabled();
    }
    card_end(p);
}

void jump_lines(SkateMenu &menu, const trainer::Jump &jump) {
    info(menu, "Takeoff", std::format("{:.1f} km/h at {:.1f} degrees", jump.takeoff_speed * 3.6f, jump.takeoff_angle));
    info(menu, "Air time", std::format("{:.2f} s", jump.air_time));
    info(menu, "Height", std::format("{:.2f} m", jump.height));
    info(menu, "Distance", std::format("{:.2f} m, {:.2f} m drop", jump.distance, jump.drop));
    info(menu, "Landing", std::format("{:.1f} km/h at {:.1f}, {:.1f}, {:.1f}", jump.landing_speed * 3.6f, jump.landing[0], jump.landing[1],
                                      jump.landing[2]));
    info(menu, "Rotation", std::format("spin {:.0f} degrees (peak {:.0f} per second), flip {:.0f} degrees", jump.spin, jump.spin_rate, jump.flip));
}
// Which game the skating plays like: the game's own, or Skate 3 on one of its three difficulties.
// One is always lit; a Skate 3 one goes out again as soon as one of its values is changed by hand.
// Everything that makes a trick line, in one place: the same values and sliders as the Tune tab,
// gathered, with one button for Skate 3's and one for the game's own.
void trickline_section(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
    ImGui::BeginDisabled(!can_edit);
    if (card(menu, p, "trickline", "TRICKLINING EXTRAS", "What Skate 3's tricklining had beyond its tuning")) {
    bool extras = view.revert_boost > 0;
    if (toggle_row(menu, "Skate 3's tricklining extras",
            "The board bending boost below at x 1, heavier revert and powerslide friction (x 3 and x 1.5) and higher footplants (no comply and "
            "boneless height x 1.5). First guesses: tune each of them below.", extras))
        trainer_command(menu, callbacks, extras ? "trickline extras on" : "trickline extras off");
    note("Turning it off puts back only what still holds these numbers: a value you have since moved stays yours. Blue = changed; Reset puts one value "
         "back. Type in a box to go past a slider's end.");
    }
    card_end(p);
    if (card(menu, p, "grind-feel", "GRINDS FEEL LIKE", "The game's own, or the way Skate 3 rode a grind")) {
    {
        {
            // The two feels: every switch the game's own, or skate.'s own ways off a grind held off.
            bool like_skate = true, like_skate3 = true;
            for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
                if (view.grind_mode[i] >= 0) like_skate = false;
                if (view.grind_mode[i] != trainer::skate3_grind_mode(i)) like_skate3 = false;
            }
            for (std::size_t i = 0; i < trainer::grind_settings.size(); ++i) {
                if (view.grind_setting[i] >= 0) like_skate = false;
                if (view.grind_setting[i] != trainer::grind_settings[i].skate3) like_skate3 = false;
            }
            if (view.grind_push != 0.0f) like_skate = false;
            if (std::abs(view.grind_push - 1.8f) > 0.005f) like_skate3 = false;
            if (view.grind_range != 0.0f) like_skate = false;
            if (view.grind_range == 0.0f) like_skate3 = false; // any range of your own keeps it Skate 3
            field(menu, "Grinds feel like");
            if (ImGui::RadioButton("skate.", like_skate)) trainer_command(menu, callbacks, "grind feel skate");
            tip("Every grind setting and switch is left to the game.");
            ImGui::SameLine();
            if (ImGui::RadioButton("Skate 3", like_skate3)) trainer_command(menu, callbacks, "grind feel skate3");
            tip("Turns off what skate. added to leaving a grind: leaning off without a pop, the sideways ollie, "
                "exits onto another grind, double grinds, stronger and camera-relative exits, linear drive. The left stick alone turns the board on a "
                "one-truck grind, the right stick tips it (5-0, nose grind) and pops, and a pop goes sideways the way the left stick is held. Change any "
                "row on the two cards below afterwards to make your own mix.");
            note("Skate 3 feel holds five of your game's own grind settings for as long as it is on. Your own settings are not changed: the game's "
                 "Settings menu keeps showing yours, and they are back the moment you pick skate. here. The left stick's turn needs an Xbox-style "
                 "(XInput) controller.");
        }
    }
    }
    card_end(p);
    if (card(menu, p, "grind-stick", "STICK AND POPS ON A GRIND", "How far the left stick turns the board, and which way a pop goes")) {
    {
        field(menu, "Pop: side force");
        float side = p.grind_side_editing ? p.grind_side_edit : view.grind_side;
        const float side_box = px(64);
        ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - side_box - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::SliderFloat("##grind-side", &side, 0.0f, 10.0f, "x %.2f")) {
            p.grind_side_edit = side;
            p.grind_side_editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("grind side {:.2f}", p.grind_side_edit));
        if (!ImGui::IsItemActive() && p.grind_side_editing && std::abs(view.grind_side - p.grind_side_edit) < 0.006f) p.grind_side_editing = false;
        if (!ImGui::IsItemActive())
            tip("Multiplies the part of a grind or slide pop that goes sideways off the edge, measured against the way you were travelling on it. "
                "1 is the game's own, 0 pops straight along the edge. Type any number in the box: the slider's ends are not a limit.");
        ImGui::SameLine();
        float side_typed = view.grind_side;
        ImGui::SetNextItemWidth(side_box);
        if (ImGui::InputFloat("##grind-side-typed", &side_typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && side_typed >= 0) {
            p.grind_side_editing = false;
            trainer_command(menu, callbacks, std::format("grind side {}", side_typed));
        }
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", "Type any multiplier and press Enter: the slider's end is not a limit.");
        {
            field(menu, "Pop: push with stick");
            float push = p.grind_push_editing ? p.grind_push_edit : view.grind_push;
            const float push_box = px(64);
            ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - push_box - ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::SliderFloat("##grind-push", &push, -5.0f, 5.0f, "%.2f m/s")) {
                p.grind_push_edit = push;
                p.grind_push_editing = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("grind push {:.2f}", p.grind_push_edit));
            if (!ImGui::IsItemActive() && p.grind_push_editing && std::abs(view.grind_push - p.grind_push_edit) < 0.006f) p.grind_push_editing = false;
            if (!ImGui::IsItemActive())
                tip("Skate 3's rule: a pop off a grind goes sideways the way the left stick is held, by this many m/s at full stick. Skate 3's own number "
                    "is 1.8; 0 is off. A negative number pushes the other way. Type any number in the box.");
            ImGui::SameLine();
            float push_typed = view.grind_push;
            ImGui::SetNextItemWidth(push_box);
            if (ImGui::InputFloat("##grind-push-typed", &push_typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) {
                p.grind_push_editing = false;
                trainer_command(menu, callbacks, std::format("grind push {}", push_typed));
            }
            if (ImGui::IsItemHovered()) wrapped_tooltip("%s", "Type any speed and press Enter: the slider's ends are not a limit.");
        }
        {
            // How far the stick turns the board on a grind: slider, and a box for any number.
            struct Speed { const char *label, *key, *help; float view; float *edit; bool *editing; float low{0}, high{10}; const char *zero{"game's own"}; };
            const Speed speeds[]{
                {"Left stick turn range", "range",
                 "Skate 3's turn on a grind: the left stick alone turns the board, with Skate 3's own dead zone, curve and easing, up to this many degrees "
                 "at the end of the stick. The right stick's sideways push no longer turns it; its up and down still tips the board (5-0, nose grind) and "
                 "pops. 0 is off: skate.'s own turn, which follows where the right stick points. A negative number turns the other way.", view.grind_range,
                 &p.grind_range_edit, &p.grind_range_editing, -180.0f, 180.0f, "off"},
            };
            for (const auto &one : speeds) {
                field(menu, one.label, one.help);
                ImGui::PushID(one.key);
                float value = *one.editing ? *one.edit : one.view;
                const float box = px(64);
                ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - ImGui::GetStyle().ItemSpacing.x));
                if (ImGui::SliderFloat("##speed", &value, one.low, one.high, value == 0.0f ? one.zero : "%.2f")) {
                    *one.edit = value;
                    *one.editing = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("grind {} {:.2f}", one.key, *one.edit));
                if (!ImGui::IsItemActive() && *one.editing && std::abs(one.view - *one.edit) < 0.006f) *one.editing = false;
                if (!ImGui::IsItemActive()) tip(one.help);
                ImGui::SameLine();
                float typed = one.view;
                ImGui::SetNextItemWidth(box);
                if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    *one.editing = false;
                    trainer_command(menu, callbacks, std::format("grind {} {}", one.key, std::max(one.low, typed)));
                }
                if (ImGui::IsItemHovered()) wrapped_tooltip("%s", "Type any speed and press Enter: the slider's end is not a limit.");
                ImGui::PopID();
            }
        }
    }
    }
    card_end(p);
    if (card(menu, p, "grind-exits", "GRIND EXITS AND TRANSFERS", "Your game's own grind settings and switches, each left to the game or held off or on")) {
    {
        if (!p.grind_watching) {
            trainer_command(menu, callbacks, "grind watch 1");
            p.grind_watching = true;
        }
        note("GAME SETTINGS: the first six rows are settings of your own game (its Settings menu has them). Ticking one here holds it on or off for as long "
             "as the trainer says so; your own setting is not changed and comes back with \"Game's own\", RESET EVERYTHING or leaving the game.");
        // The game's own settings first: they change which grind code runs.
        for (std::size_t i = 0; i < trainer::grind_settings.size(); ++i) {
            const auto &one = trainer::grind_settings[i];
            const std::string label(one.label), help = std::string(one.help) + " (The game's setting " + std::string(one.setting) + ", " +
                                                       (one.stock ? "on" : "off") + " by default.) Tick or untick to hold it there while the trainer runs; your own setting is not changed, and Game's own gives it back.";
            field(menu, label.c_str());
            ImGui::PushID(static_cast<int>(100 + i));
            const bool forced = view.grind_setting[i] >= 0;
            bool on = forced ? view.grind_setting[i] != 0 : view.grind_own[i] != 0;
            if (ImGui::Checkbox("##on", &on)) trainer_command(menu, callbacks, std::format("grind {} {}", one.key, on ? "on" : "off"));
            tip(help.c_str());
            ImGui::SameLine();
            if (forced) {
                ImGui::TextUnformatted(view.grind_setting[i] ? "held on" : "held off");
                ImGui::SameLine();
                ImGui::TextDisabled("(yours: %s)", view.grind_own[i] ? "on" : "off");
                ImGui::SameLine();
                if (ImGui::SmallButton("Game's own")) trainer_command(menu, callbacks, std::format("grind {} game", one.key));
            } else {
                ImGui::TextDisabled("%s", view.grind_own[i] ? "your game setting: on" : "your game setting: off");
            }
            ImGui::PopID();
        }
        for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
            const auto &one = trainer::grind_switches[i];
            const std::string label(one.label), help = std::string(one.help) + " (" + std::string(one.state) + ".) Game's own leaves it to the game; Off and On hold it there.";
            field(menu, label.c_str());
            ImGui::PushID(static_cast<int>(i));
            if (one.read_only) {
                ImGui::TextDisabled("%s", view.grind_live[i] < 0 ? "not in use yet" : view.grind_live[i] ? "happening now" : "not now");
                tip(help.c_str());
                ImGui::PopID();
                continue;
            }
            // A tick box: what is in force now (held, or the game's own). A click holds it the other way.
            const bool held = view.grind_mode[i] >= 0;
            bool on = held ? view.grind_mode[i] != 0 : view.grind_live[i] > 0;
            if (ImGui::Checkbox("##on", &on)) trainer_command(menu, callbacks, std::format("grind {} {}", one.key, on ? "on" : "off"));
            tip(help.c_str());
            ImGui::SameLine();
            if (held) {
                ImGui::TextUnformatted(view.grind_mode[i] ? "held on" : "held off");
                ImGui::SameLine();
                if (ImGui::SmallButton("Game's own")) trainer_command(menu, callbacks, std::format("grind {} game", one.key));
            } else {
                ImGui::TextDisabled("%s", view.grind_live[i] < 0 ? "game's own (not in use yet)" : view.grind_live[i] ? "game's own: on" : "game's own: off");
            }
            ImGui::PopID();
        }
        note("Tick or untick a switch to hold it there; Game's own gives it back to the game. These are the game's names for its own switches: what each one "
             "changes in play has not been measured yet. A switch has no value until the game first uses it (most appear "
             "once you are on the board, the rest on the first grind), and they take hold about 15 seconds after a level loads.");
    }
    }
    card_end(p);

    // What each group of rows is for in a line, said once at the top of its card.
    static constexpr const char *group_cards[][3]{
        {"", "", ""},
        {"trick-pops", "POPS",
         "A line lives on pops you can count on. Max is a held pop, min a quick flick; bring them together and every pop is the same height. The stick "
         "boost is the speed you can add by pushing the stick as you pop. Pops out of grinds are on GRINDS."},
        {"trick-spins", "SPINS AND FLIPS",
         "How fast you and the board turn, and how long a flip may take to come round (its catch time). Slower spins and longer catch times "
         "read as style; faster ones fit more into a small gap."},
        {"trick-manuals", "MANUALS AND STEERING",
         "Manual balance (the three balance numbers are how hard the game corrects you), how far the board may tip, and how the board steers and settles."},
    };
    const auto rows = [&](std::size_t g) {
        for (const auto i : p.trick_rows[g])
            if (i < view.rows.size() && row_works(view.rows[i])) value_row(menu, callbacks, p, view, view.rows[i], false, true);
    };

    if (card(menu, p, "trick-revert", "BOARD BENDING, REVERTS AND POWERSLIDES", "Speed out of a bent landing")) {
    {
        // The revert speed boost: its strength, then what counts and what it is worth.
        const auto number_row = [&](const char *label, const char *command, float value, float low, float high, const char *format, const char *tip) {
            ImGui::PushID(command);
            field(menu, label, tip);
            const auto key = std::string("revert:") + command;
            float shown = p.active == key ? static_cast<float>(p.active_value) : value;
            ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - px(64) - ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::SliderFloat("##value", &shown, low, high, format)) trainer_command(menu, callbacks, std::format("{} {:.3f}", command, shown));
            if (ImGui::IsItemActive()) {
                p.active = key;
                p.active_value = shown;
            } else if (p.active == key) {
                p.active.clear();
            }
            if (ImGui::IsItemHovered()) wrapped_tooltip("%s", tip);
            ImGui::SameLine();
            float typed = value;
            ImGui::SetNextItemWidth(px(64));
            if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) trainer_command(menu, callbacks, std::format("{} {}", command, typed));
            if (ImGui::IsItemHovered()) wrapped_tooltip("%s", "Type any number and press Enter: the slider's ends are not a limit.");
            ImGui::PopID();
        };
        const auto &r = view.revert;
        note("Board bending is how tricklining keeps its speed. Land a spin with the board twisted off your body "
             "and the game snaps the board straight. In Skate 3 that snap threw you forward, so "
             "liners bend the board on every landing instead of pushing. skate. gives nothing for it; this boost puts the speed back.");
        number_row("Board bending boost", "option revert_boost", view.revert_boost, 0.0f, 5.0f, view.revert_boost <= 0 ? "off" : "x %.2f",
                   "How hard a bend throws you forward. 0 is off, the game's own; x 1 is about +2 m/s (7 km/h) a landing.\nFrom AutoRevertBoost by Sivaes, jaq and OVM.");
        ImGui::Spacing();
        if (ImGui::TreeNodeEx("What counts as a bend, and what it is worth", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        number_row("Spin needed", "revert spin", r.min_spin, 0.0f, 180.0f, "%.0f deg", "How far you must have turned in the air. Lower it and smaller turns count as bends.");
        number_row("Board off your body", "revert twist", r.min_twist, 0.0f, 90.0f, "%.0f deg",
                   "The rule that makes a bend: the board must land twisted at least this far off your body. Clean and short 180s land 0 to 3 degrees off, "
                   "auto reverts and board bends 70 to 90.");
        number_row("or off the travel", "revert slip", r.min_slip, 0.0f, 90.0f, r.min_slip >= 90.0f ? "off" : "%.0f deg",
                   "Off at 90, as shipped. Lower it and a landing also counts when you and the board together land this far out of line with where you are "
                   "going. Careful: the game straightens ordinary 180s that way too, so a low number gives speed on plain 180s and flip trick 180s. A "
                   "landing that counts by this rule alone gets the auto revert boost at most.");
        number_row("Boost: a small bend", "revert bend", r.bend_boost, 0.0f, 10.0f, "+%.1f m/s", "Speed added when the board turned 40 degrees past the body over the flight (times the boost strength).");
        number_row("Boost: a full bend", "revert full", r.full_boost, 0.0f, 10.0f, "+%.1f m/s", "The same at 360 degrees; in between it rises in a straight line up to 140 degrees.");
        number_row("Boost: an auto revert", "revert auto", r.auto_boost, 0.0f, 10.0f, "+%.1f m/s", "Speed added when the board turned more than 140 degrees past the body.");
        number_row("No boost above", "revert max_speed", r.max_speed, 5.0f, 100.0f, "%.0f m/s", "Reverts stop adding speed here (1 m/s = 3.6 km/h).");
        number_row("Time between boosts", "revert cooldown", r.cooldown, 0.0f, 3.0f, "%.2f s", "How soon the next revert can boost again.");
        number_row("Shortest flight", "revert min_air", r.min_air, 0.0f, 2.0f, "%.2f s", "A hop shorter than this never boosts.");
        if (ImGui::Button("Reset these rules")) trainer_command(menu, callbacks, "revert reset");
        tip("Puts the rules above back to the trainer's defaults.");
        ImGui::TreePop();
        }
        ImGui::Spacing();
    }
    note("Friction: how much speed a revert or a powerslide scrubs off (higher loses more). Skate 3's were heavier, which is why bending "
         "mattered there. Forward force: a push the game adds while you revert or slide.");
    rows(0);
    }
    card_end(p);
    for (std::size_t g = 1; g < std::size(group_cards); ++g) {
        if (std::ranges::none_of(p.trick_rows[g], [&](std::size_t i) { return i < view.rows.size() && row_works(view.rows[i]); })) continue;
        if (card(menu, p, group_cards[g][0], group_cards[g][1])) {
            note(group_cards[g][2]);
            rows(g);
        }
        card_end(p);
    }
    ImGui::EndDisabled();
}


// Transition skating in one place: bowls, quarter pipes, vert and spines. The feel (the game's own or Skate 3's), the
// trainer's own help back into the ramp, the game's own settings and every tuning value that concerns a lip.
void transition_section(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
    ImGui::BeginDisabled(!can_edit);
    // A slider with a box for any number beside it; `command` gets the number appended.
    const auto number_row = [&](const char *label, const char *command, float value, float low, float high, const char *format, const char *help) {
        ImGui::PushID(command);
        field(menu, label, help);
        const auto key = std::string("transition:") + command;
        float shown = p.active == key ? static_cast<float>(p.active_value) : value;
        ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - px(64) - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::SliderFloat("##value", &shown, low, high, format)) trainer_command(menu, callbacks, std::format("{} {:.3f}", command, shown));
        if (ImGui::IsItemActive()) {
            p.active = key;
            p.active_value = shown;
        } else if (p.active == key) {
            p.active.clear();
        }
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", help);
        ImGui::SameLine();
        float typed = value;
        ImGui::SetNextItemWidth(px(64));
        if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) trainer_command(menu, callbacks, std::format("{} {}", command, typed));
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", "Type any number and press Enter: the slider's ends are not a limit.");
        ImGui::PopID();
    };
    // The game's own settings shown on one card: a tick box each, forced or left to the game.
    const auto settings = [&](std::uint8_t card) {
        for (std::size_t i = 0; i < trainer::transition_settings.size(); ++i) {
            const auto &one = trainer::transition_settings[i];
            if (one.card != card) continue;
            const std::string label(one.label), help = std::string(one.help) + " (The game's setting " + std::string(one.setting) + ", " +
                                                       (one.stock ? "on" : "off") + " by default.) A setting of your own game: ticking it here holds it there while the trainer runs. Your own "
                                                       "setting is not changed, and Game's own gives it back.";
            field(menu, label.c_str());
            ImGui::PushID(static_cast<int>(200 + i));
            const bool forced = view.tr_setting[i] >= 0;
            bool on = forced ? view.tr_setting[i] != 0 : view.tr_own[i] != 0;
            if (ImGui::Checkbox("##on", &on)) trainer_command(menu, callbacks, std::format("transition {} {}", one.key, on ? "on" : "off"));
            tip(help.c_str());
            ImGui::SameLine();
            if (forced) {
                ImGui::TextUnformatted(view.tr_setting[i] ? "held on" : "held off");
                ImGui::SameLine();
                ImGui::TextDisabled("(yours: %s)", view.tr_own[i] ? "on" : "off");
                ImGui::SameLine();
                if (ImGui::SmallButton("Game's own")) trainer_command(menu, callbacks, std::format("transition {} game", one.key));
            } else {
                ImGui::TextDisabled("%s", view.tr_own[i] ? "your game setting: on" : "your game setting: off");
            }
            ImGui::PopID();
        }
    };
    // One of the grind card's switches, shown here too because it decides how a coping is ridden.
    const auto grind_switch = [&](std::string_view key, const char *more) {
        for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
            const auto &one = trainer::grind_switches[i];
            if (one.key != key) continue;
            const std::string label(one.label), help = std::string(one.help) + " " + more + " (" + std::string(one.state) + "; the same switch as on GRINDS.)";
            field(menu, label.c_str());
            ImGui::PushID(static_cast<int>(300 + i));
            const bool held = view.grind_mode[i] >= 0;
            bool on = held ? view.grind_mode[i] != 0 : view.grind_live[i] > 0;
            if (ImGui::Checkbox("##on", &on)) trainer_command(menu, callbacks, std::format("grind {} {}", one.key, on ? "on" : "off"));
            tip(help.c_str());
            ImGui::SameLine();
            if (held) {
                ImGui::TextUnformatted(view.grind_mode[i] ? "held on" : "held off");
                ImGui::SameLine();
                if (ImGui::SmallButton("Game's own")) trainer_command(menu, callbacks, std::format("grind {} game", one.key));
            } else {
                ImGui::TextDisabled("%s", view.grind_live[i] < 0 ? "game's own (not in use yet)" : view.grind_live[i] ? "game's own: on" : "game's own: off");
            }
            ImGui::PopID();
        }
    };
    // Only rows that do something: a value the game was never found reading is left out (it is still on EVERY VALUE),
    // unless it has been changed, so it can be put back.
    const auto rows = [&](std::size_t g, std::size_t first = 0, std::size_t count = ~std::size_t{0}) {
        for (std::size_t n = first; n < p.transition_rows[g].size() && n - first < count; ++n)
            if (const auto i = p.transition_rows[g][n]; i < view.rows.size() && (view.rows[i].used || view.rows[i].touched || view.rows[i].frozen))
                value_row(menu, callbacks, p, view, view.rows[i], false, true);
    };
    const auto setting_forced_on = [&](std::string_view key) {
        for (std::size_t i = 0; i < trainer::transition_settings.size(); ++i)
            if (trainer::transition_settings[i].key == key) return view.tr_setting[i] == 1;
        return false;
    };

    if (card(menu, p, "transition-feel", "TRANSITION FEELS LIKE", "Bowls, quarter pipes, vert and spines: the game's own, or the way Skate 3 rode them")) {
    {
        const bool preset_on = std::ranges::any_of(view.presets, [](const trainer::PresetRow &row) { return row.builtin && row.active && row.name == "Transition: Skate 3"; });
        bool like_skate = !preset_on, like_skate3 = preset_on;
        for (std::size_t i = 0; i < trainer::transition_settings.size(); ++i) {
            if (view.tr_setting[i] >= 0) like_skate = false;
            if (view.tr_setting[i] != trainer::transition_settings[i].skate3) like_skate3 = false;
        }
        if (view.tr_lean != 0.0f || view.tr_carry != 0.0f || view.tr_unstick != 0.0f || view.tr_lockout != 0.0f || view.tr_spine || view.tr_plant != 0.0f) like_skate = false;
        // Any number of your own on a help that is on keeps it Skate 3.
        if (view.tr_lean == 0.0f || view.tr_carry == 0.0f || view.tr_unstick == 0.0f || view.tr_lockout == 0.0f || view.tr_plant == 0.0f) like_skate3 = false;
        field(menu, "Transition feels like");
        if (ImGui::RadioButton("skate.", like_skate)) trainer_command(menu, callbacks, "transition feel skate");
        tip("Everything on this list is left to the game.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Skate 3", like_skate3)) trainer_command(menu, callbacks, "transition feel skate3");
        tip("Turns on every help below at Skate 3's numbers: lean into the ramp on a coping grind, roll off the coping at the angle you came in, no hang-ups coming "
            "back in, handplants that come back in high on the wall. Sets Skate 3's coping exit timers and handplant exit. Airs stay skate.'s own, which is "
            "already Skate 3's rule (stick forward flies out onto the deck, anything else comes back in), and so does pumping (use Pump power and Pump assist). "
            "Change any row afterwards to make your own mix.");
        note("New in this version, and built from Skate 3's own rules before any of it was measured in skate.: tell us what each help does on your ramps. "
             "With the jump read-out on (MAP & HUD) every coping grind, handplant and hang-up writes a line starting \"Trainer transition:\" to the log. "
             "Grinds have their own choice of feel on GRINDS. The lean needs an Xbox-style (XInput) controller.");
    }
    }
    card_end(p);

    if (card(menu, p, "transition-coping", "COPING GRINDS AND STALLS", "On the lip of a bowl, a pipe or a quarter")) {
    number_row("Lean into the ramp", "transition lean", view.tr_lean, -1.0f, 1.0f, view.tr_lean == 0.0f ? "off" : "%.2f",
               "skate. stands the skater straight up on top of the coping. With this the skater leans into the ramp, the more the faster the grind runs along the "
               "coping (nothing below 1 m/s, all of it from 5 m/s). It works by showing the game your left stick pushed toward the ramp for as long as you leave that "
               "stick alone; move the stick and it is yours. 1 is as far as the game's lean goes without taking you off the grind; Skate 3 feel uses 0.6. "
               "A negative number leans the other way, should it go out over the deck.");
    number_row("Roll off at your angle", "transition carry", view.tr_carry, 0.0f, 1.0f, view.tr_carry == 0.0f ? "off" : "x %.2f",
               "skate. turns the board straight down the ramp when a coping grind ends without a pop. With this the speed you had along the coping is put back "
               "for the first third of a second, so a fast grind rolls off at a shallow angle, as it did in Skate 3. 1 keeps all of it, 0 is off.");
    number_row("Roll-off drop speed", "transition drop", view.tr_drop, 0.0f, 5.0f, view.tr_drop == 0.0f ? "no limit" : "%.2f m/s",
               "While the speed along the coping is being kept (the row above), the speed across the coping into the ramp is held to this. Skate 3 nudged the "
               "board in at 1 m/s at most. 0 is no limit. Does nothing with the row above off.");
    settings(trainer::transition_card_coping);
    grind_switch("coping", "On, as the game has it, riding straight up into a coping does not lock onto it the old way.");
    grind_switch("linear", "Off, the game's older hold on the edge runs: the one the coping exit timers below belong to.");
    note("The exit timers: in Skate 3 a coping grind ended by itself. After the time set here the skater leaned further and further into the ramp and rolled "
         "in. skate. has them off (-1). Skate 3: 0.1 s riding straight up into the coping, 0.8 s coming at it from below, 2 s on a curb.");
    rows(0);
    }
    card_end(p);

    if (card(menu, p, "transition-reentry", "RE-ENTRY AND HANG-UPS", "Coming back in off the coping, and over a spine")) {
    number_row("No hang-ups", "transition unstick", view.tr_unstick, 0.0f, 5.0f, view.tr_unstick == 0.0f ? "off" : "x %.2f",
               "Skate 3's rule for a hang-up: slower than 1.1 m/s for longer than a sixth of a second after leaving the coping (or after landing an air off a "
               "transition without riding away). The skater is then pushed into the ramp and down until they are moving again. The number is how hard; 1 is "
               "the first setting, 0 is off.");
    number_row("Coping re-catch delay", "transition lockout", view.tr_lockout, 0.0f, 3.0f, view.tr_lockout == 0.0f ? "off" : "%.2f s",
               "After a coping grind ends without a pop the skater does not reach for an edge for this long (the reach rows below are held at 0, then put back). "
               "Skate 3 waited 70 frames, 1.17 s. 0 is off.");
    {
        field(menu, "Airs clear the coping");
        bool spine = view.tr_spine;
        if (ImGui::Checkbox("##spine", &spine)) trainer_command(menu, callbacks, spine ? "transition spine on" : "transition spine off");
        tip("An air that leaves a transition going up and over the lip (little of its speed runs along the lip) does not reach for an edge until it has "
            "landed: the far coping of a spine, or the coping it left, cannot catch the board. A carve along the lip into a coping grind is left alone. "
            "With this on, an air straight up cannot be landed back on the coping as a grind either.");
    }
    note("How far the skater reaches for an edge from the air, and what an edge is worth against the ground as a place to land:");
    rows(1);
    }
    card_end(p);

    if (card(menu, p, "transition-plant", "HANDPLANTS", "Where a plant lets you go")) {
    number_row("Skate 3 plant exit", "transition plant", view.tr_plant, 0.0f, 1.0f, view.tr_plant == 0.0f ? "off" : "x %.2f",
               "When a handplant ends the skater leaves the lip at 0.9 m/s or more into the ramp and starts falling from rest, as off the top of an arc, in place of "
               "the downward speed the plant leaves them with. 1 is all the way, 0 is off. If plants still come down at the bottom of the ramp with this on, "
               "the plant itself carries the skater down before it lets go: the log line says how far (\"lost inside the plant\").");
    settings(trainer::transition_card_plant);
    note("The game's own numbers for a plant. It looks for a wall to come down on in a fan of directions from along the coping (0) to straight in (90), and "
         "leaves the lip with a speed between the two limits. Skate 3: fan to 69 degrees, 0.9 m/s out, up to 2 m/s along the coping.");
    rows(2);
    }
    card_end(p);

    if (card(menu, p, "transition-lip", "LIP TRICKS AND DROP-INS", "Stalls on the lip, rolling in from the deck")) {
    settings(trainer::transition_card_lip);
    rows(3);
    }
    card_end(p);

    if (card(menu, p, "transition-vert", "VERT AIRS AND LANDINGS", "The pop off a wall, and where the game brings you down")) {
    settings(trainer::transition_card_vert);
    note("A pop off a steep wall is straightened to go up the wall, tipped in or out by the stick; in the air the game picks a landing from a fan of "
         "directions by score. These are the numbers for both.");
    rows(4);
    }
    card_end(p);

    if (card(menu, p, "transition-pump", "PUMP POWER AND GAME SETTINGS", "How much a pump gives, and how much of it you have to do")) {
    number_row("Pump power", "option pump_power", view.pump_power, 0.0f, 10.0f, "x %.2f",
               "How much speed pumping a transition gives. 1 is the game's own; 2 gives about one more of the game's pumps on top of each of yours, 10 about "
               "nine more, and under 1 takes speed away. The game still decides when you are pumping: the extra comes for as long as it counts you as "
               "pumping. It is found in the game about 15 seconds after a level loads.");
    if (view.pump_blocked) ImGui::TextDisabled("Off in this multiplayer game: the host's rules apply here.");
    else if (view.pump_power != 1.0f && !view.pump_live) ImGui::TextDisabled("Finding the game's pumping in memory: it acts about 15 seconds after a level loads.");
    note("GAME SETTINGS: the next three rows are your game's own settings, shown here as the game holds them. The trainer never changes them: "
         "change them in the game's own Settings menu. No preset carries them, no reset touches them and they never count as MODIFIED.");
    // The game's own options: the same values its Settings menu holds, changed from either place.
    ImGui::EndDisabled(); // these are the player's own game settings: theirs to change in any session
    for (std::size_t i = 0; i < trainer::transition_options.size(); ++i) {
        const auto &one = trainer::transition_options[i];
        const std::string label(one.label), help = std::string(one.help) + " GAME SETTING (" + std::string(one.setting) +
                                                   "): this is your game's own setting, shown as the game holds it. The trainer does not change it: use "
                                                   "the game's Settings menu. It is not part of presets, resets or the STOCK badge.";
        ImGui::PushID(static_cast<int>(400 + i));
        field(menu, label.c_str());
        // Shown only: the trainer never writes the player's own game settings.
        if (one.boolean) ImGui::TextUnformatted(view.tr_option[i] != 0 ? "On" : "Off");
        else ImGui::Text("%d", view.tr_option[i]);
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", help.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "GAME SETTING");
        if (ImGui::IsItemHovered()) wrapped_tooltip("%s", help.c_str());
        ImGui::PopID();
    }
    ImGui::BeginDisabled(!can_edit);
    settings(trainer::transition_card_pump);
    // The first seven rows are the older pumping code's numbers: skate.'s newer pumping does not read them, so they
    // are only shown while Legacy pumping is forced on (or to put back one that was changed).
    if (setting_forced_on("legacypump")) {
        note("Legacy pumping is on: these are its numbers. Skate 3: strength 18, limit 10, 0.7 of a pump given without pumping.");
        rows(5, 0, 7);
    } else {
        note("Pump power above works with the game's own pumping. Tick Legacy pumping for Skate 3's pumping and its strength and limit rows: "
             "skate.'s newer pumping does not read those numbers, so they are not shown until then.");
        for (std::size_t n = 0; n < 7 && n < p.transition_rows[5].size(); ++n)
            if (const auto i = p.transition_rows[5][n]; i < view.rows.size() && (view.rows[i].touched || view.rows[i].frozen))
                value_row(menu, callbacks, p, view, view.rows[i], false, true);
    }
    rows(5, 7);
    }
    card_end(p);
    ImGui::EndDisabled();
}

void map_tab(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    if (card(menu, p, "hud", "HUD")) {
    bool hud = view.hud, jump = view.hud_jump, logging = view.logging;
    if (toggle_row(menu, "Speed and air HUD", "Speed, air time and height in the corner while you skate.", hud))
        trainer_command(menu, callbacks, std::format("option hud {}", hud ? 1 : 0));
    if (toggle_row(menu, "Jump read-out", "After each landing: takeoff speed and angle, height, distance, landing speed.", jump))
        trainer_command(menu, callbacks, std::format("option hud_jump {}", jump ? 1 : 0));
    if (toggle_row(menu, "Record telemetry", "Write position and speed every frame to a CSV in %LOCALAPPDATA%\\ReSkate\\trainer\\telemetry.",
            logging))
        trainer_command(menu, callbacks, std::format("option log {}", logging ? 1 : 0));
    }
    card_end(p);

    if (card(menu, p, "now", "RIGHT NOW")) {
    if (!telemetry.skater) {
        note("No skater yet.");
    } else {
        info(menu, "Speed", std::format("{:.1f} km/h ({:.2f} m/s), top {:.1f} km/h", telemetry.speed * 3.6f, telemetry.speed, telemetry.top_speed * 3.6f));
        info(menu, "Position", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
        info(menu, "Heading", std::format("{:.0f} degrees", telemetry.heading));
    }
    }
    card_end(p);

    if (telemetry.last.serial) {
        if (card(menu, p, "last-jump", "LAST JUMP", std::format("Jump {}", telemetry.last.serial).c_str())) {
        jump_lines(menu, telemetry.last);
        }
    card_end(p);
    }
    if (telemetry.best.serial && telemetry.best.serial != telemetry.last.serial) {
        if (card(menu, p, "best-jump", "LONGEST JUMP ON THIS MAP")) {
        jump_lines(menu, telemetry.best);
        }
    card_end(p);
    }

    if (card(menu, p, "map", "THIS MAP", view.map.empty() ? "No level loaded" : view.map.c_str())) {
    if (!view.map_note.empty()) note(view.map_note.c_str());
    if (!view.map_preset.empty()) {
        info(menu, "Author's preset", view.map_preset);
        if (button_row(menu, "author-preset", {{"APPLY THE AUTHOR'S PRESET", view.editable}}) == 0) trainer_command(menu, callbacks, "preset apply map");
    }
    for (std::size_t i = 0; i < view.spots.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (console_style()) {
            if (button_row(menu, "spot", {{("GO TO " + view.spots[i].name).c_str()}}) == 0) trainer_command(menu, callbacks, std::format("spot {}", i + 1));
        } else {
            if (ImGui::Button("Go")) trainer_command(menu, callbacks, std::format("spot {}", i + 1));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(view.spots[i].name.c_str());
        }
        ImGui::PopID();
    }
    if (view.map_note.empty() && view.map_preset.empty() && view.spots.empty())
        note("Map makers can ship a trainer.json in their mod folder with spots and a recommended preset.");
    }
    card_end(p);

    note("Your presets are above the tabs now, beside RESET EVERYTHING. Play like Skate 3 is on TUNE, under WHOLE GAME.");
}
} // namespace

bool trainer_take_open() {
    const auto view = trainer::view();
    auto &p = page();
    if (view->open_serial == p.open_serial) return false;
    p.open_serial = view->open_serial;
    // By name, see `trainer open`: 0 tune, 1 presets, 2 practice, 3 map; the rest are sections of TUNE
    // (4 realistic and 5 fun were lists of earlier versions: the top of TUNE). 7 movement: noclip, No Bail
    // and the boosts are on Skater > Cheats.
    const int tab = view->open_tab;
    if (tab == 7) {
        p.show_cheats = true;
        return true;
    }
    p.tab = tab == 2 ? 1 : tab == 3 ? 2 : 0;
    if (tab == 1) p.manage = true;
    constexpr int sections[]{-1, -1, -1, -1, -1, -1, trainer::topic_none, -1, trainer::topic_trickline, trainer::topic_transition, trainer::topic_pop, trainer::topic_speed,
                             trainer::topic_bails, trainer::topic_grinds, trainer::topic_pumping, trainer::topic_foot, trainer::topic_trickline, trainer::topic_pop,
                             trainer::topic_game};
    if (tab >= 0 && tab < static_cast<int>(std::size(sections)) && sections[tab] >= 0) {
        p.scroll_to = sections[tab];
        p.search[0] = 0;
    }
    p.show_page = true;
    return true;
}
bool trainer_page_wanted() {
    auto &p = page();
    const bool wanted = p.show_page;
    p.show_page = false;
    return wanted;
}
bool trainer_cheats_wanted() {
    auto &p = page();
    const bool wanted = p.show_cheats;
    p.show_cheats = false;
    return wanted;
}
// The game speed the PRACTICE tab sets (1: the game's own; also when it cannot be read).
double game_speed(const Model &model) {
    for (const auto &row : model.engine_settings)
        if (row.name == "SimulationTime.TimeScale" && row.available) {
            try {
                const auto first = row.value.find_first_of("-.0123456789");
                if (first != std::string::npos) return std::stod(row.value.substr(first));
            } catch (...) {}
        }
    return 1.0;
}
void trainer_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto view = trainer::view();
    // Which sections and cards are open is the player's, kept in the trainer's file.
    if (!p.folds_seeded && view->revision) {
        p.folds_seeded = true;
        for (const auto &[id, open] : view->folds) set_fold(id, open);
    }
    {
        std::string id;
        bool open{};
        while (take_fold_change(id, open)) trainer_command(menu, callbacks, std::format("fold {} {}", id, open ? 1 : 0));
    }
    // A session that sets everyone's physics: said in large type over everything else, so nobody tunes a game that
    // ignores it (the owner's ask, 2026-10-24).
    if (view->session_enforced || view->session_stock) {
        const ImVec4 amber(1.0f, 0.75f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.18f, 0.0f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_Border, amber);
        ImGui::BeginChild("server-physics", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        if (menu.heading) ImGui::PushFont(menu.heading);
        ImGui::TextColored(amber, "%s", view->session_stock ? "MATCH PHYSICS: THE GAME'S OWN" : "SERVER PHYSICS IN USE");
        if (menu.heading) ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(view->session_stock
                                   ? "This match skates on the game's own physics for everyone. Your trainer settings, presets and boosts do "
                                     "nothing until the match ends; they are kept for afterwards."
                                   : "This multiplayer game's host or server sets everyone's physics. Your trainer settings, presets and "
                                     "boosts do nothing here; you skate with theirs. Your own setup comes back when you leave.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }
    // Everything that changes how the game plays, said in words. The first parts are the trainer's own; the cheats,
    // grind exits, Board Break and the game speed are ReSkate's and count all the same.
    const double speed = game_speed(model);
    std::string what;
    const auto add = [&](const std::string &part) { what += (what.empty() ? "" : ", ") + part; };
    if (view->ready) {
        if (view->touched) add(std::format("{} value{}", view->touched, view->touched == 1 ? "" : "s"));
        static constexpr std::pair<std::uint8_t, const char *> kinds[]{{trainer::topic_pop, "trick settings"}, {trainer::topic_grinds, "grind settings"},
                                                                      {trainer::topic_trickline, "bending settings"}, {trainer::topic_transition, "transition settings"},
                                                                      {trainer::topic_pumping, "pumping settings"}, {trainer::topic_foot, "on-foot jump"}};
        for (const auto &[topic, name] : kinds)
            if (view->settings[topic]) add(name);
        // Named on its own: it changes the underflips the game scores, which "trick settings" does not say.
        if (std::abs(view->underflip_speed - 1.0f) > 1e-4f)
            add(std::format("underflip speed x{:.2f}{}", view->underflip_speed, view->underflip_split ? " (under part)" : ""));
        else if (view->underflip_split) add("underflip flip at flip trick speed");
    }
    const auto cheats = cheat_lines(model);
    for (const auto &line : cheats) add(line.name);
    if (std::abs(speed - 1.0) > 1e-3) add(std::format("game speed x{:.2f}", speed));
    // A guest under the host's physics would only wipe a setup they cannot see at work here.
    ImGui::BeginDisabled(!callbacks.queue_console_command || view->session_enforced);
    if (ImGui::Button("RESET EVERYTHING")) {
        clear_edits(p);
        trainer_command(menu, callbacks, "reset everything");
        // The cheats, grind exits, Board Break and the game speed change how the game plays too: off with the rest.
        for (const auto &line : cheats) cheat_off(menu, callbacks, line);
        if (callbacks.queue_debug && model.debug.probe) debug_request(menu, callbacks, {DebugAction::set_probe_enabled, false});
        if (std::abs(speed - 1.0) > 1e-3) send_console(menu, callbacks, "reset SimulationTime.TimeScale");
    }
    tip("Puts back every value, lock, preset, dial, trick, grind, transition and bending setting, turns No Bail, Noclip, Dark Pop, Revert Boost, "
        "grind exit triggers and Board Break off, puts the boosts back to 0 (off), sets the "
        "game speed to normal and stops loading a preset on this map: the game exactly as it shipped. Your saved presets are kept, and so are your "
        "game's own settings.");
    ImGui::EndDisabled();
    if (view->session_enforced && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        wrapped_tooltip("%s", "Off in this multiplayer game: the host controls physics here. Your own setup is kept for when you leave.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const auto locks = view->locks ? std::format("  ({} locked value{})", view->locks, view->locks == 1 ? "" : "s") : std::string{};
    if (view->session_enforced) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "HOST'S PHYSICS: your own setup is kept for when you leave.");
    else if (!view->ready && what.empty()) ImGui::TextDisabled("The game as it shipped: every value, lock, preset and setting.");
    else if (what.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "STOCK: the game is exactly as it shipped.%s", locks.c_str());
    else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "MODIFIED: %s.%s", what.c_str(), locks.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::IsItemHovered())
        wrapped_tooltip("%s", "STOCK means nothing here is changing how the game plays. Your game's own settings (the rows marked GAME SETTING) are yours and never "
                              "count; a locked value that is still the game's own does not count either. Open TUNE: every section that holds a change says so on its bar.");
    // The search for the game's tuning values reads all of the game's memory on a thread of its own: values set meanwhile
    // reach the game when it ends, and on a machine short of memory the game can stutter while it runs.
    if (const auto ms = trainer::class_search_ms()) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Finding the game's tuning values... %llu s", static_cast<unsigned long long>(ms / 1000));
        if (ImGui::IsItemHovered())
            wrapped_tooltip("%s", "The trainer is reading the game's memory for the numbers it changes (after a level loads, a respawn or a new "
                                  "setting). Changes you make meanwhile reach the game when it is done. It usually takes seconds; when the PC is "
                                  "short of free memory it can take minutes and the game may stutter until it ends.");
    }
    if (view->ready) preset_row(menu, callbacks, p, *view);
    // `preset export` leaves its line here; the clipboard belongs to this thread.
    if (!p.share_seen) {
        p.share_seen = true;
        p.share_serial = view->share_serial;
    } else if (p.share_serial != view->share_serial) {
        p.share_serial = view->share_serial;
        ImGui::SetClipboardText(view->share_text.c_str());
    }
    // Above the tabs, so every tab says why its controls do nothing in this session.
    if (view->session_enforced) {
        if (card(menu, p, "session-note", "MULTIPLAYER: THE HOST CONTROLS PHYSICS")) {
        warn("You are in a multiplayer game where the host controls physics. This menu will not work in this multiplayer game.");
        note("You skate with the host's setup. HUD and markers still work. Your own setup comes back when you leave.");
        }
    card_end(p);
    } else if (view->boosts_blocked) {
        if (card(menu, p, "session-note", "MULTIPLAYER: BOOSTS ARE OFF")) {
        warn("The host has turned boosts off in this multiplayer game: trick sliders, flip speed, auto push and the board bending boost will not work here.");
        note("The physics values still apply. The rest comes back when the host allows boosts or you leave.");
        }
    card_end(p);
    }
    category_tabs(menu, p.tab, {"TUNE", "PRACTICE", "MAP & HUD"}, "trainer-tabs"); // (the cheats are on Skater > Cheats)
    ImGui::PushID(p.tab);
    ImGui::BeginChild("trainer-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    p.pick.clear();
    switch (p.tab) {
    case 0: tune_tab(menu, model, callbacks, p, *view); break;
    case 1: practice_tab(menu, model, callbacks, p, *view); break;
    default: map_tab(menu, callbacks, p, *view); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}
} // namespace dingosdk::overlay::menu

namespace dingosdk::overlay {
bool trainer_open_requested() { return menu::trainer_take_open(); }
bool trainer_hud_pending() {
    const auto view = trainer::view();
    return (view->hud || view->hud_jump) && trainer::telemetry().skater;
}
void draw_trainer_hud() {
    const auto view = trainer::view();
    if (!view->hud && !view->hud_jump) return;
    const auto telemetry = trainer::telemetry();
    if (!telemetry.skater) return;
    static std::uint64_t shown_jump{};
    static double jump_until{};
    if (telemetry.last.serial != shown_jump) {
        shown_jump = telemetry.last.serial;
        jump_until = shown_jump ? ImGui::GetTime() + 7.0 : 0.0;
    }
    std::vector<std::string> lines;
    if (view->hud) {
        lines.push_back(std::format("{:.0f} km/h", telemetry.speed * 3.6f));
        if (telemetry.airborne) lines.push_back(std::format("air {:.2f} s   {:.2f} m", telemetry.air_time, telemetry.height));
    }
    if (view->hud_jump && telemetry.last.serial && ImGui::GetTime() < jump_until) {
        const auto &jump = telemetry.last;
        lines.push_back(std::format("jump {}:  {:.1f} km/h at {:.0f} deg", jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle));
        lines.push_back(std::format("{:.2f} s   {:.2f} m high   {:.2f} m far", jump.air_time, jump.height, jump.distance));
        lines.push_back(std::format("landed at {:.1f} km/h, {:.2f} m lower", jump.landing_speed * 3.6f, jump.drop));
        if (jump.spin >= 45.0f || jump.flip >= 90.0f) lines.push_back(std::format("spin {:.0f} deg   flip {:.0f} deg", jump.spin, jump.flip));
    }
    if (lines.empty()) return;
    auto *draw = ImGui::GetForegroundDrawList();
    const auto &display = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *font = ImGui::GetFont();
    const float big = 34.0f * scale, small = 18.0f * scale, pad = 10.0f * scale;
    float width = 0, height = pad * 2;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float size = i == 0 && view->hud ? big : small;
        width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0, lines[i].c_str()).x);
        height += size + 4.0f * scale;
    }
    const ImVec2 origin(display.x - width - pad * 2 - 24.0f * scale, 24.0f * scale);
    draw->AddRectFilled(origin, ImVec2(origin.x + width + pad * 2, origin.y + height), IM_COL32(12, 14, 18, 170), 6.0f * scale);
    float y = origin.y + pad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float size = i == 0 && view->hud ? big : small;
        draw->AddText(font, size, ImVec2(origin.x + pad, y), IM_COL32(245, 245, 240, 255), lines[i].c_str());
        y += size + 4.0f * scale;
    }
}
} // namespace dingosdk::overlay
