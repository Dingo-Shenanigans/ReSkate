#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

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
// Lists on these tabs are tiles of one fixed height, so a clipper can skip the
// ones scrolled out of view (hundreds of missions and challenges).
float tile_height() { return px(56); }
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
struct Tile {
    ImVec2 top;
    float width{}, middle{};
};
Tile begin_tile(unsigned seed) {
    Tile tile{ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0};
    tile.middle = tile.top.y + (tile_height() - ImGui::GetFrameHeight()) * .5f;
    skate_theme::rough_rect(ImGui::GetWindowDrawList(), tile.top,
                            ImVec2(tile.top.x + tile.width, tile.top.y + tile_height()), skate_theme::tile, seed, px(1));
    return tile;
}
// The name in bold with a muted line under it, clipped before `right`.
void tile_text(SkateMenu& menu, const Tile& tile, const std::string& title, const std::string& subtitle, float right) {
    ImGui::SetCursorScreenPos(ImVec2(tile.top.x + px(14), tile.top.y + px(9)));
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(right, tile.top.y + tile_height()), true);
    ImGui::BeginGroup();
    ImGui::PushFont(menu.bold);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
    ImGui::TextUnformatted(subtitle.c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::PopClipRect();
}
// Claims the tile's space; true while the pointer is over it.
bool end_tile(const Tile& tile) {
    ImGui::SetCursorScreenPos(tile.top);
    ImGui::Dummy(ImVec2(tile.width, tile_height()));
    return ImGui::IsItemHovered();
}
float list_row_height() { return tile_height() + ImGui::GetStyle().ItemSpacing.y; }
bool contains(std::string_view value, std::string_view query) {
    return std::search(value.begin(), value.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    }) != value.end();
}
} // namespace

void bus_stops(SkateMenu& menu, const ProgressionModel& model, const CallbacksV3& callbacks) {
    constexpr const char* maps[]{"All maps", "San Vansterdam", "Isle of Grom", "Super Ultra Mega Resort"};
    choice(menu, "bus-map", menu.bus_stop_map, {"All maps", "San Vansterdam", "Isle of Grom", "Mega Resort"});
    std::vector<const BusStopRow*> rows;
    for (const auto& row : model.bus_stops)
        if (!menu.bus_stop_map || static_cast<int>(row.world) + 1 == menu.bus_stop_map) rows.push_back(&row);
    note((std::to_string(rows.size()) + " bus stops, saved locally. Reload the level after edits to refresh their icons.").c_str());
    ImGui::BeginChild("bus-stop-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)));
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), list_row_height());
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& row = *rows[i];
        ImGui::PushID(static_cast<int>(row.number));
        const auto tile = begin_tile(row.number + 83);
        const float states = std::min(px(330), tile.width * .55f);
        const float right = tile.top.x + tile.width - px(14);
        ImGui::SetCursorScreenPos(ImVec2(right - states, tile.middle));
        int state = !row.visible ? (row.unlocked ? -1 : 0) : row.unlocked ? 2 : 1;
        if (choice(menu, "state", state, {"Hidden", "Locked", "Unlocked"}, true, states))
            progression_request(menu, callbacks, "busstop " + std::to_string(row.number) + " " + std::to_string(state));
        char title[32];
        std::snprintf(title, sizeof(title), "Bus stop %03u", row.number);
        tile_text(menu, tile, title, maps[static_cast<unsigned>(row.world) + 1], right - states - px(12));
        end_tile(tile);
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (rows.empty()) note("No bus stops on this map.");
    ImGui::EndChild();
}

void challenge_progress(SkateMenu& menu, const ProgressionModel& model, const CallbacksV3& callbacks) {
    bool shown = !model.challenges_hidden;
    if (toggle_row(menu, "Show challenges", "Show challenges in the world, on the compass and on the map. Saved progress is kept.",
            shown, model.challenges_enabled && callbacks.queue_console_command))
        progression_request(menu, callbacks, shown ? "challenges 1" : "challenges 0");
    constexpr const char* types[]{"All types", "OTS", "Slam", "Line", "Session", "Stunt", "OTL", "Speedline"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .65f);
    ImGui::InputTextWithHint("##challenge-search", "Search challenges...", menu.challenge_search.data(), menu.challenge_search.size());
    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##challenge-type", &menu.challenge_type, types, static_cast<int>(std::size(types)));
    std::vector<const ChallengeProgressRow*> rows;
    const std::string_view query(menu.challenge_search.data());
    for (const auto& row : model.challenges) {
        if (menu.challenge_type && row.type != types[menu.challenge_type]) continue;
        if (!query.empty() && !contains(row.id, query) && !contains(readable(row.id), query)) continue;
        rows.push_back(&row);
    }
    std::size_t done{};
    for (const auto* row : rows) done += row->goal_count && row->completed_goals.size() >= row->goal_count;
    note((std::to_string(rows.size()) + " challenges, " + std::to_string(done) + " fully complete, " +
          (!model.challenges_enabled ? "local challenges disabled" : model.challenges_hidden ? "hidden" : "saved locally") +
          ". Hover one to see its completed goals.").c_str());
    ImGui::BeginChild("challenge-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), list_row_height());
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& row = *rows[i];
        ImGui::PushID(row.id.c_str());
        const auto tile = begin_tile(static_cast<unsigned>(i) + 97);
        auto* draw = ImGui::GetWindowDrawList();
        float right = tile.top.x + tile.width - px(14);
        // Right: goals as "done / total", attempts under it.
        const auto goals = std::to_string(row.completed_goals.size()) +
            (row.goal_count ? " / " + std::to_string(row.goal_count) : std::string()) + " goals";
        const auto attempts = std::to_string(row.attempts) + (row.attempts == 1 ? " attempt" : " attempts");
        const auto goals_size = menu.bold->CalcTextSizeA(px(16), FLT_MAX, 0, goals.c_str());
        const auto attempts_size = menu.body->CalcTextSizeA(px(13), FLT_MAX, 0, attempts.c_str());
        const bool complete = row.goal_count && row.completed_goals.size() >= row.goal_count;
        draw->AddText(menu.bold, px(16), ImVec2(right - goals_size.x, tile.top.y + px(9)),
                      complete ? skate_theme::good : skate_theme::white, goals.c_str());
        draw->AddText(menu.body, px(13), ImVec2(right - attempts_size.x, tile.top.y + px(31)), skate_theme::grey_text,
                      attempts.c_str());
        right -= std::max(goals_size.x, attempts_size.x) + px(16);
        const auto type_width = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, row.type.c_str()).x + px(14);
        right -= type_width;
        ImGui::SetCursorScreenPos(ImVec2(right, tile.middle));
        tag(menu, row.type.c_str(), row.available ? skate_theme::white : skate_theme::tile_light);
        std::string subtitle = row.neighborhood.empty() ? row.id : readable(row.neighborhood) + "  -  " + row.id;
        if (!row.available) subtitle += "  -  unavailable";
        tile_text(menu, tile, readable(row.id), subtitle, right - px(12));
        if (end_tile(tile) && !row.completed_goals.empty()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Completed goals");
            for (const auto& goal : row.completed_goals) ImGui::BulletText("%s", goal.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
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
        const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        ImGui::BeginDisabled(!menu.score_dirty || !valid);
        skate_theme::push_primary_button();
        if (ImGui::Button("Save score", ImVec2(third, 0)))
            progression_request(menu, callbacks, "ripscore " + std::to_string(menu.score_edit) + " " +
                std::to_string(menu.score_cap_edit) + " " + std::to_string(menu.score_level_edit));
        skate_theme::pop_primary_button();
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Fill to cap", ImVec2(third, 0))) { menu.score_edit = menu.score_cap_edit; menu.score_dirty = true; }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(-FLT_MIN, 0))) menu.score_dirty = false;
        ImGui::EndDisabled();
        if (!valid) warn("Use score 0 to cap, cap 1 to 1,000,000,000, and level 1 to 10,000.");
    } else note("No saved RIP score is configured for this profile.");
    end_card();
    begin_card(menu, "unlocks", "UNLOCKS");
    bool everything = model.everything_unlocked;
    if (toggle_row(menu, "Unlock every item",
            "Own every cosmetic and object in the game's catalogue, including store and premium pass items. "
            "Applies the next time the game starts.",
            everything, callbacks.queue_console_command != nullptr))
        progression_request(menu, callbacks, std::string("unlockall ") + (everything ? "1" : "0"));
    end_card();
    begin_card(menu, "district-levels", "DISTRICT LEVELS");
    ImGui::BeginDisabled(!callbacks.queue_console_command);
    bool maxed = model.ranks_maxed;
    if (toggle_row(menu, "Keep all districts maxed", "Hold every district at its top level.", maxed))
        progression_request(menu, callbacks, std::string("maxranks ") + (maxed ? "1" : "0"));
    ImGui::BeginDisabled(model.ranks_maxed);
    if (ImGui::BeginTable("district-levels", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("DISTRICT", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("LEVEL", ImGuiTableColumnFlags_WidthFixed, px(100));
        ImGui::TableSetupColumn("##save", ImGuiTableColumnFlags_WidthFixed, px(80));
        ImGui::TableHeadersRow();
        for (unsigned i = 0; i < model.districts.size(); ++i) {
            const auto& row = model.districts[i];
            if (menu.district_dirty[i] && menu.district_edit[i] == static_cast<int>(row.rank)) menu.district_dirty[i] = false;
            if (!menu.district_dirty[i]) menu.district_edit[i] = static_cast<int>(row.rank);
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(ImGuiTableRowFlags_None, 32);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            menu.district_dirty[i] |= ImGui::InputInt("##rank", &menu.district_edit[i], 0, 0);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!menu.district_dirty[i] || menu.district_edit[i] < 0 || menu.district_edit[i] > 10000);
            if (ImGui::Button("Save", ImVec2(-1, 0)))
                progression_request(menu, callbacks, "district " + row.id + " " + std::to_string(menu.district_edit[i]));
            ImGui::EndDisabled(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled(); ImGui::EndDisabled();
    note("Turn off Keep all districts maxed to edit levels. The game limits each rank to its available curve.");
    end_card();
}

void missions(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .65f);
    ImGui::InputTextWithHint("##mission-search", "Search missions...", menu.mission_search.data(), menu.mission_search.size());
    ImGui::SameLine();
    constexpr const char* groups[]{"All missions", "Story", "Onboarding", "Districts"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##mission-group", &menu.mission_group, groups, static_cast<int>(std::size(groups)));
    std::vector<const MissionRow*> rows;
    const std::string_view query(menu.mission_search.data());
    for (const auto& row : model.missions) {
        if (menu.mission_group && row.group != groups[menu.mission_group]) continue;
        if (!query.empty() && !contains(row.id, query) && !contains(readable(row.id), query)) continue;
        rows.push_back(&row);
    }
    note(model.mission_feedback.empty()
        ? (std::to_string(rows.size()) + " missions, saved locally. Reload the level after changing an active mission.").c_str()
        : model.mission_feedback.c_str());
    if (!model.missions_available) warn("The local profile is unavailable.");
    ImGui::BeginChild("mission-list", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)));
    ImGui::BeginDisabled(!model.missions_available || !callbacks.queue_console_command);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), list_row_height());
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto& row = *rows[i];
        ImGui::PushID(row.id.c_str());
        const auto tile = begin_tile(static_cast<unsigned>(i) + 131);
        const float states = std::min(px(240), tile.width * .45f);
        const float right = tile.top.x + tile.width - px(14);
        ImGui::SetCursorScreenPos(ImVec2(right - states, tile.middle));
        int state = row.completed;  // -1: no saved edit, the game's own progress
        if (choice(menu, "state", state, {"Not done", "Done"}, true, states)) {
            const auto command = std::string("mission ") + (state ? "complete " : "reset ") + row.id;
            std::array<char, 512> result{};
            const bool queued = callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
            result.back() = '\0';
            feedback(menu, result[0] ? result.data() : queued ? "" : "Mission change unavailable.");
        }
        const auto subtitle = row.group + (row.completed < 0 ? "  -  following game progress" : "  -  saved edit");
        tile_text(menu, tile, readable(row.id), subtitle, right - states - px(12));
        if (end_tile(tile)) ImGui::SetTooltip("%s", row.id.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (rows.empty()) note("No matching missions.");
    ImGui::EndChild();
}

void progression_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.progression_tab, {"MISSIONS", "CHALLENGES", "BUS STOPS", "PROFILE"}, "progression-categories");
    const bool feedback_shown = !menu.feedback.empty() && ImGui::GetTime() < menu.feedback_until;
    ImGui::PushID(menu.progression_tab);
    ImGui::BeginChild("progression-category", ImVec2(0, page_body_height(menu)));
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
