#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

// The PROGRESS page: missions, challenges and bus stops as rows, and the profile's score and districts.
namespace dingosdk::overlay::menu {
using namespace theme;
void progression_request(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& arguments) {
    std::array<char, 512> result{};
    const auto command = "progression " + arguments;
    const bool queued = callbacks.queue_console_command &&
        callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
    result.back() = '\0';
    feedback(menu, result[0] ? result.data() : queued ? "" : "Change unavailable.");
}

namespace {
std::string readable(std::string_view id) {
    std::string text;
    for (const char c : id) {
        const char out = c == '_' || c == '-' || c == '.' ? ' ' : c;
        if (out == ' ' && (text.empty() || text.back() == ' ')) continue;
        text += out;
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text.empty() ? std::string(id) : text;
}
bool contains(std::string_view value, std::string_view query) {
    return std::search(value.begin(), value.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    }) != value.end();
}
// A long list (hundreds of missions and challenges) through a clipper, so only the rows in view are
// laid out. Every row is as tall as the first, which the clipper measures, except an open one: that
// is drawn on its own between two clippers. `open` is its index, -1 for none.
template<class Draw> void clipped_rows(int count, int open, Draw&& draw) {
    const auto run = [&](int first, int last) {
        if (first >= last) return;
        ImGuiListClipper clipper;
        clipper.Begin(last - first);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw(first + i);
    };
    if (open < 0 || open >= count) {
        run(0, count);
        return;
    }
    run(0, open);
    draw(open);
    run(open + 1, count);
}
// A box to find something by name, for a keyboard; a controller passes over it to the rows.
void search_box(const char* id, const char* hint, std::array<char, 128>& text) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint(id, hint, text.data(), text.size());
}
} // namespace

// A row a bus stop: left / right (or A) steps it through hidden, locked and unlocked.
void bus_stops(SkateMenu& menu, const ProgressionModel& model, const CallbacksV3& callbacks) {
    constexpr const char* maps[]{"All maps", "San Vansterdam", "Isle of Grom", "Super Ultra Mega Resort"};
    static const std::vector<std::string> map_names{"All maps", "San Vansterdam", "Isle of Grom", "Mega Resort"};
    option_row(menu, "Map", "Left / right for one map's bus stops.", menu.bus_stop_map, map_names);
    std::vector<const BusStopRow*> rows;
    for (const auto& row : model.bus_stops)
        if (!menu.bus_stop_map || static_cast<int>(row.world) + 1 == menu.bus_stop_map) rows.push_back(&row);
    note((std::to_string(rows.size()) + " bus stops, saved locally. Reload the level after edits to refresh their icons.").c_str());
    ImGui::BeginChild("bus-stop-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_NavFlattened);
    static const std::vector<std::string> states{"Hidden", "Locked", "Unlocked"};
    const bool can = callbacks.queue_console_command != nullptr;
    clipped_rows(static_cast<int>(rows.size()), -1, [&](int i) {
        const auto& row = *rows[static_cast<std::size_t>(i)];
        const auto map = static_cast<std::size_t>(row.world) + 1;
        ImGui::PushID(static_cast<int>(row.number));
        int state = !row.visible ? (row.unlocked ? -1 : 0) : row.unlocked ? 2 : 1;
        // The map is in the name while every map is listed.
        char title[96];
        if (menu.bus_stop_map) std::snprintf(title, sizeof(title), "Bus stop %03u###state", row.number);
        else std::snprintf(title, sizeof(title), "Bus stop %03u  /  %s###state", row.number, map_names[map].c_str());
        const auto hint = std::string(maps[map]) + ". Left / right: hidden, locked or unlocked.";
        if (option_row(menu, title, hint.c_str(), state, states, can))
            progression_request(menu, callbacks, "busstop " + std::to_string(row.number) + " " + std::to_string(state));
        ImGui::PopID();
    });
    if (rows.empty()) note("No bus stops on this map.");
    ImGui::EndChild();
}

// A row a challenge, its goals at the right end; A opens what it holds: attempts and the goals done.
void challenge_progress(SkateMenu& menu, const ProgressionModel& model, const CallbacksV3& callbacks) {
    bool shown = !model.challenges_hidden;
    if (toggle_row(menu, "Show challenges", "Show challenges in the world, on the compass and on the map. Saved progress is kept.",
            shown, model.challenges_enabled && callbacks.queue_console_command))
        progression_request(menu, callbacks, shown ? "challenges 1" : "challenges 0");
    static const std::vector<std::string> types{"All types", "OTS", "Slam", "Line", "Session", "Stunt", "OTL", "Speedline"};
    option_row(menu, "Type", "Left / right for one type of challenge.", menu.challenge_type, types);
    search_box("##challenge-search", "Search challenges...", menu.challenge_search);
    std::vector<const ChallengeProgressRow*> rows;
    const std::string_view query(menu.challenge_search.data());
    for (const auto& row : model.challenges) {
        if (menu.challenge_type && row.type != types[static_cast<std::size_t>(menu.challenge_type)]) continue;
        if (!query.empty() && !contains(row.id, query) && !contains(readable(row.id), query)) continue;
        rows.push_back(&row);
    }
    std::size_t done{};
    for (const auto* row : rows) done += row->goal_count && row->completed_goals.size() >= row->goal_count;
    note((std::to_string(rows.size()) + " challenges, " + std::to_string(done) + " fully complete, " +
          (!model.challenges_enabled ? "local challenges disabled" : model.challenges_hidden ? "hidden" : "saved locally") + ".").c_str());
    ImGui::BeginChild("challenge-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_NavFlattened);
    static std::string open_challenge;
    int open = -1;
    for (std::size_t i = 0; i < rows.size() && !open_challenge.empty(); ++i)
        if (rows[i]->id == open_challenge) open = static_cast<int>(i);
    clipped_rows(static_cast<int>(rows.size()), open, [&](int i) {
        const auto& row = *rows[static_cast<std::size_t>(i)];
        ImGui::PushID(row.id.c_str());
        // A fully complete challenge has the blue edge.
        const bool complete = row.goal_count && row.completed_goals.size() >= row.goal_count;
        std::string detail = row.type;
        if (!row.neighborhood.empty()) detail += "  /  " + readable(row.neighborhood);
        if (!row.available) detail += "  /  unavailable";
        const auto goals = std::to_string(row.completed_goals.size()) + (row.goal_count ? " / " + std::to_string(row.goal_count) : std::string()) +
                           " goals";
        if (expand_row(menu, row.id.c_str(), readable(row.id), detail, goals, open_challenge, complete)) {
            ImGui::Indent(px(40));
            info(menu, "Attempts", std::to_string(row.attempts));
            info(menu, "Id", row.id);
            if (row.completed_goals.empty()) {
                note("No goal completed yet.");
            } else {
                note("Completed goals");
                for (const auto& goal : row.completed_goals) ImGui::BulletText("%s", goal.c_str());
            }
            ImGui::Unindent(px(40));
            ImGui::Dummy(ImVec2(0, px(4)));
        }
        ImGui::PopID();
    });
    if (rows.empty()) note("No matching challenges.");
    ImGui::EndChild();
}

void progression_profile(SkateMenu& menu, const ProgressionModel& model, const CallbacksV3& callbacks) {
    begin_card(menu, "rip-score", "RIP SCORE");
    if (model.score_available) {
        if (menu.score_dirty && menu.score_edit == model.score && menu.score_cap_edit == model.score_cap &&
            menu.score_level_edit == model.score_level) menu.score_dirty = false;
        if (!menu.score_dirty) {
            menu.score_edit = model.score; menu.score_cap_edit = model.score_cap; menu.score_level_edit = model.score_level;
        }
        ImGui::BeginDisabled(!callbacks.queue_console_command);
        field(menu, "Score");
        menu.score_dirty |= ImGui::InputScalar("##score", ImGuiDataType_S64, &menu.score_edit);
        field(menu, "Cap");
        menu.score_dirty |= ImGui::InputScalar("##cap", ImGuiDataType_S64, &menu.score_cap_edit);
        field(menu, "Level");
        menu.score_dirty |= ImGui::InputInt("##level", &menu.score_level_edit, 0, 0);
        const bool valid = menu.score_edit >= 0 && menu.score_cap_edit > 0 && menu.score_cap_edit <= 1000000000 &&
            menu.score_edit <= menu.score_cap_edit && menu.score_level_edit > 0 && menu.score_level_edit <= 10000;
        const int pressed = button_row(menu, "score-actions", {{"SAVE SCORE", menu.score_dirty && valid, true, "Save this score, cap and level."},
                                                               {"FILL TO CAP", true, false, "The score up to the cap."},
                                                               {"DISCARD", true, false, "Back to the saved score."}});
        if (pressed == 0)
            progression_request(menu, callbacks, "ripscore " + std::to_string(menu.score_edit) + " " +
                std::to_string(menu.score_cap_edit) + " " + std::to_string(menu.score_level_edit));
        if (pressed == 1) { menu.score_edit = menu.score_cap_edit; menu.score_dirty = true; }
        if (pressed == 2) menu.score_dirty = false;
        ImGui::EndDisabled();
        if (!valid) warn("Use score 0 to cap, cap 1 to 1,000,000,000, and level 1 to 10,000.");
    } else note("No saved RIP score is configured for this profile.");
    end_card();

    // A row a district, its level at the right end; A opens the level to step, then SAVE.
    begin_card(menu, "district-levels", "DISTRICT LEVELS");
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    bool maxed = model.ranks_maxed;
    if (toggle_row(menu, "Keep all districts maxed", "Hold every district at its top level.", maxed))
        progression_request(menu, callbacks, std::string("maxranks ") + (maxed ? "1" : "0"));
    static std::string open_district;
    for (std::size_t i = 0; i < model.districts.size() && i < menu.district_edit.size(); ++i) {
        const auto& row = model.districts[i];
        auto& edit = menu.district_edit[i];
        auto& dirty = menu.district_dirty[i];
        if (dirty && edit == static_cast<int>(row.rank)) dirty = false;
        if (!dirty) edit = static_cast<int>(row.rank);
        const auto id = "district-" + std::to_string(i);
        ImGui::PushID(id.c_str());
        const auto side = model.ranks_maxed ? std::string("Maxed")
                        : "Level " + std::to_string(row.rank) + (dirty ? "  >  " + std::to_string(edit) : std::string());
        if (expand_row(menu, id.c_str(), row.name, "", side, open_district)) {
            ImGui::BeginDisabled(model.ranks_maxed);
            ImGui::Indent(px(40));
            field(menu, "Level", "Left / right a level at a time, or type one in the box.");
            set_console_step(1);
            int level = edit;
            if (slider_int("##rank", &level, 0, 10000, "Level %d", ImGuiSliderFlags_AlwaysClamp)) {
                edit = level;
                dirty = true;
            }
            ImGui::Unindent(px(40));
            const int pressed = row_actions(menu, "actions", {{"SAVE", dirty && edit >= 0 && edit <= 10000, true, "Set this district to that level."},
                                                              {"DISCARD", dirty, false, "Back to its saved level."}});
            if (pressed == 0) progression_request(menu, callbacks, "district " + row.id + " " + std::to_string(edit));
            if (pressed == 1) dirty = false;
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    note("Turn off Keep all districts maxed to edit levels. The game limits each rank to its available curve.");
    end_card();
}

// A row a mission: left / right (or A) marks it done or not done.
void missions(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    static const std::vector<std::string> groups{"All missions", "Story", "Onboarding", "Districts"};
    option_row(menu, "Show", "Left / right for one kind of mission.", menu.mission_group, groups);
    search_box("##mission-search", "Search missions...", menu.mission_search);
    std::vector<const MissionRow*> rows;
    const std::string_view query(menu.mission_search.data());
    for (const auto& row : model.missions) {
        if (menu.mission_group && row.group != groups[static_cast<std::size_t>(menu.mission_group)]) continue;
        if (!query.empty() && !contains(row.id, query) && !contains(readable(row.id), query)) continue;
        rows.push_back(&row);
    }
    note(model.mission_feedback.empty()
        ? (std::to_string(rows.size()) + " missions, saved locally. Reload the level after changing an active mission.").c_str()
        : model.mission_feedback.c_str());
    if (!model.missions_available) warn("The local profile is unavailable.");
    ImGui::BeginChild("mission-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_NavFlattened);
    static const std::vector<std::string> states{"Not done", "Done"};
    const bool can = model.missions_available && callbacks.queue_console_command;
    clipped_rows(static_cast<int>(rows.size()), -1, [&](int i) {
        const auto& row = *rows[static_cast<std::size_t>(i)];
        ImGui::PushID(row.id.c_str());
        int state = row.completed;  // -1: no saved edit, the game's own progress
        // Its group, whose progress it shows and its id: the line the menu shows for the focused row.
        const auto hint = row.group + (row.completed < 0 ? ", following game progress. " : ", a saved edit. ") + row.id;
        if (option_row(menu, (readable(row.id) + "###state").c_str(), hint.c_str(), state, states, can)) {
            const auto command = std::string("mission ") + (state ? "complete " : "reset ") + row.id;
            std::array<char, 512> result{};
            const bool queued = callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
            result.back() = '\0';
            feedback(menu, result[0] ? result.data() : queued ? "" : "Mission change unavailable.");
        }
        ImGui::PopID();
    });
    if (rows.empty()) note("No matching missions.");
    ImGui::EndChild();
}

void progression_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.progression_tab, {"MISSIONS", "CHALLENGES", "BUS STOPS", "PROFILE"}, "progression-categories");
    const bool feedback_shown = !menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until;
    ImGui::PushID(menu.progression_tab);
    ImGui::BeginChild("progression-category", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    if (menu.progression_tab == 0) missions(menu, model, callbacks);
    else if (!model.progression.available) warn("The local profile is unavailable.");
    else {
        switch (menu.progression_tab) {
        case 1: challenge_progress(menu, model.progression, callbacks); break;
        case 2: bus_stops(menu, model.progression, callbacks); break;
        case 3: progression_profile(menu, model.progression, callbacks); break;
        }
    }
    ImGui::EndChild(); ImGui::PopID();
    if (menu.progression_tab != 0 && !model.progression.feedback.empty() && feedback_shown &&
        ImGui::GetTime() > menu.feedback_until - 5.75) menu.feedback = model.progression.feedback;
}

}
