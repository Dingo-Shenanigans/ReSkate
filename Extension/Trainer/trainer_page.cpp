#include "trainer_page.h"
#include "trainer.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

// The TRAINER page and the skating HUD. Presentation thread: reads the trainer's snapshots
// and queues `trainer ...` console commands; it never touches the game.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
    int tab{}; // opens on TUNE: the dials and switches
    std::array<char, 96> search{};
    int mode{1}; // the Tune tab: 0 Realistic, 1 Fun (two short lists), 2 Everything
    int group{1}; // Everything: 1: every group, 2..: one of the view's groups
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
    float hippy_edit{1}, nocomply_edit{1}, boneless_edit{1}, offboard_edit{1}, flip_edit{1};
    bool hippy_editing{}, nocomply_editing{}, boneless_editing{}, offboard_editing{}, flip_editing{};
    std::uint64_t open_serial{}; // the last `trainer open` acted on
    bool show_page{};
};
void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view);
Page &page() {
    static auto *value = new Page;
    return *value;
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

void filter_rows(Page &p, const trainer::View &view) {
    const auto words = lower(p.search.data());
    const auto key = std::format("{}|{}|{}|{}|{}|{}", words, p.group, p.only_changed, p.graph_points, p.show_unused, p.mode);
    if (p.shown_revision == view.revision && p.shown_key == key) return;
    p.shown_revision = view.revision;
    p.shown_key = key;
    p.shown.clear();
    p.hidden_unused = 0;
    const std::string *group = p.mode == 2 && p.group > 1 && static_cast<std::size_t>(p.group) <= view.groups.size() + 1
        ? &view.groups[static_cast<std::size_t>(p.group) - 2] : nullptr;
    const bool essentials = p.mode < 2;
    const std::uint8_t list = p.mode == 0 ? trainer::mode_realistic : trainer::mode_fun;
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const auto &row = view.rows[i];
        if (essentials) {
            // Only what the game was found or seen to read: a short list has no room for maybes.
            if (!row.friendly.empty() && (row.modes & list) && row.used) p.shown.push_back(i);
            continue;
        }
        if (row.detail && !p.graph_points && !row.touched) continue;
        if (p.only_changed && !row.touched && !row.frozen) continue;
        // Searching looks through every group.
        if (words.empty() || p.mode < 2 ? (group && row.group != *group) : !contains_words(lower(row.id + " " + row.label + " " + row.friendly), words))
            continue;
        // A value nothing in the game reads would be a slider that does nothing.
        if (!row.used && !p.show_unused && !row.touched && !row.frozen) {
            ++p.hidden_unused;
            continue;
        }
        p.shown.push_back(i);
    }
    if (essentials) std::ranges::sort(p.shown, {}, [&](std::size_t i) { return view.rows[i].rank; });
}

void value_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view, const trainer::Row &row,
               bool with_group, bool friendly) {
    ImGui::PushID(row.id.c_str());
    bool frozen = row.frozen;
    if (ImGui::Checkbox("##freeze", &frozen)) trainer_command(menu, callbacks, std::format("freeze {} {}", row.id, frozen ? 1 : 0));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock: presets and Reset values leave this value alone.\nReset everything (top of the page) clears locks too.\nYou do not need it for a change to apply.");
    ImGui::SameLine();
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.52f;
    const auto label = friendly && !row.friendly.empty() ? row.friendly : with_group ? row.group + " / " + row.label : row.label;
    const bool tinted = row.touched || !row.used;
    if (tinted) ImGui::PushStyleColor(ImGuiCol_Text, row.used ? skate_theme::blue : IM_COL32(150, 150, 150, 255));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.used ? label.c_str() : (label + "  (no use found)").c_str());
    if (tinted) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nStock: %s%s", row.id.c_str(), number(row.stock).c_str(),
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
        const double low = scale ? 0.0 : -1.0e6, high = 1.0e6;
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
    if (edited) trainer_command(menu, callbacks, std::format("set {} {}", row.id, number(value)));
    if (row.touched && row.kind != trainer::Kind::flag) {
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(reset, 0))) trainer_command(menu, callbacks, "reset " + row.id);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// A built-in preset as a switch: blue while every value it sets holds what it sets.
void preset_switch(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::PresetRow &preset) {
    if (preset.active) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button(preset.name.c_str());
    if (preset.active) ImGui::PopStyleColor();
    if (pressed) trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + preset.name);
    if (ImGui::IsItemHovered() && !preset.note.empty()) ImGui::SetTooltip("%s", preset.note.c_str());
}
// A built-in preset as a dial: one multiplier that moves everything the preset moves.
void dial_row(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::PresetRow &preset) {
    ImGui::PushID(preset.name.c_str());
    const float column = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.40f;
    const bool moved = std::abs(preset.factor - 1.0) > 1e-4;
    if (moved) ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(preset.title.c_str());
    if (moved) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n1 is the game's own; \"%s\" is x %s.", preset.note.c_str(), preset.name.c_str(), number(preset.amount).c_str());
    ImGui::SameLine(column);
    const float button = ImGui::CalcTextSize("Fast Parkour Flips").x + ImGui::GetStyle().FramePadding.x * 2;
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    const auto id = "dial:" + preset.name;
    double value = p.active == id ? p.active_value : preset.factor;
    // A preset that turns its value down gets a dial with more room below 1.
    const double low = preset.amount < 1 ? 0.05 : 0.2, high = preset.amount < 1 ? 3.0 : 10.0;
    const float box = px(64);
    ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - button - reset - ImGui::GetStyle().ItemSpacing.x * 3));
    if (ImGui::SliderScalar("##dial", ImGuiDataType_Double, &value, &low, &high, "x %.2f", ImGuiSliderFlags_Logarithmic) && value > 0)
        trainer_command(menu, callbacks, std::format("dial {} {}", number(value), preset.name));
    if (ImGui::IsItemActive()) {
        p.active = id;
        p.active_value = value;
    } else if (p.active == id) {
        p.active.clear();
    }
    ImGui::SameLine();
    // Any number at all: the slider only covers the sensible range.
    double typed = preset.factor;
    ImGui::SetNextItemWidth(box);
    if (ImGui::InputDouble("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && typed > 0)
        trainer_command(menu, callbacks, std::format("dial {} {}", number(typed), preset.name));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type any multiplier and press Enter: the slider's ends are not a limit.");
    ImGui::SameLine();
    if (preset.active) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
    const bool pressed = ImGui::Button(preset.name.c_str(), ImVec2(button, 0));
    if (preset.active) ImGui::PopStyleColor();
    if (pressed) trainer_command(menu, callbacks, std::string(preset.active ? "preset remove " : "preset apply ") + preset.name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preset.note.c_str());
    if (moved) {
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(reset, 0))) trainer_command(menu, callbacks, "dial 1 " + preset.name);
    }
    ImGui::PopID();
}

// One screen for everything that changes how the game plays: the presets as dials and
// switches, the trick multipliers, the player's own presets and the values themselves. All of
// them show the same values, so none can disagree with another.
void tune_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    if (!view.ready) {
        begin_card(menu, "tune-wait", "PHYSICS TUNING");
        note(view.status.c_str());
        note("The values appear once a level is loaded.");
        end_card();
        return;
    }
    if (!view.editable) warn(view.blocked.c_str());
    // Two short lists and the whole table.
    {
        static constexpr const char *modes[]{"REALISTIC", "FUN", "EVERYTHING"};
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const bool on = p.mode == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
            if (ImGui::Button(modes[i], ImVec2(width, 0))) p.mode = i;
            if (on) ImGui::PopStyleColor();
        }
    }
    const std::uint8_t list = p.mode == 0 ? trainer::mode_realistic : p.mode == 1 ? trainer::mode_fun : std::uint8_t{0xff};
    const bool can_edit = view.editable && callbacks.queue_console_command != nullptr;

    begin_card(menu, "dials", p.mode == 0 ? "FEEL: TONE IT DOWN" : p.mode == 1 ? "FEEL: TURN IT UP" : "FEEL",
               "1 is the game's own. A dial moves every value its preset moves.");
    ImGui::BeginDisabled(!can_edit);
    for (const auto &preset : view.presets)
        if (preset.builtin && preset.dial && (p.mode == 2 || (preset.modes & list))) dial_row(menu, callbacks, p, preset);
    // The presets that are plain switches, and the ones that set several dials at once.
    bool first = true;
    for (const auto &preset : view.presets) {
        if (!preset.builtin || preset.dial || preset.name == "Stock" || !(p.mode == 2 || (preset.modes & list))) continue;
        if (!first && ImGui::GetItemRectMax().x + ImGui::CalcTextSize(preset.name.c_str()).x + ImGui::GetStyle().FramePadding.x * 4 <
                          ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x)
            ImGui::SameLine();
        first = false;
        preset_switch(menu, callbacks, preset);
    }
    ImGui::EndDisabled();
    if (p.mode != 0) {
        // The game's own bail protection, the same switch as Skater > Movement.
        const auto &debug = model.debug;
        bool no_bail = debug.no_bail;
        if (toggle_row(menu, "Never bail", "No wipeouts at all. Recover from a bail before switching it on.", no_bail,
                (debug.no_bail_available || debug.no_bail) && callbacks.queue_debug != nullptr))
            debug_request(menu, callbacks, {DebugAction::set_no_bail, no_bail});
    }
    ImGui::BeginDisabled(!can_edit || !view.touched);
    if (ImGui::Button("Reset values")) trainer_command(menu, callbacks, "reset all");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every tuning value back to the game's own. Locked values stay.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu values changed", view.touched);
    end_card();

    ImGui::BeginDisabled(!can_edit);
    trick_heights(menu, callbacks, p, view);
    ImGui::EndDisabled();

    if (p.mode == 2) {
        begin_card(menu, "preset-save", "YOUR PRESETS", "Save what is changed right now");
        ImGui::BeginDisabled(!can_edit);
        const float button = ImGui::CalcTextSize("Turn off").x + ImGui::GetStyle().FramePadding.x * 2;
        for (const auto &preset : view.presets) {
            if (preset.builtin) continue;
            ImGui::PushID(preset.name.c_str());
            if (preset.active) {
                ImGui::PushStyleColor(ImGuiCol_Button, skate_theme::blue);
                if (ImGui::Button("Turn off", ImVec2(button, 0))) trainer_command(menu, callbacks, "preset remove " + preset.name);
                ImGui::PopStyleColor();
            } else if (ImGui::Button("Turn on", ImVec2(button, 0))) {
                trainer_command(menu, callbacks, "preset apply " + preset.name);
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(preset.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", preset.note.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - px(120));
            if (ImGui::SmallButton("This map")) trainer_command(menu, callbacks, "profile set " + preset.name);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) trainer_command(menu, callbacks, "preset delete " + preset.name);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        field(menu, "Name");
        const float save = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - save - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##preset-name", p.preset_name.data(), p.preset_name.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(!p.preset_name[0] || !view.touched);
        if (ImGui::Button("Save", ImVec2(save, 0))) trainer_command(menu, callbacks, std::string("preset save ") + p.preset_name.data());
        ImGui::EndDisabled();
        if (!view.map.empty()) {
            info(menu, "This map applies", view.profile_preset.empty() ? "nothing" : view.profile_preset);
            if (!view.profile_preset.empty() && ImGui::Button("Stop applying it on this map")) trainer_command(menu, callbacks, "profile clear");
            note("\"This map\" beside one of your presets applies it every time this map loads.");
        }
        end_card();
    }

    // The values themselves: the short list's, or all of them.
    ImGui::Spacing();
    ImGui::SeparatorText(p.mode == 2 ? "EVERY VALUE" : "FINE TUNING");
    if (p.mode == 2) {
        // The list includes the game's tuning classes, which the trainer has to find in memory
        // before an edit to one does anything: with the list open, it looks now.
        trainer::note_class_list_shown();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::InputTextWithHint("##search", "Search every value...", p.search.data(), p.search.size());
        ImGui::SameLine();
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
        ImGui::Checkbox("Only what I changed", &p.only_changed);
        ImGui::SameLine();
        ImGui::Checkbox("Graph points", &p.graph_points);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show every point of the tuning graphs, not just one multiplier per graph.");
        ImGui::SameLine();
        ImGui::Checkbox("Values with no use found", &p.show_unused);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Also list the tuning values that the game was never found or seen reading.\nChanging those will probably do nothing.");
    } else {
        note("The same values the dials move, one at a time. The box on the left locks a value against presets, dials and Reset values.");
    }
    filter_rows(p, view);
    if (p.mode == 2) {
        ImGui::SameLine();
        if (p.hidden_unused) ImGui::TextDisabled("%zu shown, %zu changed, %zu hidden", p.shown.size(), view.touched, p.hidden_unused);
        else ImGui::TextDisabled("%zu shown, %zu changed", p.shown.size(), view.touched);
    }
    const bool with_group = p.mode == 2 && (p.search[0] != 0 || p.group <= 1 || p.only_changed);
    const bool friendly = p.mode < 2;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(p.shown.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            value_row(menu, callbacks, p, view, view.rows[p.shown[static_cast<std::size_t>(i)]], with_group, friendly);
    if (p.shown.empty()) note("Nothing matches.");
}

void trick_heights(SkateMenu &menu, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    begin_card(menu, "trick-heights", "TRICKS", "1.0 is the game's own");
    // `limited`: the game itself stops at the slider's end, so a typed number is held to it too.
    const auto slider = [&](const char *label, const char *option, float value, float &edit, bool &editing, float high = 50.0f, bool limited = false) {
        field(menu, label);
        ImGui::PushID(option);
        float shown = editing ? edit : value;
        const float box = px(64);
        ImGui::SetNextItemWidth(std::max(px(70), ImGui::GetContentRegionAvail().x - box - ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::SliderFloat("##height", &shown, 0.1f, high, "x %.2f", (limited ? ImGuiSliderFlags_AlwaysClamp : 0) | ImGuiSliderFlags_Logarithmic)) {
            edit = shown;
            editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option {} {:.2f}", option, edit));
        if (!ImGui::IsItemActive() && editing && std::abs(value - edit) < 0.005f * std::max(1.0f, std::abs(edit))) editing = false;
        ImGui::SameLine();
        float typed = value;
        ImGui::SetNextItemWidth(box);
        if (ImGui::InputFloat("##typed", &typed, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue) && typed > 0) {
            editing = false;
            trainer_command(menu, callbacks, std::format("option {} {}", option, typed));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(limited ? "Type a multiplier and press Enter." : "Type any multiplier and press Enter: the slider's end is not a limit.");
        ImGui::PopID();
    };
    slider("Flip trick speed", "flip_speed", view.flip_speed, p.flip_edit, p.flip_editing, 3.0f, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slows board flips. Above 1 the game's own limit on how fast a board turns takes over.");
    slider("No comply height", "nocomply_height", view.nocomply_height, p.nocomply_edit, p.nocomply_editing);
    slider("Boneless height", "boneless_height", view.boneless_height, p.boneless_edit, p.boneless_editing);
    slider("Hippy jump height", "hippy_height", view.hippy_height, p.hippy_edit, p.hippy_editing);
    slider("Off-board jump height", "offboard_height", view.offboard_height, p.offboard_edit, p.offboard_editing);
    if (ImGui::Button("Reset tricks")) {
        p.hippy_editing = p.nocomply_editing = p.boneless_editing = p.offboard_editing = p.flip_editing = false;
        trainer_command(menu, callbacks, "reset tricks");
    }
    note("Not tuning values: the game sets these in its trick scripts and flip curves. Type in the box to go past a slider's end.");
    end_card();
}

void practice_tab(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks, Page &p, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    begin_card(menu, "speed", "GAME SPEED", "Slow motion for learning a line");
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
            field(menu, "Speed");
            auto value = static_cast<float>(current);
            if (ImGui::SliderFloat("##game-speed", &value, 0.05f, 2.0f, current == 0.0 ? "paused" : "%.2fx", ImGuiSliderFlags_AlwaysClamp))
                set(value);
            if (ImGui::Button(current == 0.0 ? "Resume" : "Pause")) set(current == 0.0 ? 1.0 : 0.0);
            for (const auto quick : {0.1, 0.25, 0.5, 0.75, 1.0}) {
                ImGui::SameLine();
                if (ImGui::Button(std::format("{}x", number(quick)).c_str())) set(quick);
            }
            note("The jump read-out measures real time, so it reads low while the game is slowed.");
        }
    }
    end_card();

    begin_card(menu, "markers", "MARKERS", "Saved for each map");
    int slot = view.slot;
    if (choice(menu, "marker-slot", slot, {"1", "2", "3", "4", "5"})) trainer_command(menu, callbacks, std::format("slot {}", slot + 1));
    const auto &marker = view.markers[static_cast<std::size_t>(std::clamp(view.slot, 0, static_cast<int>(trainer::marker_slots) - 1))];
    info(menu, "Selected", marker.set ? std::format("{:.1f}, {:.1f}, {:.1f}", marker.position[0], marker.position[1], marker.position[2]) : "empty");
    ImGui::BeginDisabled(view.map.empty());
    ImGui::BeginDisabled(!telemetry.skater);
    if (ImGui::Button("Save here")) trainer_command(menu, callbacks, "marker save");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!marker.set);
    if (ImGui::Button("Go")) trainer_command(menu, callbacks, "marker go");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) trainer_command(menu, callbacks, "marker clear");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    bool auto_return = view.auto_return;
    if (toggle_row(menu, "Return after a bail", "Go back to the selected marker when the skater wipes out.", auto_return))
        trainer_command(menu, callbacks, std::format("option auto_return {}", auto_return ? 1 : 0));
    field(menu, "Return delay");
    float delay = view.return_delay;
    ImGui::SliderFloat("##return-delay", &delay, 0.0f, 10.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) trainer_command(menu, callbacks, std::format("option return_delay {:.1f}", delay));
    bool pad = view.pad_shortcuts;
    if (toggle_row(menu, "Controller shortcuts", "Hold LB + RB, then D-pad: up saves, down goes, left / right pick the slot.", pad))
        trainer_command(menu, callbacks, std::format("option pad {}", pad ? 1 : 0));
    end_card();

    begin_card(menu, "teleport", "TELEPORT");
    // The waypoint line is why the map's registry is read at all: only while this is drawn.
    trainer::note_teleport_card_shown();
    info(menu, "Map waypoint", view.waypoint_set ? std::format("{:.1f}, {:.1f}, {:.1f}", view.waypoint[0], view.waypoint[1], view.waypoint[2])
                                                 : "none - place one on the pause map");
    ImGui::BeginDisabled(!view.waypoint_set || !telemetry.skater);
    if (ImGui::Button("Go to map waypoint")) trainer_command(menu, callbacks, "waypoint");
    ImGui::EndDisabled();
    if (telemetry.skater)
        info(menu, "You are at", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
    field(menu, "X, Y, Z");
    ImGui::InputFloat3("##teleport", p.teleport.data(), "%.2f");
    ImGui::BeginDisabled(!telemetry.skater);
    if (ImGui::Button("Here")) p.teleport = telemetry.position;
    ImGui::SameLine();
    if (ImGui::Button("Go##teleport"))
        trainer_command(menu, callbacks, std::format("tp {:.3f} {:.3f} {:.3f}", p.teleport[0], p.teleport[1], p.teleport[2]));
    ImGui::SameLine();
    if (ImGui::Button("Copy position"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy for Blender"))
        ImGui::SetClipboardText(std::format("{:.3f}, {:.3f}, {:.3f}", telemetry.position[0], -telemetry.position[2], telemetry.position[1]).c_str());
    ImGui::EndDisabled();
    end_card();
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
void map_tab(SkateMenu &menu, const CallbacksV3 &callbacks, const trainer::View &view) {
    const auto telemetry = trainer::telemetry();
    begin_card(menu, "hud", "HUD");
    bool hud = view.hud, jump = view.hud_jump, logging = view.logging;
    if (toggle_row(menu, "Speed and air HUD", "Speed, air time and height in the corner while you skate.", hud))
        trainer_command(menu, callbacks, std::format("option hud {}", hud ? 1 : 0));
    if (toggle_row(menu, "Jump read-out", "After each landing: takeoff speed and angle, height, distance, landing speed.", jump))
        trainer_command(menu, callbacks, std::format("option hud_jump {}", jump ? 1 : 0));
    if (toggle_row(menu, "Record telemetry", "Write position and speed every frame to a CSV in %LOCALAPPDATA%\\ReSkate\\trainer\\telemetry.",
            logging))
        trainer_command(menu, callbacks, std::format("option log {}", logging ? 1 : 0));
    end_card();

    begin_card(menu, "now", "RIGHT NOW");
    if (!telemetry.skater) {
        note("No skater yet.");
    } else {
        info(menu, "Speed", std::format("{:.1f} km/h ({:.2f} m/s), top {:.1f} km/h", telemetry.speed * 3.6f, telemetry.speed, telemetry.top_speed * 3.6f));
        info(menu, "Position", std::format("{:.2f}, {:.2f}, {:.2f}", telemetry.position[0], telemetry.position[1], telemetry.position[2]));
        info(menu, "Heading", std::format("{:.0f} degrees", telemetry.heading));
    }
    end_card();

    if (telemetry.last.serial) {
        begin_card(menu, "last-jump", "LAST JUMP", std::format("Jump {}", telemetry.last.serial).c_str());
        jump_lines(menu, telemetry.last);
        end_card();
    }
    if (telemetry.best.serial && telemetry.best.serial != telemetry.last.serial) {
        begin_card(menu, "best-jump", "LONGEST JUMP ON THIS MAP");
        jump_lines(menu, telemetry.best);
        end_card();
    }

    begin_card(menu, "map", "THIS MAP", view.map.empty() ? "No level loaded" : view.map.c_str());
    if (!view.map_note.empty()) note(view.map_note.c_str());
    if (!view.map_preset.empty()) {
        info(menu, "Author's preset", view.map_preset);
        ImGui::BeginDisabled(!view.editable);
        if (ImGui::Button("Apply the author's preset")) trainer_command(menu, callbacks, "preset apply map");
        ImGui::EndDisabled();
    }
    for (std::size_t i = 0; i < view.spots.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button("Go")) trainer_command(menu, callbacks, std::format("spot {}", i + 1));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(view.spots[i].name.c_str());
        ImGui::PopID();
    }
    if (view.map_note.empty() && view.map_preset.empty() && view.spots.empty())
        note("Map makers can ship a trainer.json in their mod folder with spots and a recommended preset.");
    end_card();
}
} // namespace

bool trainer_take_open() {
    const auto view = trainer::view();
    auto &p = page();
    if (view->open_serial == p.open_serial) return false;
    p.open_serial = view->open_serial;
    // `trainer open` still takes "presets": they live on the Tune tab now.
    p.tab = view->open_tab >= 4 ? 0 : std::clamp(view->open_tab - 1, 0, 2);
    if (view->open_tab >= 4) p.mode = std::clamp(view->open_tab - 4, 0, 2);
    p.show_page = true;
    return true;
}
bool trainer_page_wanted() {
    auto &p = page();
    const bool wanted = p.show_page;
    p.show_page = false;
    return wanted;
}
void trainer_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto view = trainer::view();
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    if (ImGui::Button("RESET EVERYTHING")) {
        p.hippy_editing = p.nocomply_editing = p.boneless_editing = p.offboard_editing = p.flip_editing = false;
        trainer_command(menu, callbacks, "reset everything");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("The game as it shipped: every value, lock, preset and trick slider.");
    category_tabs(menu, p.tab, {"TUNE", "PRACTICE", "MAP & HUD"}, "trainer-tabs");
    ImGui::PushID(p.tab);
    ImGui::BeginChild("trainer-tab", ImVec2(0, page_body_height(menu)));
    switch (p.tab) {
    case 0: tune_tab(menu, model, callbacks, p, *view); break;
    case 1: practice_tab(menu, model, callbacks, p, *view); break;
    default: map_tab(menu, callbacks, *view); break;
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
