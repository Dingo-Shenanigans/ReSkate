#include "trainer_page.h"
#include "trainer.h"
#include "trainer_classes.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Text/ascii.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

// The SANDBOX page (once the Trainer) and the skating HUD. Presentation thread: reads the trainer's snapshots
// and queues `trainer ...` console commands; it never touches the game. Everything on it is reachable with the
// D-pad and A: values are the kit's steppers, a list of things is rows that open on their actions.
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
    bool want_presets{}; // `trainer open presets`: MY PRESETS opens, once the saved folds are in
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
    // The open row of each list that opens: EVERY VALUE (and a search), QUICK SETTINGS, MY PRESETS.
    std::string open_value, open_quick, open_preset;
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
bool card(SkateMenu &menu, Page &p, const char *id, const char *title, const char *subtitle = nullptr, bool open_by_default = true) {
    p.card_skipped = !p.pick.empty() && std::ranges::find(p.pick, std::string_view(id)) == p.pick.end();
    if (!p.card_skipped && p.open_cards > 0 && p.open_cards == p.current_topic) set_fold(id, true); // "Go to" shows what it points at
    return !p.card_skipped && fold_card(menu, id, title, subtitle, open_by_default);
}
void card_end(Page &p) {
    if (!p.card_skipped) end_card();
    p.card_skipped = false;
}
// A slider shows what the player is stepping until the trainer's own number catches up; after a reset there is nothing
// to catch up with.
void clear_edits(Page &p) {
    p.manual_editing = p.hippy_editing = p.nocomply_editing = p.boneless_editing = p.offboard_editing = p.flip_editing = p.pump_editing = p.revert_editing =
        p.underflip_editing = false;
    p.grind_side_editing = p.grind_push_editing = p.grind_range_editing = p.gravity_editing = p.gravity_landing_editing =
        p.gravity_timing_editing = false;
    p.trick_editing = {};
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

// A number the trainer takes at once: sent with every step or slide, and the box beside it takes any number.
void number_row(SkateMenu &menu, const CallbacksV3 &callbacks, const char *label, const char *command, float value, float low, float high,
                const char *format, const char *help) {
    ImGui::PushID(command);
    field(menu, label, help);
    float shown = value;
    if (slider_float("##value", &shown, low, high, format)) trainer_command(menu, callbacks, std::format("{} {}", command, number(shown)));
    ImGui::PopID();
}
// A number sent once it is let go (the end of a run of steps, a slide, a typed number), shown as edited until the
// trainer's own number catches up. Nothing under `floor` is sent.
void held_slider(SkateMenu &menu, const CallbacksV3 &callbacks, const char *label, const char *id, const char *help, float value, float &edit,
                 bool &editing, float low, float high, const char *format, const char *command, float floor = std::numeric_limits<float>::lowest()) {
    field(menu, label, help);
    float shown = editing ? edit : value;
    if (slider_float(id, &shown, low, high, format)) {
        edit = shown;
        editing = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        edit = std::max(floor, edit);
        trainer_command(menu, callbacks, std::format("{} {}", command, number(edit)));
    }
    if (!ImGui::IsItemActive() && editing && std::abs(value - edit) < 0.006f) editing = false;
}
// A setting the trainer can hold: left to the game (`own` says what that is now), held off or held on. Left / right
// steps it, and each step is sent.
void held_row(SkateMenu &menu, const CallbacksV3 &callbacks, const char *command, std::string_view key, const std::string &label, const std::string &hint,
              int held, std::string own, bool can) {
    const std::vector<std::string> options{std::move(own), "Held off", "Held on"};
    int selected = held < 0 ? 0 : held != 0 ? 2 : 1;
    if (option_row(menu, (label + "###" + std::string(key)).c_str(), hint.c_str(), selected, options, can))
        trainer_command(menu, callbacks, std::format("{} {} {}", command, key, selected == 0 ? "game" : selected == 1 ? "off" : "on"));
}
// What one of the game's grind switches reads while the game has it (-1: not found yet).
std::string live_text(int live) { return live < 0 ? "Game's own: not in use" : live ? "Game's own: on" : "Game's own: off"; }

// The rows EVERY VALUE and a search show.
void filter_rows(Page &p, const trainer::View &view) {
    const auto words = ascii_lower(p.search.data());
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
        if (words.empty() ? (group && row.group != *group) : !contains_words(ascii_lower(row.id + " " + row.label + " " + row.friendly), words))
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
    for (std::size_t i = 0; i < view.rows.size(); ++i) ids[i] = ascii_lower(view.rows[i].id);
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

// ---- a tuning value, as a stepper row or as a row that opens -----------------------------------------
std::string row_name(const trainer::Row &row, bool with_group, bool friendly) {
    return friendly && !row.friendly.empty() ? row.friendly : with_group ? row.group + " / " + row.label : row.label;
}
std::string row_hint(const trainer::Row &row) {
    return std::format("{}{}{}. The game's own is {}.{}", row.help, row.help.empty() ? "" : " ", row.id, number(row.stock),
                       row.used ? "" : " The game was not found reading it, so changing it may do nothing.");
}
std::string value_text(const trainer::Row &row) {
    if (row.kind == trainer::Kind::flag) return row.value != 0 ? "on" : "off";
    if (row.kind == trainer::Kind::integer) return std::format("{:.0f}", row.value);
    if (row.kind == trainer::Kind::curve || row.kind == trainer::Kind::graph) return std::format("x {:.2f}", row.value);
    return number(row.value);
}
// A value as the kit's stepper, nudged a fiftieth of its size at a time (a whole step for a whole number). The bar spans
// a sensible range around the game's own value; stepping on past an end, or the box beside it, takes any number.
void value_stepper(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::Row &row, bool can) {
    const bool scale = row.kind == trainer::Kind::curve || row.kind == trainer::Kind::graph;
    const bool whole = row.kind == trainer::Kind::integer;
    double value = row.value;
    set_console_step(whole ? 1.0 : scale ? 0.05 : std::max({std::abs(row.stock), std::abs(value), 0.01}) * 0.02);
    const double size = std::max({std::abs(row.stock), whole ? 5.0 : 0.01});
    const double low = scale ? 0.0 : row.stock >= 0 ? (row.stock == 0 ? -size * 10 : 0.0) : row.stock * 4,
                 high = scale ? 4.0 : row.stock <= 0 ? (row.stock == 0 ? size * 10 : 0.0) : row.stock * 4;
    ImGui::BeginDisabled(!can);
    if (slider_double("##value", &value, low, high, whole ? "%.0f" : scale ? "x %.2f" : "%.4g"))
        trainer_command(menu, callbacks, std::format("set {} {}", row.id, number(value)));
    ImGui::EndDisabled();
}
// "Go to" pointed at the row drawn from `top`: a blue tint over it for a few seconds, and the page scrolls to it once.
void flash_over(Page &p, const std::string &id, ImVec2 top, float width) {
    if (p.in_quick || p.flash.empty() || p.flash != id) return;
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(top.x - px(4), top.y - px(2)), ImVec2(top.x + width + px(4), bottom + px(2)), IM_COL32(60, 130, 230, 80));
    p.flash_y = top.y;
}
// A value on a section's card: a switch, or its name and a stepper ("*": changed from the game's own).
void value_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::Row &row,
               bool with_group, bool friendly) {
    ImGui::PushID(row.id.c_str());
    const auto top = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const auto label = row_name(row, with_group, friendly);
    const auto hint = row_hint(row);
    const bool can = view.editable && callbacks.queue_console_command;
    if (row.kind == trainer::Kind::flag) {
        bool on = row.value != 0;
        if (toggle_row(menu, label.c_str(), hint.c_str(), on, can)) // (a label that changes would lose the focus)
            trainer_command(menu, callbacks, std::format("set {} {}", row.id, on ? 1 : 0));
    } else {
        field(menu, (row.touched ? label + "  *" : label).c_str(), hint.c_str());
        value_stepper(menu, callbacks, row, can);
    }
    flash_over(p, row.id, top, width);
    ImGui::PopID();
}
// A value of EVERY VALUE or a search, as a row that opens: what it is set to at the right end (a blue edge: changed),
// and under it what it does, the control, RESET and LOCK. A locked value is one presets and resets leave alone.
void value_entry(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::Row &row, bool with_group) {
    ImGui::PushID(row.id.c_str());
    const auto top = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool can = view.editable && callbacks.queue_console_command;
    auto title = row_name(row, with_group, false);
    if (!row.used) title += "  (no use found)";
    if (expand_row(menu, row.id.c_str(), title, "", (row.frozen ? "LOCKED  " : "") + value_text(row), p.open_value, row.touched)) {
        ImGui::Indent(px(40));
        if (row.help.empty())
            note(std::format("{} / {}: one of the game's own tuning values, under its own name. The game's own is {}.", row.group, row.label, number(row.stock)).c_str());
        else note(std::format("{} The game's own is {}.", row.help, number(row.stock)).c_str());
        if (!row.used) warn("The game was not found reading it, so changing it may do nothing.");
        if (row.kind == trainer::Kind::flag) {
            bool on = row.value != 0;
            if (toggle_row(menu, "On", row.id.c_str(), on, can)) trainer_command(menu, callbacks, std::format("set {} {}", row.id, on ? 1 : 0));
        } else {
            field(menu, "Value", row.id.c_str());
            value_stepper(menu, callbacks, row, can);
        }
        ImGui::Unindent(px(40));
        const int pressed = row_actions(menu, "actions", {{"RESET", can && row.touched, false, "Back to the game's own."},
                                                          {row.frozen ? "UNLOCK" : "LOCK", callbacks.queue_console_command != nullptr, row.frozen,
                                                           "Locked, presets and resets leave it alone (RESET EVERYTHING clears locks too). A change applies without it."}});
        if (pressed == 0) trainer_command(menu, callbacks, "reset " + row.id);
        if (pressed == 1) trainer_command(menu, callbacks, std::format("freeze {} {}", row.id, row.frozen ? 0 : 1));
    }
    flash_over(p, row.id, top, width);
    ImGui::PopID();
}
// The rows of EVERY VALUE or a search. Only those on screen are drawn, but always the open one (it is taller than the
// rest, so it is not clipped with them), and every row on the frame "Go to" scrolls to one of them.
void value_list(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, bool with_group) {
    const int count = static_cast<int>(p.shown.size());
    const auto draw = [&](int i) { value_entry(menu, callbacks, p, view, view.rows[p.shown[static_cast<std::size_t>(i)]], with_group); };
    const auto run = [&](int first, int last) {
        if (first >= last) return;
        ImGuiListClipper clipper;
        clipper.Begin(last - first);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw(first + i);
    };
    if (p.flash_scroll && !p.flash.empty()) {
        for (int i = 0; i < count; ++i) draw(i);
        return;
    }
    int open = -1;
    for (int i = 0; i < count && !p.open_value.empty(); ++i)
        if (view.rows[p.shown[static_cast<std::size_t>(i)]].id == p.open_value) open = i;
    if (open < 0) {
        run(0, count);
        return;
    }
    run(0, open);
    draw(open);
    run(open + 1, count);
}

// A built-in preset as a dial: one multiplier that moves everything the preset moves, 1 the game's own.
void dial_row(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::PresetRow &preset) {
    ImGui::PushID(preset.name.c_str());
    const auto hint = std::format("{} 1 is the game's own; \"{}\" is x {}.", preset.note, preset.name, number(preset.amount));
    field(menu, std::abs(preset.factor - 1.0) > 1e-4 ? (preset.title + "  *").c_str() : preset.title.c_str(), hint.c_str());
    double value = preset.factor;
    // From 0: Grind lock-on at 0 is no lock-on (the owner's ask, 2026-10-19). A preset that turns its value down gets
    // less room above 1.
    const double low = 0.0, high = preset.amount < 1 ? 3.0 : 10.0;
    if (slider_double("##dial", &value, low, high, "x %.2f", ImGuiSliderFlags_Logarithmic) && value >= 0)
        trainer_command(menu, callbacks, std::format("dial {} {}", number(value), preset.name));
    ImGui::PopID();
}
// A built-in preset that is a plain switch: ON while every value it sets holds what it sets.
void preset_toggle(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::PresetRow &preset, bool can) {
    bool on = preset.active;
    if (toggle_row(menu, preset.name.c_str(), preset.note.c_str(), on, can)) {
        clear_edits(p);
        trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + console_arg(preset.name));
    }
}

// ---- the first section: every kind of preset, and everything that is changed in one list ---------
std::string setup_text(const trainer::SetupRow &row) {
    if (row.kind == trainer::setup_switch) return row.value != 0 ? "on" : "off";
    if (row.kind == trainer::setup_mode) return row.value < 0 ? "game's own" : row.value != 0 ? "held on" : "held off";
    return number(row.value);
}
// One of the trainer's own settings (a trick height, a grind switch, a bending rule ...) as the control that changes it.
void setup_row(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view, const trainer::SetupRow &row) {
    ImGui::PushID(row.key.c_str());
    const bool can = view.editable && callbacks.queue_console_command;
    const auto stock = row.kind == trainer::setup_mode ? std::string("left to the game") : row.kind == trainer::setup_switch ? std::string(row.stock != 0 ? "on" : "off")
                                                                                                                              : number(row.stock);
    const auto hint = std::format("One of the trainer's own settings ({}). The game's own: {}. GO TO shows it where it lives.", row.key, stock);
    if (row.kind == trainer::setup_switch) {
        bool on = row.value != 0;
        if (toggle_row(menu, row.title.c_str(), hint.c_str(), on, can)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, on ? 1 : 0));
    } else if (row.kind == trainer::setup_mode) {
        int mode = row.value < 0 ? 0 : row.value != 0 ? 2 : 1;
        field(menu, row.title.c_str(), hint.c_str());
        if (choice(menu, "##value", mode, {"The game's own", "Held off", "Held on"}, can)) trainer_command(menu, callbacks, std::format("setup {} {}", row.key, mode - 1));
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
// Every built-in preset in one card: the whole-game feels, the grind and transition feels, the tricklining extras and
// the built-in styles. Whatever one sets shows up in QUICK SETTINGS.
void presets_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, bool can_edit) {
    if (card(menu, p, "presets", "PRESETS", "A whole setup in one press")) {
        const auto active = [&](std::string_view name) {
            return std::ranges::any_of(view.presets, [&](const trainer::PresetRow &row) { return row.builtin && row.active && row.name == name; });
        };
        const auto feel = [&](const char *command) {
            clear_edits(p);
            trainer_command(menu, callbacks, command);
        };
        section(menu, "WHOLE GAME");
        const bool any = active("Skate 3 Easy") || active("Skate 3") || active("Skate 3 Hardcore");
        switch (button_row(menu, "whole-game",
                           {{"SKATE.", can_edit, !any, "The game's own tuning: takes a Skate 3 choice off again."},
                            {"SKATE 3 EASY", can_edit, active("Skate 3 Easy"), "Skate 3 on Easy: full-height pops every time, strong pushes, generous grind lock-on, easy spins."},
                            {"SKATE 3", can_edit, active("Skate 3"), "Skate 3 on Normal: its pop, grind pops, pushing, pumping, steering, manuals, spins, flips and bails."},
                            {"SKATE 3 HARDCORE", can_edit, active("Skate 3 Hardcore"), "Skate 3 on Hardcore: lower pops, weaker pushes, tight grind lock-on, slow auto spins."}})) {
        case 0: feel("feel stock"); break;
        case 1: feel("feel easy"); break;
        case 2: feel("feel normal"); break;
        case 3: feel("feel hardcore"); break;
        default: break;
        }
        const int grinds = grind_feel(view), transition = transition_feel(view);
        section(menu, "GRINDS");
        if (const int pressed = button_row(menu, "grind-feel",
                                           {{"SKATE.", can_edit, grinds == 0, "Every grind setting and switch is left to the game."},
                                            {"SKATE 3", can_edit, grinds == 1, "The left stick alone turns the board on a one-truck grind, the right stick tips and pops, a pop goes "
                                                                               "sideways the way the left stick is held, and what skate. added to leaving a grind is off."}});
            pressed >= 0)
            feel(pressed == 0 ? "grind feel skate" : "grind feel skate3");
        if (grinds == 2) note("Now your own mix. GRINDS has every row behind it.");
        section(menu, "TRANSITION");
        if (const int pressed = button_row(menu, "transition-feel",
                                           {{"SKATE.", can_edit, transition == 0, "Everything on transition is left to the game."},
                                            {"SKATE 3", can_edit, transition == 1, "Lean into the ramp on a coping grind, roll off the coping at your angle, no hang-ups coming back in, "
                                                                                   "handplants that come back in high on the wall, with Skate 3's coping timers."}});
            pressed >= 0)
            feel(pressed == 0 ? "transition feel skate" : "transition feel skate3");
        if (transition == 2) note("Now your own mix. TRANSITION has every row behind it.");
        section(menu, "TRICKLINING");
        bool extras = view.revert_boost > 0;
        if (toggle_row(menu, "Skate 3's extras", "The board bending boost at x 1, heavier revert and powerslide friction and higher footplants. TRICKLINING has every row behind it.",
                       extras, can_edit))
            feel(extras ? "trickline extras on" : "trickline extras off");
        section(menu, "STYLES");
        for (const auto &preset : view.presets)
            if (preset.builtin && preset.name != "Stock" && !preset.name.starts_with("Skate 3") && !preset.name.starts_with("Transition:"))
                preset_toggle(menu, callbacks, p, preset, can_edit);
        note("Each style also has a dial in its section.");
    }
    card_end(p);
}
// The player's own presets: load one or turn it off, have one load by itself on this map, share it as a line of text,
// delete it; save what is set now under a name, and import a line someone shared.
void my_presets_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    std::vector<const trainer::PresetRow *> own;
    for (const auto &preset : view.presets)
        if (!preset.builtin) own.push_back(&preset);
    if (p.want_presets) {
        set_fold("my-presets", true);
        p.want_presets = false;
    }
    const auto subtitle = own.empty() ? std::string("None saved yet") : std::format("{} saved", own.size());
    if (card(menu, p, "my-presets", "MY PRESETS", subtitle.c_str())) {
        const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
        for (const auto *preset : own) {
            const bool here = !view.map.empty() && view.profile_preset == preset->name;
            const auto side = std::string(preset->active ? "ON" : "") + (here ? (preset->active ? ", THIS MAP" : "THIS MAP") : "");
            ImGui::PushID(preset->name.c_str());
            if (expand_row(menu, preset->name.c_str(), preset->name, preset->note, side, p.open_preset, preset->active)) {
                const int pressed = row_actions(menu, "actions",
                                                {{preset->active ? "TURN OFF" : "LOAD", can_edit, !preset->active,
                                                  preset->active ? "Puts everything it set back to the game's own." : "Sets everything saved in it, on top of what is set now."},
                                                 {"ON THIS MAP", can_edit, false, "Loads it by itself every time this map loads."},
                                                 {"SHARE", can_edit, false, "Copies it to the clipboard as one line of text: anyone with ReSkate can import it."},
                                                 {"DELETE", can_edit, false, "Removes it from your list."}});
                if (pressed == 0) {
                    clear_edits(p);
                    trainer_command(menu, callbacks, std::string(preset->active ? "preset remove " : "preset apply ") + console_arg(preset->name));
                }
                if (pressed == 1) trainer_command(menu, callbacks, "profile set " + preset->name);
                if (pressed == 2) trainer_command(menu, callbacks, "preset export " + console_arg(preset->name));
                if (pressed == 3) trainer_command(menu, callbacks, "preset delete " + console_arg(preset->name));
            }
            ImGui::PopID();
        }
        if (own.empty()) note("Set the game up the way you like it, give it a name and SAVE.");
        // Naming one wants a keyboard; a controller passes over the box.
        field(menu, "New preset", "A name for the preset you are about to save.");
        ImGui::InputTextWithHint("##preset-name", "a name for it", p.preset_name.data(), p.preset_name.size());
        const int pressed = button_row(menu, "preset-save",
                                       {{"SAVE", p.preset_name[0] != 0 && !view.stock, true,
                                         view.stock ? "Nothing is changed, so there is nothing to save yet."
                                                    : "Saves everything changed now: tuning values, trick, grind, transition and bending settings. Not saved: locks, "
                                                      "No Bail, Noclip, Dark Pop, the game speed and your game's own settings."},
                                        {"SHARE WHAT IS CHANGED", !view.stock, false, "Copies your current setup to the clipboard as one line of text."},
                                        {"IMPORT FROM CLIPBOARD", true, false, "Adds a preset line someone shared (it starts with RST1:). Nothing changes until you load it."}});
        if (pressed == 0) trainer_command(menu, callbacks, std::string("preset save ") + console_arg(p.preset_name.data()));
        if (pressed == 1) trainer_command(menu, callbacks, "preset export current");
        if (pressed == 2) {
            const char *text = ImGui::GetClipboardText();
            if (text && trainer::stage_import(text)) trainer_command(menu, callbacks, "preset import");
            else feedback(menu, "The clipboard holds no text.");
        }
        if (!view.map.empty()) {
            info(menu, "This map loads", view.profile_preset.empty() ? "nothing by itself" : view.profile_preset);
            if (!view.profile_preset.empty() && button_row(menu, "profile-clear", {{"STOP LOADING IT ON THIS MAP"}}) == 0) trainer_command(menu, callbacks, "profile clear");
        }
    }
    card_end(p);
}
// The cheats that change how the skater plays (Skater > Cheats): listed in QUICK SETTINGS, named on the MODIFIED line and
// turned back by RESET EVERYTHING like the trainer's own values.
struct CheatLine {
    std::string name, value; // value: what it is set to, empty for a plain switch
    DebugRequest off{};      // how Reset puts it back: a debug request, or (with `console`) a command
    std::string console;
};
std::vector<CheatLine> cheat_lines(const Model &model) {
    std::vector<CheatLine> lines;
    const auto &debug = model.debug;
    if (debug.noclip) lines.push_back({"Noclip", {}, {DebugAction::set_noclip, false}});
    if (debug.no_bail) lines.push_back({"No Bail", {}, {DebugAction::set_no_bail, false}});
    if (debug.dark_pop) lines.push_back({"Dark Pop", {}, {DebugAction::set_dark_pop_enabled, false}});
    // A boost's stock speed is 0: off (InteractiveDebug).
    const auto boost = [&](const char *name, float speed, DebugAction action) {
        if (speed > 1e-3f) lines.push_back({name, std::format("+{:.1f}", speed), {action, false, 0.0f}});
    };
    boost("Forward boost", debug.forward_velocity_speed, DebugAction::set_forward_velocity_speed);
    boost("Up boost", debug.up_velocity_speed, DebugAction::set_up_velocity_speed);
    boost("Off-board up boost", debug.offboard_up_velocity_speed, DebugAction::set_offboard_up_velocity_speed);
    if (debug.revert_boost)
        lines.push_back({"Revert Boost", std::format("x{:.1f}", debug.revert_boost_speed), {DebugAction::set_revert_boost_enabled, false}});
    // (Grind exits and Board Break are plugins now: their own pages say what they are set to.)
    return lines;
}
void cheat_off(SkateMenu &menu, const CallbacksV3 &callbacks, const CheatLine &line) {
    if (!line.console.empty()) {
        if (callbacks.queue_console_command) send_console(menu, callbacks, line.console);
    } else if (callbacks.queue_debug) debug_request(menu, callbacks, line.off);
}
// The cheats and addons at the top of QUICK SETTINGS: a row each with what it is set to; it opens on GO TO (where it
// lives) and on turning it off.
void cheat_rows(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const std::vector<CheatLine> &lines) {
    if (lines.empty()) return;
    section(menu, "CHEATS AND ADDONS");
    for (const auto &line : lines) {
        const auto id = "cheat:" + line.name;
        ImGui::PushID(id.c_str());
        if (expand_row(menu, id.c_str(), line.name, "", line.value.empty() ? "on" : line.value, p.open_quick)) {
            const bool boost = line.off.action == DebugAction::set_forward_velocity_speed || line.off.action == DebugAction::set_up_velocity_speed ||
                               line.off.action == DebugAction::set_offboard_up_velocity_speed;
            const bool can = line.console.empty() ? callbacks.queue_debug != nullptr : callbacks.queue_console_command != nullptr;
            const int pressed = row_actions(menu, "actions", {{"GO TO", true, false, "Opens Skater > Cheats."},
                                                              {boost ? "BACK TO 0" : "TURN OFF", can, false, boost ? "Back to 0: off, as the game shipped." : "Turns it off."}});
            if (pressed == 0) {
                open_page(menu, page_skater);
                menu.skater_tab = 2;
            }
            if (pressed == 1) cheat_off(menu, callbacks, line);
        }
        ImGui::PopID();
    }
}
std::string quick_id(std::string_view id) {
    std::string result = "qh:" + std::string(id);
    for (auto &c : result)
        if (c == ' ') c = '_';
    return result;
}
// Everything that is changed right now, in one list: a row each, opening on the control to change it again, GO TO
// (where it lives), HIDE (off the list, still set) and RESET.
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
        cheat_rows(menu, callbacks, p, cheats);
        if (p.quick_all.empty()) {
            // With only cheats and addons on, their rows above are the whole list.
            if (cheats.empty())
                note("Everything you change shows up here, whatever section it is in. Open a row to change it again, hide it, or GO TO where it lives.");
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
            const auto show_hidden = hidden ? std::format("SHOW {} HIDDEN", hidden) : std::string("SHOW HIDDEN");
            const int tool = button_row(menu, "quick-tools",
                                        {{show_hidden.c_str(), hidden > 0, false, "Brings back every row you hid."},
                                         {"HIDE ALL", !shown.empty(), false, "Hides every row here. They stay set; each comes back the next time it changes."}});
            if (tool == 0)
                for (const auto &item : p.quick_all)
                    if (const auto id = id_of(item); fold_open(id, false)) hide(id, false);
            if (tool == 1)
                for (const auto &item : p.quick_all)
                    if (const auto id = id_of(item); !fold_open(id, false)) hide(id, true);
            p.in_quick = true;
            for (const auto &item : shown) {
                if (item.kind == 0) {
                    const auto &topic = trainer::topics[item.topic];
                    section(menu, item.topic ? std::string(topic.title).c_str() : "OTHER VALUES");
                    continue;
                }
                const auto id = id_of(item);
                ImGui::PushID(id.c_str());
                std::string title, side;
                if (item.kind == 1) {
                    const auto &preset = view.presets[item.index];
                    title = preset.title;
                    side = std::format("x {:.2f}", preset.factor);
                } else if (item.kind == 2) {
                    title = view.setups[item.index].title;
                    side = setup_text(view.setups[item.index]);
                } else {
                    title = row_name(view.rows[item.index], !item.topic, true);
                    side = value_text(view.rows[item.index]);
                }
                if (expand_row(menu, id.c_str(), title, "", side, p.open_quick)) {
                    ImGui::Indent(px(40));
                    if (item.kind == 1) {
                        ImGui::BeginDisabled(!can_edit);
                        dial_row(menu, callbacks, view.presets[item.index]);
                        ImGui::EndDisabled();
                    } else if (item.kind == 2) {
                        setup_row(menu, callbacks, view, view.setups[item.index]);
                    } else {
                        value_row(menu, callbacks, p, view, view.rows[item.index], !item.topic, true);
                    }
                    ImGui::Unindent(px(40));
                    const int pressed = row_actions(menu, "actions",
                                                    {{"GO TO", true, false, "Opens the section it lives in and scrolls to it, with the related settings around it."},
                                                     {"HIDE", true, false, "Off this list; it stays set, and comes back the next time it changes."},
                                                     {"RESET", can_edit, false, "Back to the game's own."}});
                    if (pressed == 0) go(item);
                    if (pressed == 1) hide(id, true);
                    if (pressed == 2) {
                        if (item.kind == 1) {
                            trainer_command(menu, callbacks, "dial 1 " + view.presets[item.index].name);
                        } else if (item.kind == 2) {
                            clear_edits(p);
                            const auto &setup = view.setups[item.index];
                            trainer_command(menu, callbacks, std::format("setup {} {}", setup.key, number(setup.stock)));
                        } else {
                            trainer_command(menu, callbacks, "reset " + view.rows[item.index].id);
                        }
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
// The bar that opens and closes one section, as a row of the kit: the focus lights it white and A (or a click) opens
// or folds it. What the section holds is under its name, what is changed in it at its right end.
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
    const float height = px(54), width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id.c_str());
    const bool pressed = ImGui::InvisibleButton("##section", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), lit = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    const auto seed = static_cast<unsigned>(ImGui::GetItemID()) & 0xfff;
    ImGui::PopID();
    if (pressed) {
        set_fold(id, open = !open);
        trainer_command(menu, callbacks, std::format("fold {} {}", id, open ? 1 : 0));
    }
    if (lit || hovered) set_console_hint(open ? "A folds this section away." : "A opens this section.");
    constexpr ImU32 ink = IM_COL32(11, 11, 11, 255);
    const ImU32 quiet = lit ? IM_COL32(70, 70, 74, 255) : skate_theme::grey_text;
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + height);
    skate_theme::rough_rect(draw, at, end, lit ? skate_theme::white : hovered ? skate_theme::tile_light : skate_theme::tile_grey, seed, px(1));
    if (!lit) draw->AddRectFilled(at, ImVec2(at.x + px(4), end.y), skate_theme::blue);
    fold_arrow(ImVec2(at.x + px(12), at.y), height, open, lit ? ink : skate_theme::white);
    const float x = at.x + px(18) + ImGui::GetFontSize() * 0.9f;
    float right = end.x - px(16);
    if (!summary.empty()) {
        const auto size = menu.bold->CalcTextSizeA(px(15), FLT_MAX, 0, summary.c_str());
        right -= size.x;
        draw->AddText(menu.bold, px(15), ImVec2(right, at.y + (height - size.y) * 0.5f), lit ? ink : skate_theme::blue, summary.c_str());
        right -= px(14);
    }
    draw->PushClipRect(at, ImVec2(right, end.y), true);
    draw->AddText(menu.bold, px(17), ImVec2(x, at.y + px(8)), lit ? ink : skate_theme::white, one.title.data(), one.title.data() + one.title.size());
    draw->AddText(menu.body, px(15), ImVec2(x, at.y + px(29)), quiet, one.about.data(), one.about.data() + one.about.size());
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(0, px(open ? 6.0f : 2.0f)));
    return open;
}
// The built-in presets of one section: a dial each, then the ones that are plain switches.
void dials_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, std::uint8_t topic, bool can_edit) {
    const auto shown = [&](const trainer::PresetRow &preset) {
        return preset.builtin && preset.topic == topic && preset.name != "Stock" && !preset.name.starts_with("Skate 3") && !preset.name.starts_with("Transition:");
    };
    if (std::ranges::none_of(view.presets, shown)) return;
    const std::string id = "dials-" + std::string(trainer::topics[topic].key);
    if (card(menu, p, id.c_str(), "DIALS AND SWITCHES", "1 is the game's own")) {
        ImGui::BeginDisabled(!can_edit);
        for (const auto &preset : view.presets)
            if (shown(preset) && preset.dial) dial_row(menu, callbacks, preset);
        ImGui::EndDisabled();
        for (const auto &preset : view.presets)
            if (shown(preset) && !preset.dial) preset_toggle(menu, callbacks, p, preset, can_edit);
        note("A dial moves every value its preset moves.");
    }
    card_end(p);
}
// A section's named values that no other card of it shows, one at a time.
void fine_card(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, std::uint8_t topic) {
    const auto &rows = p.topic_rows[topic];
    if (std::ranges::none_of(rows, [&](std::size_t i) { return row_works(view.rows[i]); })) return;
    const std::string id = "fine-" + std::string(trainer::topics[topic].key);
    if (card(menu, p, id.c_str(), "FINE TUNING", "One value at a time, * changed")) {
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
    std::vector<std::string> groups{"Every group"};
    groups.insert(groups.end(), view.groups.begin(), view.groups.end());
    int group = std::clamp(p.group - 1, 0, static_cast<int>(groups.size()) - 1);
    if (option_row(menu, "Group", "Left / right: one group of tuning values at a time.", group, groups)) p.group = group + 1;
    toggle_row(menu, "Only what I changed", "Lists only the values that differ from the game's own (and locked ones).", p.only_changed);
    toggle_row(menu, "Graph points", "Every point of the tuning graphs, not just one multiplier per graph.", p.graph_points);
    toggle_row(menu, "Values with no use found", "Also the values the game was never found or seen reading: changing those will probably do nothing.", p.show_unused);
    if (button_row(menu, "reset-values", {{"RESET VALUES", view.editable && callbacks.queue_console_command && view.touched, false,
                                           "Every tuning value back to the game's own. Locked values stay, and so do the trick, grind and transition settings."}}) == 0) {
        clear_edits(p);
        trainer_command(menu, callbacks, "reset all");
    }
    filter_rows(p, view);
    if (p.hidden_unused) note(std::format("{} shown, {} changed, {} hidden. A row opens on its control, RESET and LOCK.", p.shown.size(), view.touched, p.hidden_unused).c_str());
    else note(std::format("{} shown, {} changed. A row opens on its control, RESET and LOCK.", p.shown.size(), view.touched).c_str());
    value_list(menu, callbacks, p, view, p.group <= 1 || p.only_changed);
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
        my_presets_card(menu, callbacks, p, view);
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
        field(menu, "Grind lock-on (game setting)",
              "Your game's own Grind Assist, the same number as in its Settings menu: change it here or there. How much the game helps the board onto "
              "a rail or ledge. It is your setting, so it never counts as MODIFIED and no preset or reset changes it. The lock-on distance, reach and "
              "speeds are tuning values in this section.");
        if (view.grind_assist < 0) {
            note("Not saved yet: set it once in the game's Settings.");
        } else {
            double assist = view.grind_assist;
            ImGui::BeginDisabled(!callbacks.queue_console_command);
            if (slider_double("##assist", &assist, 0.0, 100.0, "%.0f")) trainer_command(menu, callbacks, std::format("grind assist {:.0f}", assist));
            ImGui::EndDisabled();
        }
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
    // One search for the whole tab: dials, switches, trick sliders and every value. Typing wants a keyboard; a
    // controller passes over the box to the sections.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search dials, tricks and every value...", p.search.data(), p.search.size());
    if (const int pressed = button_row(menu, "fold-sections", {{"OPEN ALL", true, false, "Opens every section."}, {"FOLD ALL", true, false, "Folds every section to its bar."}});
        pressed >= 0)
        for (const auto topic : order) {
            const std::string id = "section-" + std::string(trainer::topics[topic].key);
            set_fold(id, pressed == 0);
            trainer_command(menu, callbacks, std::format("fold {} {}", id, pressed == 0 ? 1 : 0));
        }
    const auto words = ascii_lower(p.search.data());
    place_rows(p, view);
    if (!words.empty()) {
        // Searching looks through everything at once, whatever section it is in.
        const auto listed = [&](const trainer::PresetRow &preset) {
            return preset.builtin && preset.name != "Stock" && !preset.name.starts_with("Skate 3") && !preset.name.starts_with("Transition:") &&
                   contains_words(ascii_lower(preset.title + " " + preset.name + " " + preset.note), words);
        };
        if (std::ranges::any_of(view.presets, listed)) {
            begin_card(menu, "dials-found", "DIALS AND SWITCHES", "1 is the game's own");
            ImGui::BeginDisabled(!can_edit);
            for (const auto &preset : view.presets)
                if (listed(preset) && preset.dial) dial_row(menu, callbacks, preset);
            ImGui::EndDisabled();
            for (const auto &preset : view.presets)
                if (listed(preset) && !preset.dial) preset_toggle(menu, callbacks, p, preset, can_edit);
            end_card();
        }
        ImGui::BeginDisabled(!can_edit);
        trick_heights(menu, callbacks, p, view, words, 0);
        ImGui::EndDisabled();
        trainer::note_class_list_shown();
        filter_rows(p, view);
        note(std::format("{} values match", p.shown.size()).c_str());
        value_list(menu, callbacks, p, view, true);
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
                const auto label = std::format("RESET {}", one.title);
                if (button_row(menu, "reset-section", {{label.c_str(), can_edit && view.changed[topic], false,
                                                        "Everything this section shows back to the game's own: its values, dials and settings. Locked values stay; "
                                                        "the other sections are left alone."}}) == 0) {
                    clear_edits(p);
                    trainer_command(menu, callbacks, std::format("reset section {}", one.key));
                }
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
        return contains_words(ascii_lower(std::string(label) + " tricks"), words);
    };
    if (std::ranges::none_of(labels, wanted)) return;
    // The switches are drawn by hand, so they are told when the card is locked.
    const bool can = view.editable && callbacks.queue_console_command != nullptr;
    if (card(menu, p, part == 2 ? "trick-foot" : part == 1 ? "trick-heights" : "trick-found", part == 2 ? "JUMP" : "TRICKS", "1.0 is the game's own")) {
        // A multiplier sent once it is let go. `limited`: the game itself stops at the slider's end, so a typed number is
        // held to it too.
        const auto slider = [&](const char *label, const char *option, float value, float &edit, bool &editing, const char *help, float high = 50.0f, bool limited = false, bool always = false) {
            if (!always && !wanted(label)) return false;
            // A value that is not the game's own (1) is marked, like a moved dial.
            const auto shown_label = std::abs(value - 1.0f) > 1e-4f ? std::string(label) + "  *" : std::string(label);
            field(menu, shown_label.c_str(), help);
            ImGui::PushID(option);
            float shown = editing ? edit : value;
            if (slider_float("##height", &shown, 0.1f, high, "x %.2f", (limited ? ImGuiSliderFlags_AlwaysClamp : 0) | ImGuiSliderFlags_Logarithmic)) {
                edit = shown;
                editing = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                // A typed 0 or less is no multiplier.
                if (edit > 0) trainer_command(menu, callbacks, std::format("option {} {:.2f}", option, edit));
                else editing = false;
            }
            if (!ImGui::IsItemActive() && editing && std::abs(value - edit) < 0.005f * std::max(1.0f, std::abs(edit))) editing = false;
            ImGui::PopID();
            return true;
        };
        const bool gravity_shown = slider("Gravity", "gravity", view.gravity, p.gravity_edit, p.gravity_editing,
                                          "Gravity in the air, on the board and on foot. 1 is the game's own. At 0.5 the same pop goes twice as high and hangs twice "
                                          "as long; at 2 you drop twice as fast. It acts in the air only: rolling down a hill, grinds and a bailed skater are the game's "
                                          "own. The game aims an air off a ramp back at the ramp with its own gravity, so with another gravity a vert air can come down "
                                          "somewhere else. It takes hold at once; ADVANCED GRAVITY times the tricks and landings for it.",
                                          3.0f);
        if (gravity_shown && view.gravity != 1.0f && !view.gravity_live) note("On foot this game build keeps its own gravity: the setting acts on the board only.");
        if (gravity_shown) {
            if (fold_card(menu, "advanced-gravity", "ADVANCED GRAVITY", "Trick timing and landing plans", view.gravity_landing_own || view.gravity_timing != 1.0f)) {
                // Animation timing: 0 .. 1, not a multiplier.
                field(menu, view.gravity_timing != 1.0f ? "Animation timing  *" : "Animation timing",
                      "How the skater's tricks and landings are timed in the air when Gravity is not 1. 1 (the default): animations play faster by just as "
                      "much as the flight is shorter (slower when it is longer), so tricks, catches and landings look as at the game's own gravity. 0: the "
                      "game's own timing, which cannot hurry a catch, so with more gravity a catch can still be playing when the wheels touch.");
                ImGui::PushID("gravity_timing");
                float timing = p.gravity_timing_editing ? p.gravity_timing_edit : view.gravity_timing;
                if (slider_float("##timing", &timing, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                    p.gravity_timing_edit = timing;
                    p.gravity_timing_editing = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option gravity_timing {:.2f}", p.gravity_timing_edit));
                if (!ImGui::IsItemActive() && p.gravity_timing_editing && std::abs(view.gravity_timing - p.gravity_timing_edit) < 0.005f)
                    p.gravity_timing_editing = false;
                ImGui::PopID();
                bool own = view.gravity_landing_own;
                if (toggle_row(menu, "Landing gravity of its own",
                               "The game plans every landing (where you come down, when, and the time its animations count down to) with a gravity. Off: with "
                               "Gravity, so the plan matches the flight. On: with Landing gravity below; less than the flight lands late, more reaches for the "
                               "ground early.",
                               own, can))
                    trainer_command(menu, callbacks, std::format("option gravity_landing_own {}", own ? 1 : 0));
                if (own)
                    slider("Landing gravity", "gravity_landing", view.gravity_landing, p.gravity_landing_edit, p.gravity_landing_editing,
                           "The gravity the game plans landings with, as a multiple of its own. Set it to Gravity for landings that match the flight.", 3.0f, false, true);
            }
            end_card();
        }
        slider("Manual pop height", "manual_height", view.manual_height, p.manual_edit, p.manual_editing,
               "How high you pop out of a manual or a nose manual. 1 is the game's own (lower than a riding ollie). Ollie height raises both.");
        slider("Flip trick speed", "flip_speed", view.flip_speed, p.flip_edit, p.flip_editing,
               "How fast the board turns in every flip trick, not how high you pop. A slow flip is slow from the start and is not hurried round before "
               "you land: with too little air you land on a board still turning. It takes hold about 15 seconds after every level load.",
               3.0f);
        if (slider("Underflip speed", "underflip_speed", view.underflip_speed, p.underflip_edit, p.underflip_editing,
                   "How fast the board turns in an underflip (the kickflip, heelflip, shuvit and impossible underflips, the hospital and flower flips). 1 is "
                   "the game's own. By itself it sets the whole underflip and Flip trick speed does not reach it; with 'Underflip part only' the flip turns "
                   "at Flip trick speed and this sets the part where the board turns back.",
                   3.0f)) {
            bool split = view.underflip_split;
            if (toggle_row(menu, "Underflip part only", "Off: Underflip speed sets the whole underflip. On: its flip turns at Flip trick speed, and Underflip speed "
                                                        "takes over where the board starts to turn back (seen from the board, so a tick or two late).",
                           split, can))
                trainer_command(menu, callbacks, std::format("option underflip_split {}", split ? 1 : 0));
        }
        {
            constexpr std::size_t late = trainer::late_flip_slot;
            slider("Late flip speed", "flip.lateflip", view.flip_trick[late], p.trick_edit[late], p.trick_editing[late],
                   "How fast the board turns in a late flip (a flick after the pop, in the air), times the flip trick speed. 1 is the game's own. It counts "
                   "with or without Advanced trick speed.",
                   3.0f);
        }
        if (!view.flip_live && (view.flip_speed != 1.0f || view.flip_advanced || view.flip_flick) && wanted("Flip trick speed"))
            note("Finding the flip animations: flip speeds take hold about 15 seconds after a level loads.");
        if (wanted("Flip trick speed") || wanted("Slow flips pop higher") || wanted("Flick tricks at full speed") || wanted("Flip landing limit")) {
            bool pop = view.flip_pop;
            if (toggle_row(menu, "Slow flips pop higher",
                           "Every pop into a slowed flip trick goes at least as high as the trick needs (about 0.9 m at x 0.5 and 2.9 m at x 0.25, where the game's "
                           "own is 0.4 m), so it lands clean instead of sinking. Ollies, nollies and boned ollies are slowed and popped the same way while it is on.",
                           pop, can))
                trainer_command(menu, callbacks, std::format("option flip_pop {}", pop ? 1 : 0));
            bool flick = view.flip_flick;
            if (toggle_row(menu, "Flick tricks at full speed",
                           "The game turns the board slower the slower the stick was flicked, so long stick motions (front foot impossibles, 360 pop shuvits) "
                           "land half way round. With this every flip turns as if the flick was a full one; a slow flick no longer gives a slow flip.",
                           flick, can))
                trainer_command(menu, callbacks, std::format("option flip_flick {}", flick ? 1 : 0));
            bool limit = view.flip_limit;
            if (toggle_row(menu, "Flip landing limit",
                           "The game's own rule: a flip still turning just before the landing (about 1/6 s) is hurried round so it lands bolts. On is the game's "
                           "own. Off lets slowed flips stay slow to the ground (pair it with 'Slow flips pop higher'); off counts as modified.",
                           limit, can))
                trainer_command(menu, callbacks, std::format("option flip_limit {}", limit ? 1 : 0));
        }
        if (wanted(advanced_label)) {
            bool on = view.flip_advanced;
            if (toggle_row(menu, "Advanced trick speed",
                           "Every flip trick gets a speed of its own, times the flip trick speed above (each underflip times Underflip speed). Nollie tricks use "
                           "their regular trick's speed, but for the nollie impossibles. The ollie's only acts with 'Slow flips pop higher'. Off, the speeds are "
                           "kept but not used. They take hold about 15 seconds after every level load.",
                           on, can))
                trainer_command(menu, callbacks, std::format("option flip_advanced {}", on ? 1 : 0));
            if (view.flip_advanced_blocked) warn("Off in this multiplayer game: the host's rules apply here.");
            if (on) {
                if (fold_card(menu, "flip-each", "SPEED OF EACH FLIP TRICK", "Times the speeds above")) {
                    for (std::size_t i = 0; i < trainer::late_flip_slot; ++i) { // the Late flip has its slider above
                        const auto &trick = trainer::flip_tricks[i];
                        const std::string label(trick.label), option(trick.key);
                        slider(label.c_str(), option.c_str(), view.flip_trick[i], p.trick_edit[i], p.trick_editing[i],
                               "How fast the board turns in this trick, times the flip trick speed above. 1 leaves it at that speed.", 3.0f, false, true);
                    }
                    section(menu, "UNDERFLIPS");
                    for (std::size_t i = trainer::first_underflip_slot; i < trainer::flip_tricks.size(); ++i) {
                        const auto &trick = trainer::flip_tricks[i];
                        const std::string label(trick.label), option(trick.key);
                        slider(label.c_str(), option.c_str(), view.flip_trick[i], p.trick_edit[i], p.trick_editing[i],
                               "How fast the board turns in this underflip, times Underflip speed above (with 'Underflip part only', the part where the board "
                               "turns back). Nollie underflips use their regular trick's speed, but for the nollie impossibles.",
                               3.0f, false, true);
                    }
                    section(menu, "LATE FLIPS");
                    slider("Late flip", "flip.lateflip", view.flip_trick[trainer::late_flip_slot], p.trick_edit[trainer::late_flip_slot],
                           p.trick_editing[trainer::late_flip_slot],
                           "The same setting as Late flip speed above: the game tells every late flip apart only as 'a late flip', so they share this one speed.",
                           3.0f, false, true);
                }
                end_card();
            }
        }
        slider("No comply height", "nocomply_height", view.nocomply_height, p.nocomply_edit, p.nocomply_editing, "How high a no comply pops. 1 is the game's own.");
        slider("Boneless height", "boneless_height", view.boneless_height, p.boneless_edit, p.boneless_editing, "How high a boneless pops. 1 is the game's own.");
        slider("Hippy jump height", "hippy_height", view.hippy_height, p.hippy_edit, p.hippy_editing, "How high you jump off the board in a hippy jump. 1 is the game's own.");
        slider("Off-board jump height", "offboard_height", view.offboard_height, p.offboard_edit, p.offboard_editing, "How high you jump on foot. 1 is the game's own.");
        if (slider("Pump power (pumping transitions)", "pump_power", view.pump_power, p.pump_edit, p.pump_editing,
                   "How much speed pumping a transition gives. 1 is the game's own; 2 gives about one more of the game's pumps on top of each of yours, 10 "
                   "about nine more, under 1 takes speed away. The game still decides when you are pumping. It is found about 15 seconds after a level loads.")) {
            if (view.pump_blocked) warn("Off in this multiplayer game: the host's rules apply here.");
            else if (view.pump_power != 1.0f && !view.pump_live) note("Finding the game's pumping in memory: it acts about 15 seconds after a level loads.");
        }
        if (wanted("Board bending boost (revert speed)")) {
            // Not a multiplier: 0 is the game's own (no boost).
            field(menu, "Board bending boost",
                  "Land with the board turned out of line and the game snaps it straight; with this, that snap throws you forward as in Skate 3. x 1 gives "
                  "about +1.5 to +2.6 m/s a landing. 0 is off, the game's own. From AutoRevertBoost by Sivaes, jaq and OVM.");
            ImGui::PushID("revert_boost");
            float shown = p.revert_editing ? p.revert_edit : view.revert_boost;
            if (slider_float("##strength", &shown, 0.0f, 5.0f, shown <= 0.0f ? "off" : "x %.2f")) {
                p.revert_edit = shown;
                p.revert_editing = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (p.revert_edit >= 0) trainer_command(menu, callbacks, std::format("option revert_boost {:.2f}", p.revert_edit));
                else p.revert_editing = false;
            }
            if (!ImGui::IsItemActive() && p.revert_editing && std::abs(view.revert_boost - p.revert_edit) < 0.006f) p.revert_editing = false;
            ImGui::PopID();
        }
        if (part == 1 && button_row(menu, "reset-tricks", {{"RESET TRICKS", can, false, "Every slider and switch on this card back to the game's own."}}) == 0) {
            clear_edits(p);
            trainer_command(menu, callbacks, "reset tricks");
        }
        note("Not tuning values: the game sets these in its trick scripts and flip curves. Type in the box to go past a slider's end.");
    }
    card_end(p);
}

void practice_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    if (card(menu, p, "speed", "GAME SPEED", "Slow motion for learning a line")) {
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
            if (slider_float("##game-speed", &value, 0.05f, 2.0f, current == 0.0 ? "paused" : "%.2fx") && value >= 0) set(value);
            const int pressed = button_row(menu, "speeds", {{current == 0.0 ? "RESUME" : "PAUSE", true, current == 0.0}, {"0.1x", true, current == 0.1},
                {"0.25x", true, current == 0.25}, {"0.5x", true, current == 0.5}, {"0.75x", true, current == 0.75}, {"1x", true, current == 1.0}});
            constexpr std::array<double, 5> quick{0.1, 0.25, 0.5, 0.75, 1.0};
            if (pressed == 0) set(current == 0.0 ? 1.0 : 0.0);
            else if (pressed > 0) set(quick[static_cast<std::size_t>(pressed - 1)]);
            note("The jump read-out measures real time, so it reads low while the game is slowed.");
        }
    }
    card_end(p);

    if (card(menu, p, "markers", "MARKERS", "Saved for each map")) {
        int slot = view.slot;
        if (choice(menu, "marker-slot", slot, {"1", "2", "3", "4", "5"})) trainer_command(menu, callbacks, std::format("slot {}", slot + 1));
        const auto &marker = view.markers[static_cast<std::size_t>(std::clamp(view.slot, 0, static_cast<int>(trainer::marker_slots) - 1))];
        info(menu, "Selected", marker.set ? std::format("{:.1f}, {:.1f}, {:.1f}", marker.position[0], marker.position[1], marker.position[2]) : "empty");
        const bool map = !view.map.empty();
        const int pressed = button_row(menu, "marker-actions", {{"SAVE HERE", map && telemetry.skater, false, "Put the selected marker where you are."},
            {"GO", map && marker.set, false, "Go to the selected marker."}, {"CLEAR", map && marker.set, false, "Empty the selected marker."}});
        if (pressed == 0) trainer_command(menu, callbacks, "marker save");
        if (pressed == 1) trainer_command(menu, callbacks, "marker go");
        if (pressed == 2) trainer_command(menu, callbacks, "marker clear");
        bool auto_return = view.auto_return;
        if (toggle_row(menu, "Return after a bail", "Go back to the selected marker when the skater wipes out.", auto_return))
            trainer_command(menu, callbacks, std::format("option auto_return {}", auto_return ? 1 : 0));
        field(menu, "Return delay", "How long after a bail before you are put back at the marker, in seconds. Type a number for longer than the slider.");
        float delay = view.return_delay;
        slider_float("##return-delay", &delay, 0.0f, 10.0f, "%.1f s");
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
        // A position typed wants a keyboard; with a controller SET TO HERE fills it from where you are.
        field(menu, "X, Y, Z", "A position in the game's own coordinates: X, height, Z.");
        ImGui::InputFloat3("##teleport", p.teleport.data(), "%.2f");
        const int pressed = button_row(menu, "teleport-actions",
                                       {{"SET TO HERE", telemetry.skater, false, "Fills the position with where you are now."},
                                        {"GO THERE", telemetry.skater, false, "Moves you to the position. You arrive on foot."},
                                        {"COPY POSITION", telemetry.skater, false, "Copies where you are to the clipboard as X, Y, Z."},
                                        {"COPY FOR BLENDER", telemetry.skater, false, "Copies where you are in Blender's axes (Z up), for map makers."}});
        if (pressed == 0) p.teleport = telemetry.position;
        if (pressed == 1) trainer_command(menu, callbacks, std::format("tp {:.3f} {:.3f} {:.3f}", p.teleport[0], p.teleport[1], p.teleport[2]));
        if (pressed == 2) ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]).c_str());
        if (pressed == 3) ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], -telemetry.position[2], telemetry.position[1]).c_str());
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
// Everything that makes a trick line, in one place: the same values and sliders as the Tune tab's sections, gathered,
// with the game's own and Skate 3's grind feel. TRICKLINING, GRINDS and POP AND TRICKS pick their cards out of it.
void trickline_section(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;
    ImGui::BeginDisabled(!can_edit);
    if (card(menu, p, "trickline", "TRICKLINING EXTRAS", "What Skate 3's tricklining had beyond its tuning")) {
        bool extras = view.revert_boost > 0;
        if (toggle_row(menu, "Skate 3's tricklining extras",
                       "The board bending boost below at x 1, heavier revert and powerslide friction (x 3 and x 1.5) and higher footplants (no comply and "
                       "boneless height x 1.5). First guesses: tune each of them below.",
                       extras, can_edit))
            trainer_command(menu, callbacks, extras ? "trickline extras on" : "trickline extras off");
        note("Off puts back only what still holds these numbers: a value you have moved since stays yours.");
    }
    card_end(p);
    if (card(menu, p, "grind-feel", "GRINDS FEEL LIKE", "The game's own, or Skate 3's")) {
        const int feel = grind_feel(view);
        const int pressed = button_row(menu, "grind-feel",
                                       {{"SKATE.", can_edit, feel == 0, "Every grind setting and switch is left to the game."},
                                        {"SKATE 3", can_edit, feel == 1,
                                         "Turns off what skate. added to leaving a grind (leaning off without a pop, the sideways ollie, exits onto another grind, double "
                                         "grinds, stronger and camera-relative exits, linear drive). The left stick alone turns the board on a one-truck grind, the right "
                                         "stick tips it and pops, and a pop goes sideways the way the left stick is held."}});
        if (pressed == 0) trainer_command(menu, callbacks, "grind feel skate");
        if (pressed == 1) trainer_command(menu, callbacks, "grind feel skate3");
        if (feel == 2) note("Now your own mix of the rows below.");
        note("Skate 3 feel holds five of your game's own grind settings while it is on; your own settings are not changed and are back the moment "
             "you pick skate. The left stick's turn needs an Xbox-style (XInput) controller.");
    }
    card_end(p);
    if (card(menu, p, "grind-stick", "STICK AND POPS ON A GRIND", "How far the stick turns the board, and where a pop goes")) {
        held_slider(menu, callbacks, "Pop: side force", "##grind-side",
                    "Multiplies the part of a grind or slide pop that goes sideways off the edge. 1 is the game's own, 0 pops straight along the edge.",
                    view.grind_side, p.grind_side_edit, p.grind_side_editing, 0.0f, 10.0f, "x %.2f", "grind side", 0.0f);
        held_slider(menu, callbacks, "Pop: push with stick", "##grind-push",
                    "Skate 3's rule: a pop off a grind goes sideways the way the left stick is held, by this many m/s at full stick. Skate 3's own number is "
                    "1.8; 0 is off. A negative number pushes the other way.",
                    view.grind_push, p.grind_push_edit, p.grind_push_editing, -5.0f, 5.0f, "%.2f m/s", "grind push");
        held_slider(menu, callbacks, "Left stick turn range", "##grind-range",
                    "Skate 3's turn on a grind: the left stick alone turns the board, with Skate 3's dead zone, curve and easing, up to this many degrees at "
                    "the end of the stick; the right stick still tips and pops. 0 is off: skate.'s own turn, which follows the right stick. A negative "
                    "number turns the other way.",
                    view.grind_range, p.grind_range_edit, p.grind_range_editing, -180.0f, 180.0f,
                    (p.grind_range_editing ? p.grind_range_edit : view.grind_range) == 0.0f ? "off" : "%.2f", "grind range", -180.0f);
    }
    card_end(p);
    if (card(menu, p, "grind-exits", "GRIND EXITS AND TRANSFERS", "Each left to the game, or held off or on")) {
        if (!p.grind_watching) {
            trainer_command(menu, callbacks, "grind watch 1");
            p.grind_watching = true;
        }
        note("The first six are your own game settings: holding one here leaves yours as it is, and it is back with the game's own, RESET "
             "EVERYTHING or leaving the game.");
        // The game's own settings first: they change which grind code runs.
        for (std::size_t i = 0; i < trainer::grind_settings.size(); ++i) {
            const auto &one = trainer::grind_settings[i];
            const std::string label(one.label), help = std::string(one.help) + " (The game's setting " + std::string(one.setting) + ", " +
                                                       (one.stock ? "on" : "off") + " by default.) Held, your own setting is not changed.";
            held_row(menu, callbacks, "grind", one.key, label, help, view.grind_setting[i], view.grind_own[i] ? "Yours: on" : "Yours: off", can_edit);
        }
        for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
            const auto &one = trainer::grind_switches[i];
            const std::string label(one.label);
            if (one.read_only) {
                info(menu, label.c_str(), view.grind_live[i] < 0 ? "not in use yet" : view.grind_live[i] ? "happening now" : "not now");
                continue;
            }
            const std::string help = std::string(one.help) + " (" + std::string(one.state) + ".)";
            held_row(menu, callbacks, "grind", one.key, label, help, view.grind_mode[i], live_text(view.grind_live[i]), can_edit);
        }
        note("These are the game's names for its own switches: what each changes in play is not measured yet. Most have a value once you are on "
             "the board, the rest on the first grind, and they take hold about 15 seconds after a level loads.");
    }
    card_end(p);

    // What each group of rows is for in a line, said once at the top of its card.
    static constexpr const char *group_cards[][3]{
        {"", "", ""},
        {"trick-pops", "POPS",
         "Max is a held pop, min a quick flick; bring them together and every pop is the same height. The stick boost is the speed you can add by "
         "pushing the stick as you pop. Pops out of grinds are on GRINDS."},
        {"trick-spins", "SPINS AND FLIPS",
         "How fast you and the board turn, and how long a flip may take to come round (its catch time)."},
        {"trick-manuals", "MANUALS AND STEERING",
         "Manual balance (the three balance numbers are how hard the game corrects you), how far the board may tip, and how the board steers and settles."},
    };
    const auto rows = [&](std::size_t g) {
        for (const auto i : p.trick_rows[g])
            if (i < view.rows.size() && row_works(view.rows[i])) value_row(menu, callbacks, p, view, view.rows[i], false, true);
    };

    if (card(menu, p, "trick-revert", "BOARD BENDING, REVERTS AND POWERSLIDES", "Speed out of a bent landing")) {
        const auto &r = view.revert;
        note("Land a spin with the board twisted off your body and the game snaps it straight. In Skate 3 that snap threw you forward, so liners "
             "bent the board on every landing instead of pushing; skate. gives nothing for it, and this boost puts the speed back.");
        number_row(menu, callbacks, "Board bending boost", "option revert_boost", view.revert_boost, 0.0f, 5.0f, view.revert_boost <= 0 ? "off" : "x %.2f",
                   "How hard a bend throws you forward. 0 is off, the game's own; x 1 is about +2 m/s (7 km/h) a landing. From AutoRevertBoost by Sivaes, jaq and OVM.");
        if (fold_card(menu, "revert-rules", "WHAT COUNTS AS A BEND", "And what it is worth", false)) {
            number_row(menu, callbacks, "Spin needed", "revert spin", r.min_spin, 0.0f, 180.0f, "%.0f deg",
                       "How far you must have turned in the air. Lower it and smaller turns count as bends.");
            number_row(menu, callbacks, "Board off your body", "revert twist", r.min_twist, 0.0f, 90.0f, "%.0f deg",
                       "The rule that makes a bend: the board must land twisted at least this far off your body. Clean 180s land 0 to 3 degrees off, auto "
                       "reverts and board bends 70 to 90.");
            number_row(menu, callbacks, "or off the travel", "revert slip", r.min_slip, 0.0f, 90.0f, r.min_slip >= 90.0f ? "off" : "%.0f deg",
                       "Off at 90, as shipped. Lower, a landing also counts when you and the board land this far out of line with where you are going. "
                       "Careful: the game straightens ordinary 180s that way too, so a low number gives speed on plain 180s. Such a landing gets the auto "
                       "revert boost at most.");
            number_row(menu, callbacks, "Boost: a small bend", "revert bend", r.bend_boost, 0.0f, 10.0f, "+%.1f m/s",
                       "Speed added when the board turned 40 degrees past the body over the flight (times the boost strength).");
            number_row(menu, callbacks, "Boost: a full bend", "revert full", r.full_boost, 0.0f, 10.0f, "+%.1f m/s",
                       "The same at 360 degrees; in between it rises in a straight line up to 140 degrees.");
            number_row(menu, callbacks, "Boost: an auto revert", "revert auto", r.auto_boost, 0.0f, 10.0f, "+%.1f m/s",
                       "Speed added when the board turned more than 140 degrees past the body.");
            number_row(menu, callbacks, "No boost above", "revert max_speed", r.max_speed, 5.0f, 100.0f, "%.0f m/s", "Reverts stop adding speed here (1 m/s = 3.6 km/h).");
            number_row(menu, callbacks, "Time between boosts", "revert cooldown", r.cooldown, 0.0f, 3.0f, "%.2f s", "How soon the next revert can boost again.");
            number_row(menu, callbacks, "Shortest flight", "revert min_air", r.min_air, 0.0f, 2.0f, "%.2f s", "A hop shorter than this never boosts.");
            if (button_row(menu, "revert-reset", {{"RESET THESE RULES", can_edit, false, "The rules above back to the trainer's defaults."}}) == 0)
                trainer_command(menu, callbacks, "revert reset");
        }
        end_card();
        note("Friction: how much speed a revert or a powerslide scrubs off (higher loses more); Skate 3's were heavier. Forward force: a push the "
             "game adds while you revert or slide.");
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
    // The game's own settings shown on one card: a row each, left to the game or held off or on.
    const auto settings = [&](std::uint8_t which) {
        for (std::size_t i = 0; i < trainer::transition_settings.size(); ++i) {
            const auto &one = trainer::transition_settings[i];
            if (one.card != which) continue;
            const std::string label(one.label), help = std::string(one.help) + " (The game's setting " + std::string(one.setting) + ", " +
                                                       (one.stock ? "on" : "off") + " by default.) A setting of your own game: held, yours is not changed.";
            held_row(menu, callbacks, "transition", one.key, label, help, view.tr_setting[i], view.tr_own[i] ? "Yours: on" : "Yours: off", can_edit);
        }
    };
    // One of the grind card's switches, shown here too because it decides how a coping is ridden.
    const auto grind_switch = [&](std::string_view key, const char *more) {
        for (std::size_t i = 0; i < trainer::grind_switches.size(); ++i) {
            const auto &one = trainer::grind_switches[i];
            if (one.key != key) continue;
            const std::string label(one.label), help = std::string(one.help) + " " + more + " (" + std::string(one.state) + "; the same switch as on GRINDS.)";
            held_row(menu, callbacks, "grind", one.key, label, help, view.grind_mode[i], live_text(view.grind_live[i]), can_edit);
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

    if (card(menu, p, "transition-feel", "TRANSITION FEELS LIKE", "The game's own, or Skate 3's")) {
        const int feel = transition_feel(view);
        const int pressed = button_row(menu, "transition-feel",
                                       {{"SKATE.", can_edit, feel == 0, "Everything on this list is left to the game."},
                                        {"SKATE 3", can_edit, feel == 1,
                                         "Every help below at Skate 3's numbers: lean into the ramp on a coping grind, roll off the coping at your angle, no hang-ups "
                                         "coming back in, handplants that come back in high on the wall, with Skate 3's coping exit timers and handplant exit. Airs and "
                                         "pumping stay skate.'s own (use Pump power)."}});
        if (pressed == 0) trainer_command(menu, callbacks, "transition feel skate");
        if (pressed == 1) trainer_command(menu, callbacks, "transition feel skate3");
        if (feel == 2) note("Now your own mix of the rows below.");
        note("New, and built from Skate 3's rules before it was measured in skate.: tell us what each help does on your ramps. With the jump read-out "
             "on (MAP & HUD) every coping grind, handplant and hang-up writes a \"Trainer transition:\" line to the log. The lean needs an Xbox-style "
             "(XInput) controller.");
    }
    card_end(p);

    if (card(menu, p, "transition-coping", "COPING GRINDS AND STALLS", "On the lip of a bowl, a pipe or a quarter")) {
        number_row(menu, callbacks, "Lean into the ramp", "transition lean", view.tr_lean, -1.0f, 1.0f, view.tr_lean == 0.0f ? "off" : "%.2f",
                   "skate. stands the skater straight up on the coping. With this the skater leans into the ramp, the more the faster the grind runs (nothing "
                   "below 1 m/s, all of it from 5 m/s), by showing the game your left stick pushed toward the ramp while you leave that stick alone. 1 is as "
                   "far as the game's lean goes without taking you off; Skate 3 feel uses 0.6. A negative number leans the other way.");
        number_row(menu, callbacks, "Roll off at your angle", "transition carry", view.tr_carry, 0.0f, 1.0f, view.tr_carry == 0.0f ? "off" : "x %.2f",
                   "skate. turns the board straight down the ramp when a coping grind ends without a pop. With this the speed along the coping is put back "
                   "for the first third of a second, so a fast grind rolls off at a shallow angle. 1 keeps all of it, 0 is off.");
        number_row(menu, callbacks, "Roll-off drop speed", "transition drop", view.tr_drop, 0.0f, 5.0f, view.tr_drop == 0.0f ? "no limit" : "%.2f m/s",
                   "While the speed along the coping is kept (the row above), the speed into the ramp is held to this. Skate 3 nudged the board in at 1 m/s "
                   "at most. 0 is no limit. Does nothing with the row above off.");
        settings(trainer::transition_card_coping);
        grind_switch("coping", "On, as the game has it, riding straight up into a coping does not lock onto it the old way.");
        grind_switch("linear", "Off, the game's older hold on the edge runs: the one the coping exit timers below belong to.");
        note("Exit timers: in Skate 3 a coping grind ended by itself, leaning further into the ramp until it rolled in. skate. has them off (-1). "
             "Skate 3: 0.1 s riding straight up into the coping, 0.8 s coming at it from below, 2 s on a curb.");
        rows(0);
    }
    card_end(p);

    if (card(menu, p, "transition-reentry", "RE-ENTRY AND HANG-UPS", "Coming back in off the coping, and over a spine")) {
        number_row(menu, callbacks, "No hang-ups", "transition unstick", view.tr_unstick, 0.0f, 5.0f, view.tr_unstick == 0.0f ? "off" : "x %.2f",
                   "Skate 3's rule for a hang-up: slower than 1.1 m/s for longer than a sixth of a second after leaving the coping (or landing an air off a "
                   "transition without riding away). The skater is then pushed into the ramp and down until moving again. The number is how hard; 0 is off.");
        number_row(menu, callbacks, "Coping re-catch delay", "transition lockout", view.tr_lockout, 0.0f, 3.0f, view.tr_lockout == 0.0f ? "off" : "%.2f s",
                   "After a coping grind ends without a pop the skater does not reach for an edge for this long (the reach rows below are held at 0, then "
                   "put back). Skate 3 waited 1.17 s. 0 is off.");
        bool spine = view.tr_spine;
        if (toggle_row(menu, "Airs clear the coping",
                       "An air that leaves a transition going up and over the lip does not reach for an edge until it has landed, so the far coping of a spine, "
                       "or the one it left, cannot catch the board. A carve along the lip into a coping grind is left alone; an air straight up cannot be landed "
                       "back on the coping as a grind either.",
                       spine, can_edit))
            trainer_command(menu, callbacks, spine ? "transition spine on" : "transition spine off");
        note("How far the skater reaches for an edge from the air, and what an edge is worth against the ground as a place to land:");
        rows(1);
    }
    card_end(p);

    if (card(menu, p, "transition-plant", "HANDPLANTS", "Where a plant lets you go")) {
        number_row(menu, callbacks, "Skate 3 plant exit", "transition plant", view.tr_plant, 0.0f, 1.0f, view.tr_plant == 0.0f ? "off" : "x %.2f",
                   "When a handplant ends the skater leaves the lip at 0.9 m/s or more into the ramp and falls from rest, as off the top of an arc. 1 is all the "
                   "way, 0 is off. If plants still come down at the bottom with this on, the plant itself carries the skater down first: the log line says how "
                   "far (\"lost inside the plant\").");
        settings(trainer::transition_card_plant);
        note("The game's own numbers for a plant: it looks for a wall to come down on in a fan from along the coping (0) to straight in (90), and leaves "
             "the lip between the two speed limits. Skate 3: fan to 69 degrees, 0.9 m/s out, up to 2 m/s along the coping.");
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
        note("A pop off a steep wall is straightened to go up the wall, tipped in or out by the stick; in the air the game picks a landing from a fan "
             "of directions by score. These are the numbers for both.");
        rows(4);
    }
    card_end(p);

    if (card(menu, p, "transition-pump", "PUMP POWER AND GAME SETTINGS", "How much a pump gives, and how much of it you do")) {
        number_row(menu, callbacks, "Pump power", "option pump_power", view.pump_power, 0.0f, 10.0f, "x %.2f",
                   "How much speed pumping a transition gives. 1 is the game's own; 2 gives about one more of the game's pumps on top of each of yours, 10 "
                   "about nine more, under 1 takes speed away. The game still decides when you are pumping. It is found about 15 seconds after a level loads.");
        if (view.pump_blocked) warn("Off in this multiplayer game: the host's rules apply here.");
        else if (view.pump_power != 1.0f && !view.pump_live) note("Finding the game's pumping in memory: it acts about 15 seconds after a level loads.");
        note("GAME SETTINGS: the next three are your game's own, shown as the game holds them. Change them in its Settings menu; no preset or reset "
             "touches them and they never count as MODIFIED.");
        // The game's own options: shown only, and not dimmed with the rest, since they are the player's in any session.
        ImGui::EndDisabled();
        for (std::size_t i = 0; i < trainer::transition_options.size(); ++i) {
            const auto &one = trainer::transition_options[i];
            info(menu, std::string(one.label).c_str(), one.boolean ? std::string(view.tr_option[i] != 0 ? "On" : "Off") : std::to_string(view.tr_option[i]));
        }
        ImGui::BeginDisabled(!can_edit);
        settings(trainer::transition_card_pump);
        // The first seven rows are the older pumping code's numbers: skate.'s newer pumping does not read them, so they
        // are only shown while Legacy pumping is held on (or to put back one that was changed).
        if (setting_forced_on("legacypump")) {
            note("Legacy pumping is on: these are its numbers. Skate 3: strength 18, limit 10, 0.7 of a pump given without pumping.");
            rows(5, 0, 7);
        } else {
            note("Pump power works with the game's own pumping. Hold Legacy pumping on for Skate 3's pumping and its strength and limit rows.");
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
        bool hud = view.hud, jump = view.hud_jump;
        if (toggle_row(menu, "Speed and air HUD", "Speed, air time and height in the corner while you skate.", hud))
            trainer_command(menu, callbacks, std::format("option hud {}", hud ? 1 : 0));
        if (toggle_row(menu, "Jump read-out", "After each landing: takeoff speed and angle, height, distance, landing speed.", jump))
            trainer_command(menu, callbacks, std::format("option hud_jump {}", jump ? 1 : 0));
#if defined(DINGOSDK_DEV_TOOLS)
        // (`trainer option log`, a research aid: development builds only.)
        bool logging = view.logging;
        if (toggle_row(menu, "Record telemetry", "Write position and speed every frame to a CSV in %LOCALAPPDATA%\\ReSkate\\trainer\\telemetry.", logging))
            trainer_command(menu, callbacks, std::format("option log {}", logging ? 1 : 0));
#endif
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
        if (card(menu, p, "last-jump", "LAST JUMP", std::format("Jump {}", telemetry.last.serial).c_str())) jump_lines(menu, telemetry.last);
        card_end(p);
    }
    if (telemetry.best.serial && telemetry.best.serial != telemetry.last.serial) {
        if (card(menu, p, "best-jump", "LONGEST JUMP ON THIS MAP")) jump_lines(menu, telemetry.best);
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
            if (button_row(menu, "spot", {{("GO TO " + view.spots[i].name).c_str()}}) == 0) trainer_command(menu, callbacks, std::format("spot {}", i + 1));
            ImGui::PopID();
        }
        if (view.map_note.empty() && view.map_preset.empty() && view.spots.empty())
            note("Map makers can ship a trainer.json in their mod folder with spots and a recommended preset.");
    }
    card_end(p);

    note("Presets, your own and Play like Skate 3 are on TUNE, in its first section.");
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
    // Presets: the first section of TUNE, where MY PRESETS opens.
    constexpr int sections[]{-1, trainer::topic_game, -1, -1, -1, -1, trainer::topic_none, -1, trainer::topic_trickline, trainer::topic_transition, trainer::topic_pop,
                             trainer::topic_speed, trainer::topic_bails, trainer::topic_grinds, trainer::topic_pumping, trainer::topic_foot, trainer::topic_trickline,
                             trainer::topic_pop, trainer::topic_game};
    if (tab == 1) p.want_presets = true;
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
    // A session that sets everyone's physics: said first, in its own card's large title, so nobody tunes a game that
    // ignores it (the owner's ask, 2026-10-24). The card does not fold away.
    if (view->session_stock || view->session_enforced) {
        begin_card(menu, "server-physics", view->session_stock ? "MATCH PHYSICS: THE GAME'S OWN" : "SERVER PHYSICS IN USE");
        warn(view->session_stock ? "This match skates on the game's own physics for everyone. Your trainer settings, presets and boosts do nothing "
                                   "until the match ends; they are kept for afterwards."
                                 : "This multiplayer game's host or server sets everyone's physics. Your trainer settings, presets and boosts do "
                                   "nothing here; you skate with theirs.");
        if (view->session_enforced) note("HUD and markers still work. Your own setup comes back when you leave.");
        end_card();
    }
    if (!view->session_enforced && view->boosts_blocked) {
        begin_card(menu, "session-note", "BOOSTS ARE OFF");
        warn("The host has turned boosts off: trick sliders, flip speed, auto push and the board bending boost do nothing here.");
        note("The physics values still apply. The rest comes back when the host allows boosts or you leave.");
        end_card();
    }
    // Everything that changes how the game plays, said in words. The first parts are the trainer's own; the cheats
    // and the game speed are ReSkate's and count all the same.
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
    // STOCK means nothing here changes how the game plays: the player's own game settings never count, nor does a
    // locked value that is still the game's own.
    const auto locks = view->locks ? std::format("  ({} locked value{})", view->locks, view->locks == 1 ? "" : "s") : std::string{};
    ImGui::PushTextWrapPos(0.0f);
    if (view->session_enforced) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "HOST'S PHYSICS: your own setup is kept for when you leave.");
    else if (!view->ready && what.empty()) note("The game as it shipped: every value, lock, preset and setting.");
    else if (what.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "STOCK: the game is exactly as it shipped.%s", locks.c_str());
    else ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "MODIFIED: %s.%s", what.c_str(), locks.c_str());
    // The search for the game's tuning values reads all of the game's memory on a thread of its own: values set meanwhile
    // reach the game when it ends, and on a machine short of memory the game can stutter while it runs.
    if (const auto ms = trainer::class_search_ms())
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Finding the game's tuning values... %llu s. Changes made meanwhile reach the game when it is done.",
                           static_cast<unsigned long long>(ms / 1000));
    ImGui::PopTextWrapPos();
    // A guest under the host's physics would only wipe a setup they cannot see at work here.
    if (button_row(menu, "reset-everything",
                   {{"RESET EVERYTHING", callbacks.queue_console_command && !view->session_enforced, false,
                     view->session_enforced ? "Off in this multiplayer game: the host controls physics here. Your own setup is kept for when you leave."
                                            : "The game exactly as it shipped: every value, lock, preset, dial and trick, grind, transition and bending setting; No "
                                              "Bail, Noclip, Dark Pop, Revert Boost, grind exit triggers and Board Break off, the boosts at 0, normal game speed, and "
                                              "no preset loading on this map. Your saved presets and your game's own settings are kept."}}) == 0) {
        clear_edits(p);
        trainer_command(menu, callbacks, "reset everything");
        // The cheats and the game speed change how the game plays too: off with the rest.
        for (const auto &line : cheats) cheat_off(menu, callbacks, line);
        if (callbacks.queue_debug && model.debug.probe) debug_request(menu, callbacks, {DebugAction::set_probe_enabled, false});
        if (std::abs(speed - 1.0) > 1e-3) send_console(menu, callbacks, "reset SimulationTime.TimeScale");
    }
    // `preset export` leaves its line here; the clipboard belongs to this thread.
    if (!p.share_seen) {
        p.share_seen = true;
        p.share_serial = view->share_serial;
    } else if (p.share_serial != view->share_serial) {
        p.share_serial = view->share_serial;
        ImGui::SetClipboardText(view->share_text.c_str());
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
