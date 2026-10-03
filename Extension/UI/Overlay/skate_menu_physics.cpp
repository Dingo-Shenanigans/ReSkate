#include "skate_menu_internal.h"
#include "Extension/Skater/physics_tuning.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// The SKATER page's PHYSICS tab: a slider for every plain number in the game's skate physics
// tuning (Gameplay/SkatePhysicsTuning), a search box, and named presets. Values are changed in
// the running game (Extension/Skater/physics_tuning.h, live::).
namespace dingosdk::overlay::menu {
namespace {
namespace tune = dingosdk::physics_tuning::live;

float trailing_width(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
}
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
// Every word typed must appear in the parameter's full name, in any order.
bool matches(const std::vector<std::string>& words, const std::string& path_lower) {
    return std::ranges::all_of(words, [&](const std::string& word) { return path_lower.find(word) != std::string::npos; });
}
std::vector<std::string> split_words(const char* text) {
    std::vector<std::string> words;
    std::string word;
    for (const char* c = text; ; ++c) {
        if (*c && !std::isspace(static_cast<unsigned char>(*c))) { word += static_cast<char>(std::tolower(static_cast<unsigned char>(*c))); continue; }
        if (!word.empty()) words.push_back(std::exchange(word, {}));
        if (!*c) break;
    }
    return words;
}
std::pair<float, float> slider_range(float game, int mode) {
    if (!std::isfinite(game) || game == 0.0f) return {-1.0f, 1.0f};
    const float low = mode == 0 ? game - std::fabs(game) * 0.5f : std::min(0.0f, game * 3.0f);
    const float high = mode == 0 ? game + std::fabs(game) * 0.5f : std::max(0.0f, game * 3.0f);
    return {low, high};
}
// What a changed value is compared with: the game's own tuning, to a tolerance that ignores float noise.
bool differs(float value, float game) {
    return std::fabs(value - game) > 1e-6f * std::max(1.0f, std::fabs(game));
}

void status_card(SkateMenu& menu, const tune::Snapshot& snapshot) {
    begin_card(menu, "physics-status", "PHYSICS TUNING", "The game's own numbers, changed while you skate");
    if (!snapshot.ready) {
        note(snapshot.status.empty() ? "Waiting for the game's physics tuning." : snapshot.status.c_str());
        end_card();
        return;
    }
    if (snapshot.locked)
        warn("A host's physics tuning is in use in this session, so these are read-only until you leave it.");
    info(menu, "Changed", std::to_string(snapshot.changed) + " of " + std::to_string(snapshot.params->size()) + " values");
    ImGui::BeginDisabled(snapshot.locked || snapshot.changed == 0);
    if (ImGui::Button("Reset all", ImVec2(-FLT_MIN, 0))) tune::reset_all();
    ImGui::EndDisabled();
    note("Ctrl+click a slider to type an exact value. Big changes can make the skater clip into the ground or "
         "launch: Reset all puts everything back. In a lobby you host, guests skate with your values. "
         "Masses and collision sizes apply from your next respawn.");
    end_card();
}

void preset_card(SkateMenu& menu, const tune::Snapshot& snapshot) {
    begin_card(menu, "physics-presets", "PRESETS", "Saved in %LOCALAPPDATA%\\ReSkate\\physics_presets");
    const bool editable = snapshot.ready && !snapshot.locked;
    field(menu, "Name");
    ImGui::BeginDisabled(!editable);
    const float save_width = trailing_width("Save");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - save_width - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##physics-preset-name", "New preset name", menu.physics_preset_name.data(),
                             menu.physics_preset_name.size());
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(save_width, 0))) {
        std::string name = menu.physics_preset_name.data();
        if (tune::save_preset(name)) feedback(menu, ("Saved preset \"" + name + "\".").c_str());
        else feedback(menu, "Could not save. Use 1 to 40 letters, digits, spaces, - or _.");
    }
    ImGui::EndDisabled();
    if (snapshot.presets.empty()) note("No presets yet. Save your current changes under a name to keep them.");
    for (const auto& name : snapshot.presets) {
        ImGui::PushID(name.c_str());
        field(menu, name.c_str());
        const float load_width = trailing_width("Load"), delete_width = trailing_width("Delete");
        ImGui::BeginDisabled(!editable);
        if (ImGui::Button("Load", ImVec2(load_width, 0))) {
            if (tune::load_preset(name)) feedback(menu, ("Loaded preset \"" + name + "\".").c_str());
            else feedback(menu, "Could not load that preset.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(delete_width, 0)) && !tune::delete_preset(name))
            feedback(menu, "Could not delete that preset.");
        ImGui::PopID();
    }
    end_card();
}

void slider_row(SkateMenu& menu, const tune::Param& param, float value, bool locked) {
    const bool changed = differs(value, param.game);
    ImGui::PushID(param.offset);
    const std::string label = changed ? param.name + " *" : param.name;
    field(menu, label.c_str(), param.path.c_str());
    ImGui::BeginDisabled(locked);
    if (param.flag) {
        bool on = value >= 0.5f;
        if (ImGui::Checkbox("##value", &on)) tune::set(param.offset, on ? 1.0f : 0.0f);
    } else {
        const auto [low, high] = slider_range(param.game, menu.physics_range);
        const float reset_width = trailing_width("Reset");
        float shown = value;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset_width - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::SliderFloat("##value", &shown, low, high, "%.4g")) tune::set(param.offset, shown);
        ImGui::SameLine();
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Reset", ImVec2(reset_width, 0))) tune::set(param.offset, param.game);
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void values_card(SkateMenu& menu, const tune::Snapshot& snapshot) {
    begin_card(menu, "physics-values", "VALUES", "Names are the game's own");
    field(menu, "Search");
    ImGui::InputTextWithHint("##physics-search", "e.g. push, jump, friction, steering", menu.physics_search.data(),
                             menu.physics_search.size());
    field(menu, "Slider range");
    choice(menu, "physics-range", menu.physics_range, {"Around the game's value", "Zero to 3x"});
    const auto words = split_words(menu.physics_search.data());
    const bool searching = !words.empty();
    // Fields of one group sit together; keep the game's order.
    const auto& params = *snapshot.params;
    std::size_t shown = 0;
    for (std::size_t first = 0; first < params.size();) {
        std::size_t last = first + 1;
        while (last < params.size() && params[last].group == params[first].group) ++last;
        std::size_t matching = 0, changed = 0;
        for (std::size_t i = first; i < last; ++i) {
            if (differs(snapshot.values[i], params[i].game)) ++changed;
            if (!searching || matches(words, lower(params[i].path))) ++matching;
        }
        if (matching) {
            const std::string header = params[first].group + "  (" + std::to_string(matching) +
                (changed ? ", " + std::to_string(changed) + " changed" : std::string{}) + ")###physics-group-" +
                std::to_string(first);
            if (searching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            if (ImGui::CollapsingHeader(header.c_str())) {
                for (std::size_t i = first; i < last; ++i)
                    if (!searching || matches(words, lower(params[i].path)))
                        slider_row(menu, params[i], snapshot.values[i], snapshot.locked);
            }
            shown += matching;
        }
        first = last;
    }
    if (searching && !shown) note("Nothing matches that search.");
    note("Curves (values that change with speed or angle) are not included, only plain numbers and switches.");
    end_card();
}
} // namespace

void physics_controls(SkateMenu& menu, const Model&, const CallbacksV3&) {
    const auto snapshot = tune::snapshot();
    status_card(menu, snapshot);
    if (!snapshot.ready || !snapshot.params || snapshot.values.size() != snapshot.params->size()) return;
    preset_card(menu, snapshot);
    values_card(menu, snapshot);
}
} // namespace dingosdk::overlay::menu
