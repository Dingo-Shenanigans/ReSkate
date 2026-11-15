#include "gui_internal.h"

#include "Engine/Game/World/world_names.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/initfs.h"

#include <cctype>
#include <cmath>
#include <format>

// The Settings page: the launcher's own settings and skate.'s, a tile each along the top, with
// the pages of the one that is open as tabs under them. Every setting is a row of the same kind:
// what it is called and a line about it at the left, its control at the right, the rows of a kind
// together in a box. What is typed in the search shows the rows that match, from every page.
namespace dingosdk::launcher_gui::detail {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------- rows

// The page as it is being drawn. A group (begin_group ... end_group) is a caption over a box; a
// row (begin_row ... end_row) is one setting in it. The caption and the box are only drawn once a
// row of the group is, so a group the search leaves nothing of is not there at all; the same goes
// for the page's name, which is said over its first group while the search shows several pages.
struct Page {
    const Fonts* fonts{};
    std::string query;          // what the search is narrowed to, lower case; empty: everything
    const char* heading{};      // the page's name, still to be said (search only)
    const char* group{};        // the group's caption
    bool group_open{};          // a row of the group has been drawn: its box is open
    ImVec2 group_start{};
    float width{};
    float limit{};              // how wide a box may be: the page's own width, short of where its scrollbar runs
    int rows{};                 // drawn in the open group
    ImVec2 row_start{};
    float row_tall{};
    bool any{};                 // a row has been drawn on the page
};
Page g_page;

void begin_group(const char* caption) {
    g_page.group = caption;
    g_page.group_open = false;
}

void end_group() {
    if (!g_page.group_open) return;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(g_page.group_start.x + g_page.width, ImGui::GetCursorScreenPos().y);
    // The box, under the rows drawn since it was opened.
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(g_page.group_start, end, color::tile, S(6));
    draw->AddRect(g_page.group_start, end, color::outline, S(6));
    draw->ChannelsMerge();
    g_page.group_open = false;
    ImGui::Dummy(ImVec2(0, S(6)));
}

constexpr float row_pad = 16, row_control = 280;

// A row: `label` over `note` (a line; none when empty) at the left, and the cursor left where its
// control goes, `control` wide at the right end, set for a control a frame tall. `tip`: the whole
// of what the note cuts short, shown while the pointer is over the text. False, with nothing
// drawn, when the search does not ask for it: the caller then draws no control and calls no end_row.
bool begin_row(const char* label, const char* note, const char* tip = nullptr, float control = S(row_control)) {
    if (!g_page.query.empty()) {
        const auto& query = g_page.query;
        if (lower(label).find(query) == std::string::npos && lower(note).find(query) == std::string::npos &&
            lower(g_page.group ? g_page.group : "").find(query) == std::string::npos &&
            lower(g_page.heading ? g_page.heading : "").find(query) == std::string::npos && !(tip && lower(tip).find(query) != std::string::npos))
            return false;
    }
    const auto& fonts = *g_page.fonts;
    if (!g_page.group_open) {
        if (g_page.heading && *g_page.heading) {
            ImGui::Dummy(ImVec2(0, g_page.any ? S(10) : 0.0f));
            ImGui::PushFont(fonts.heading);
            ImGui::TextUnformatted(g_page.heading);
            ImGui::PopFont();
            g_page.heading = nullptr;
        }
        // (The caption close over its box, not an item's spacing away.)
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(5)));
        ImGui::PushFont(fonts.caption);
        ImGui::TextDisabled("%s", g_page.group ? g_page.group : "");
        ImGui::PopFont();
        ImGui::PopStyleVar();
        g_page.group_start = ImGui::GetCursorScreenPos();
        g_page.width = g_page.limit > 0 ? std::min(g_page.limit, ImGui::GetContentRegionAvail().x) : ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        g_page.group_open = true;
        g_page.rows = 0;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const float pad = S(row_pad), frame = ImGui::GetFrameHeight();
    const float text_width = g_page.width - pad * 3 - control;
    const float label_height = ImGui::CalcTextSize(label, nullptr, false, text_width).y;
    const float note_height = *note ? ImGui::CalcTextSize(note, nullptr, false, text_width).y : 0.0f;
    const float text_height = label_height + (note_height > 0 ? S(3) + note_height : 0.0f);
    const float tall = std::max(*note ? S(58) : S(46), S(12) * 2 + std::max(text_height, frame));
    const ImVec2 start(g_page.group_start.x, ImGui::GetCursorScreenPos().y), end(start.x + g_page.width, start.y + tall);
    if (g_page.rows) draw->AddLine(ImVec2(start.x + S(1), start.y), ImVec2(end.x - S(1), start.y), color::outline);
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(start, end);
    if (over) draw->AddRectFilled(ImVec2(start.x + S(1), start.y + S(1)), ImVec2(end.x - S(1), end.y), rgba(255, 255, 255, 0.025f));
    const float text_y = start.y + (tall - text_height) * 0.5f;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(start.x + pad, text_y), ImGui::GetColorU32(ImGuiCol_Text), label, nullptr,
                  text_width);
    if (note_height > 0)
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(start.x + pad, text_y + label_height + S(3)),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), note, nullptr, text_width);
    if (tip && over && ImGui::IsMouseHoveringRect(start, ImVec2(start.x + pad + text_width, end.y))) {
        ImGui::SetNextWindowSize(ImVec2(S(420), 0));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    g_page.row_start = start;
    g_page.row_tall = tall;
    ++g_page.rows;
    g_page.any = true;
    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(ImVec2(end.x - pad - control, start.y + (tall - frame) * 0.5f));
    ImGui::SetNextItemWidth(control);
    return true;
}

void end_row() {
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(g_page.group_start.x, g_page.row_start.y + g_page.row_tall));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Dummy(ImVec2(g_page.width, 0));
    ImGui::PopStyleVar();
}

// An on/off setting: the whole row is what is pressed, with the switch at its right end. True when
// it was flipped.
bool row_switch(const char* label, const char* note, bool& value, const char* tip = nullptr) {
    if (!begin_row(label, note, tip, switch_size().x)) return false;
    const auto mark = switch_size();
    ImGui::SetCursorScreenPos(g_page.row_start);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(g_page.width, g_page.row_tall), ImGuiButtonFlags_EnableNav);
    if (pressed) value = !value;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    draw_switch(ImGui::GetWindowDrawList(),
                ImVec2(g_page.row_start.x + g_page.width - S(row_pad) - mark.x, g_page.row_start.y + (g_page.row_tall - mark.y) * 0.5f), value, hovered);
    end_row();
    return pressed;
}

// A slider, `width` across at the cursor: a groove filled up to a round knob, and the value as a
// figure after it. `fraction`: how far along the value is, for the fill; `slide` draws ImGui's own
// slider `track` wide (for its dragging and its keys), with nothing of its own showing but the knob.
template<class Slide> bool slider(float width, float fraction, const std::string& figure, Slide slide) {
    const float figure_width = S(52), track = width - figure_width;
    const float frame = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(3)));
    const float tall = ImGui::GetFrameHeight(), knob = tall - S(4);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, knob);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, knob);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(color::text));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(color::blue));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 at(start.x, start.y + (frame - tall) * 0.5f);
    auto* draw = ImGui::GetWindowDrawList();
    const float middle = at.y + tall * 0.5f, groove = S(4);
    const float from = at.x + S(2) + knob * 0.5f, to = at.x + track - S(2) - knob * 0.5f;
    draw->AddRectFilled(ImVec2(from - knob * 0.5f, middle - groove * 0.5f), ImVec2(to + knob * 0.5f, middle + groove * 0.5f),
                        rgba(255, 255, 255, 0.12f), groove * 0.5f);
    draw->AddRectFilled(ImVec2(from - knob * 0.5f, middle - groove * 0.5f),
                        ImVec2(from + (to - from) * std::clamp(fraction, 0.0f, 1.0f), middle + groove * 0.5f), color::blue, groove * 0.5f);
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemWidth(track);
    const bool changed = slide();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    const float figure_x = at.x + width - ImGui::CalcTextSize(figure.c_str()).x;
    draw->AddText(ImVec2(figure_x, start.y + (frame - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), figure.c_str());
    return changed;
}

// A notice over the page's rows.
void notice(const char* text, ImU32 ink = color::muted) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ink));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- skate.'s own settings

namespace game = launcher_game_settings;

// Reads the game's settings the first time a page of them is drawn after Settings opens. False
// when there are none to show (ui.game.error says why, when it is not just that the game has
// never saved any). `running`: the game is up, so they cannot be changed from here.
bool game_settings_ready(Launcher& launcher, Ui& ui, bool& running) {
    auto& pages = ui.game;
    if (!pages.loaded) {
        pages.loaded = true;
        pages.error.clear();
        try {
            pages.saved = game::load(game::save_root());
        } catch (const std::exception& failure) {
            pages.saved = {};
            pages.error = failure.what();
        }
        pages.values = pages.saved.values;
        pages.resolutions = game::display_resolutions();
    }
    running = launcher.game() != 0 || launcher.busy();
    return !pages.saved.folders.empty();
}

// Stores what the pages show once it differs from the game's save and no control is held (a
// slider being dragged writes when it is let go).
void store_game_settings(Ui& ui) {
    auto& pages = ui.game;
    if (!pages.loaded || pages.saved.folders.empty() || ImGui::IsAnyItemActive()) return;
    game::Values changes;
    for (const auto& [key, value] : pages.values) {
        const auto before = pages.saved.values.find(key);
        if (before != pages.saved.values.end() && before->second != value) changes.emplace(key, value);
    }
    if (changes.empty()) return;
    try {
        // (...\ReSkate\Game\Skate\data -> ...\ReSkate\game-settings-backup)
        game::save(pages.saved, changes, game::save_root().parent_path().parent_path().parent_path() / L"game-settings-backup");
        pages.saved.values = pages.values;
        pages.error.clear();
    } catch (const std::exception& failure) {
        pages.error = std::string("Not saved: ") + failure.what() + ".";
        pages.values = pages.saved.values;
    }
}

// A choice between "Off" and "On" is a switch: the two choices, off first, or nulls.
template<class Choices> auto on_and_off(const Choices& choices) {
    using Choice = decltype(&*std::begin(choices));
    std::pair<Choice, Choice> found{nullptr, nullptr};   // (off, on)
    if (std::size(choices) != 2) return found;
    for (const auto& choice : choices) {
        const auto label = lower(choice.label);
        if (label == "off") found.first = &choice;
        else if (label == "on") found.second = &choice;
    }
    if (!found.first || !found.second) found = {nullptr, nullptr};
    return found;
}

// One of the game's settings pages: each setting a row, the rows of a section in a box.
void game_settings_page(Ui& ui, std::string_view page, bool running) {
    auto& pages = ui.game;
    const char* section = nullptr;
    ImGui::BeginDisabled(running);
    for (const auto& item : game::items()) {
        if (page != item.page) continue;
        // A setting this game version does not save is not offered.
        const auto found = pages.values.find(item.key);
        if (found == pages.values.end()) continue;
        if (!section || std::string_view(section) != item.section) {
            if (section) end_group();
            section = item.section;
            begin_group(section);
        }
        const auto steps = [&](float value) {
            const float snapped = item.least + std::round((value - item.least) / item.step) * item.step;
            return std::clamp(snapped, item.least, item.most);
        };
        const float span = std::max(item.most - item.least, 0.0001f);
        switch (item.kind) {
        case game::Kind::toggle:
            if (auto* on = std::get_if<bool>(&found->second)) row_switch(item.label, "", *on);
            break;
        case game::Kind::whole:
            if (auto* number = std::get_if<std::int32_t>(&found->second); number && begin_row(item.label, "")) {
                int value = *number;
                if (slider(S(row_control), (static_cast<float>(value) - item.least) / span, std::to_string(value), [&] {
                        return ImGui::SliderInt("##value", &value, static_cast<int>(item.least), static_cast<int>(item.most), "",
                                                ImGuiSliderFlags_AlwaysClamp);
                    }))
                    *number = static_cast<std::int32_t>(steps(static_cast<float>(value)));
                end_row();
            }
            break;
        case game::Kind::real:
            if (auto* number = std::get_if<float>(&found->second); number && begin_row(item.label, "")) {
                float value = *number;
                const auto figure = item.step >= 1 ? std::format("{:.0f}", value) : item.step >= 0.1f ? std::format("{:.1f}", value)
                                                                                                       : std::format("{:.2f}", value);
                if (slider(S(row_control), (value - item.least) / span, figure, [&] {
                        return ImGui::SliderFloat("##value", &value, item.least, item.most, "", ImGuiSliderFlags_AlwaysClamp);
                    }))
                    *number = steps(value);
                end_row();
            }
            break;
        case game::Kind::choice:
            if (auto* number = std::get_if<std::int32_t>(&found->second)) {
                if (const auto [off, on] = on_and_off(item.choices); off && on && (*number == off->value || *number == on->value)) {
                    bool value = *number == on->value;
                    if (row_switch(item.label, "", value)) *number = value ? on->value : off->value;
                } else if (begin_row(item.label, "")) {
                    // A value the launcher has no name for shows as its number.
                    std::string preview = std::to_string(*number);
                    for (const auto& choice : item.choices)
                        if (choice.value == *number) preview = choice.label;
                    if (ImGui::BeginCombo("##value", preview.c_str())) {
                        for (const auto& choice : item.choices)
                            if (ImGui::Selectable(choice.label, choice.value == *number)) *number = choice.value;
                        ImGui::EndCombo();
                    }
                    end_row();
                }
            }
            break;
        case game::Kind::resolution:
            // The game saves a place in this PC's list of display sizes.
            if (auto* number = std::get_if<std::int32_t>(&found->second); number && begin_row(item.label, "")) {
                const auto name = [&](int index) {
                    if (index < 0 || index >= static_cast<int>(pages.resolutions.size())) return std::format("Mode {}", index);
                    const auto [width, height] = pages.resolutions[static_cast<std::size_t>(index)];
                    return std::format("{} x {}", width, height);
                };
                if (ImGui::BeginCombo("##value", name(*number).c_str())) {
                    for (int index = 0; index < static_cast<int>(pages.resolutions.size()); ++index)
                        if (ImGui::Selectable(name(index).c_str(), index == *number)) *number = index;
                    ImGui::EndCombo();
                }
                end_row();
            }
            break;
        }
    }
    if (section) end_group();
    ImGui::EndDisabled();
}

// Settings > GRAPHICS, under the display settings: the game's graphics options, which it
// saves together as one text.
void graphics_options(Ui& ui, bool running) {
    auto& pages = ui.game;
    const auto text = pages.values.find(game::options_key);
    if (text == pages.values.end() || !std::holds_alternative<std::string>(text->second)) return;
    game::Options options;
    try {
        options = game::parse_options(std::get<std::string>(text->second));
    } catch (const std::exception&) {
        return;
    }
    const auto value_of = [&](std::string_view name) -> std::string* {
        for (auto& option : options)
            if (option.name == name) return &option.value;
        return nullptr;
    };
    bool changed = false;
    ImGui::BeginDisabled(running);
    // Every quality option at once: the level they all stand at is the one lit, and none is when
    // they have been set apart.
    begin_group("QUALITY");
    static constexpr std::array<const char*, 4> levels{"Low", "Medium", "High", "Ultra"};
    const auto& fonts = *g_page.fonts;
    // (As wide as its four parts come to, so the control ends where the others do.)
    float preset_width = 0;
    for (const char* level : levels) preset_width += S(28) + fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, level).x;
    if (begin_row("Preset", "Sets every quality option below.", nullptr, preset_width)) {
        const auto stands_at = [&](int level) {
            bool any = false;
            for (const auto& setting : game::graphics_options())
                if (const auto* wanted = game::level_value(setting, level))
                    if (const auto* value = value_of(setting.name)) {
                        if (*value != wanted) return false;
                        any = true;
                    }
            return any;
        };
        const float frame = ImGui::GetFrameHeight();
        for (int level = 0; level < static_cast<int>(levels.size()); ++level) {
            if (!segment(fonts, std::format("##level{}", level).c_str(), levels[static_cast<std::size_t>(level)], {}, stands_at(level),
                         segment_place(static_cast<std::size_t>(level), levels.size()), frame))
                continue;
            for (const auto& setting : game::graphics_options())
                if (const auto* wanted = game::level_value(setting, level))
                    if (auto* value = value_of(setting.name)) *value = wanted, changed = true;
        }
        end_row();
    }
    end_group();
    const char* category = nullptr;
    for (const auto& setting : game::graphics_options()) {
        // An option this game version does not save is not offered.
        auto* value = value_of(setting.name);
        if (!value) continue;
        if (!category || std::string_view(category) != setting.category) {
            if (category) end_group();
            category = setting.category;
            begin_group(category);
        }
        const auto* needed = *setting.needs_name ? value_of(setting.needs_name) : nullptr;
        ImGui::BeginDisabled(*setting.needs_name && (!needed || *needed != setting.needs_value));
        if (const auto [off, on] = on_and_off(setting.choices); off && on && (*value == off->value || *value == on->value)) {
            bool flipped = *value == on->value;
            if (row_switch(setting.label, "", flipped)) *value = flipped ? on->value : off->value, changed = true;
        } else if (begin_row(setting.label, "")) {
            // A value the launcher has no name for (set in game, or by a newer game) shows as it is.
            const char* preview = value->c_str();
            for (const auto& choice : setting.choices)
                if (*value == choice.value) preview = choice.label;
            if (ImGui::BeginCombo("##value", preview)) {
                for (const auto& choice : setting.choices)
                    if (ImGui::Selectable(choice.label, *value == choice.value) && *value != choice.value)
                        *value = choice.value, changed = true;
                ImGui::EndCombo();
            }
            end_row();
        }
        ImGui::EndDisabled();
    }
    if (category) end_group();
    ImGui::EndDisabled();
    if (!changed) return;
    // The game's preset no longer describes them.
    if (auto* preset = value_of(game::preset_name)) *preset = game::preset_custom;
    try {
        text->second = game::join_options(options);
    } catch (const std::exception&) {}
}

// ---------------------------------------------------------------- the launcher's own pages

// GAME: where the game is, how it starts, and what a mod maker wants of it.
void page_game(Launcher& launcher, Ui& ui, bool& open) {
    auto& settings = launcher.settings();
    const auto& paths = launcher.session().paths;
    begin_group("GAME FOLDER");
    if (begin_row(utf8(paths.directory.wstring()).c_str(), "Where skate. is installed.", nullptr, S(250))) {
        if (ImGui::Button("Open", ImVec2(S(96), 0))) open_path(paths.directory);
        ImGui::SameLine(0, S(8));
        ImGui::BeginDisabled(launcher.busy());
        if (ImGui::Button("Verify files", ImVec2(S(146), 0))) {
            open = false;
            open_sign_in(launcher, ui, true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Checks the game's files against Steam's and downloads any that differ");
        end_row();
    }
    end_group();

    begin_group("PLAY");
    row_switch("Offline mode", "Play without Steam; multiplayer is hidden.", settings.offline,
               "Play without Steam as Unknown Player; multiplayer is hidden. Used automatically whenever Steam isn't running.");
    {
        const auto* chosen = settings.start_map.empty() ? nullptr
            : launcher_start_maps::find_start_map(start_map_list(launcher.session(), ui), settings.start_map);
        const bool missing = !settings.start_map.empty() && !chosen;
        if (begin_row("Start map", missing ? "Its mod is missing or off: Skate starts in San Vansterdam." : "Loads as soon as the game is up.",
                missing ? "This map's mod is not installed, is switched off, or could not be merged: Skate starts in San Vansterdam until it is back."
                        : "skate. always opens in San Vansterdam; another map loads as soon as the game is up. A map mod that is "
                          "switched off or removed later falls back to San Vansterdam.")) {
            if (ImGui::BeginCombo("##start_map", start_map_name(settings, launcher.session(), ui).c_str())) {
                if (start_map_items(settings, launcher.session(), ui)) launcher.save();
                ImGui::EndCombo();
            }
            end_row();
        }
    }
    row_switch("Discord status", "Show where you are skating on your Discord profile.", settings.discord_status,
               "Show on your Discord profile where you are skating: the map, the server or lobby and how many are in it. "
               "Off, the game does not talk to Discord at all.");
    row_switch("Loose files", "Export the game's scripts and config beside Skate.exe.", settings.loose_files,
               "Export the game's scripts/ and config/ beside Skate.exe so you can edit them.");
    end_group();

    begin_group("FOR MOD MAKERS");
    if (!ui.plugin_developer) {
        try { ui.plugin_developer = initfs::plugin_developer_mode(paths.directory); } catch (const std::exception&) { ui.plugin_developer = false; }
    }
    bool developer = *ui.plugin_developer;
    row_switch("Plugin developer mode", "Plugins you approved update without asking again.", developer,
               "Updates to plugins you already approved won't ask again while this is on. A plugin you have not approved is "
               "still held until you answer.");
    if (developer != *ui.plugin_developer) {
        try { initfs::set_plugin_developer_mode(paths.directory, developer); } catch (const std::exception& error) {
            logging::log(logging::Level::warning, logging::Channel::launcher, "Plugin developer mode was not saved: {}", error.what());
        }
        ui.plugin_developer = developer;
    }
    if (begin_row("Extra game arguments", "Passed to Skate.exe as they are.", nullptr, S(420))) {
        std::array<char, 1024> arguments{};
        std::copy_n(settings.arguments.begin(), std::min(settings.arguments.size(), arguments.size() - 1), arguments.begin());
        if (ImGui::InputTextWithHint("##arguments", "None", arguments.data(), arguments.size())) settings.arguments = arguments.data();
        end_row();
    }
    end_group();
}

// DISPLAY: the window the game starts in, when it is not left to the game's own display settings.
void page_display(Launcher& launcher) {
    auto& settings = launcher.settings();
    begin_group("WINDOW");
    row_switch("Windowed", "Start in a window, not as the game's display settings say.", settings.windowed,
               "Start in a window of the size below instead of the game's saved display settings.");
    ImGui::BeginDisabled(!settings.windowed);
    if (begin_row("Window size", "Its width and height, in pixels.")) {
        const float each = (S(row_control) - S(28)) * 0.5f;
        ImGui::SetNextItemWidth(each);
        ImGui::InputInt("##width", &settings.width, 0);
        ImGui::SameLine(0, S(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("x");
        ImGui::SameLine(0, S(8));
        ImGui::SetNextItemWidth(each);
        ImGui::InputInt("##height", &settings.height, 0);
        end_row();
    }
    ImGui::EndDisabled();
    settings.width = std::clamp(settings.width, 320, 16384);
    settings.height = std::clamp(settings.height, 200, 16384);
    end_group();
}

// KEYS: the keys that open ReSkate's menu and console in game. True when a key changed, so the
// caller can save straight away.
bool page_keys(Settings& settings, Ui& ui) {
    bool changed = false;
    if (ui.binding && !g_capturing_key) {
        const auto key = g_captured_key.exchange(0);
        int& target = ui.binding == 1 ? settings.menu_key : settings.console_key;
        const int other = ui.binding == 1 ? settings.console_key : settings.menu_key;
        if (key == VK_ESCAPE) ui.key_error.clear();
        else if (!launcher::bindable_key(key))
            ui.key_error = launcher::key_name(key) + " can't be used. Pick another key.";
        else if (static_cast<int>(key) == other)
            ui.key_error = launcher::key_name(key) + " already opens the " + (ui.binding == 1 ? "console." : "menu.");
        else { changed = target != static_cast<int>(key); target = static_cast<int>(key); ui.key_error.clear(); }
        ui.binding = 0;
    }
    begin_group("IN-GAME KEYS");
    const auto row = [&](int which, const char* label, const char* note, int key) {
        if (!begin_row(label, note, "Click the key, then press the one you want. Esc cancels. The chosen key stops reaching the game "
                                    "while it runs, so avoid keys you skate with."))
            return;
        const bool waiting = ui.binding == which;
        const auto text = (waiting ? std::string("Press a key...") : launcher::key_name(static_cast<unsigned>(key))) + "##key";
        if (waiting) push_primary_button();
        if (ImGui::Button(text.c_str(), ImVec2(S(row_control), 0)) && !waiting) {
            ui.binding = which;
            ui.key_error.clear();
            g_captured_key = 0;
            g_capturing_key = true;
        }
        if (waiting) pop_primary_button();
        end_row();
    };
    row(1, "ReSkate menu", "Opens and closes the ReSkate menu.", settings.menu_key);
    row(2, "Console", "Opens and closes the command console.", settings.console_key);
    const bool defaults = settings.menu_key == static_cast<int>(launcher::default_menu_key) &&
                          settings.console_key == static_cast<int>(launcher::default_console_key);
    if (begin_row("Defaults", "Insert for the menu, ` for the console.", nullptr, S(170))) {
        ImGui::BeginDisabled(defaults || ui.binding);
        if (ImGui::Button("Reset both", ImVec2(S(170), 0))) {
            settings.menu_key = static_cast<int>(launcher::default_menu_key);
            settings.console_key = static_cast<int>(launcher::default_console_key);
            ui.key_error.clear();
            changed = true;
        }
        ImGui::EndDisabled();
        end_row();
    }
    end_group();
    if (!ui.key_error.empty()) notice(ui.key_error.c_str(), color::danger);
    else if (g_page.query.empty())
        notice("Click a key, then press the one you want. Esc cancels. The chosen keys stop reaching the game while it runs, "
               "so avoid keys you skate with.");
    return changed;
}

// ADVANCED: the launcher itself, its updates, and what helps when something goes wrong.
void page_advanced(Launcher& launcher, Ui& ui) {
    auto& settings = launcher.settings();
    begin_group("LAUNCHER");
    if (begin_row("Interface size", "The launcher's text and layout. Takes a restart.",
            "Makes the launcher's text and layout bigger or smaller. A size too big for this screen is cut to the largest that "
            "fits. Applies when the launcher starts again.")) {
        const auto chosen = std::ranges::find(interface_sizes, settings.interface_size);
        int size_index = static_cast<int>(chosen - interface_sizes.begin());
        if (ImGui::Combo("##interface_size", &size_index, interface_size_names.data(), static_cast<int>(interface_size_names.size()))) {
            settings.interface_size = interface_sizes[static_cast<std::size_t>(size_index)];
            launcher.save();
        }
        end_row();
    }
    // Fonts and the window's smallest size are made at startup, so the new size needs a new window.
    if (settings.interface_size != g_interface_size && begin_row("Restart the launcher", "To use the new interface size.", nullptr, S(170))) {
        ImGui::BeginDisabled(launcher.busy());
        push_primary_button();
        if (ImGui::Button("RESTART", ImVec2(S(170), 0))) {
            launcher.save();
            launcher.request_restart();
        }
        pop_primary_button();
        ImGui::EndDisabled();
        end_row();
    }
    if (begin_row("Mod downloads at once", "Fewer suits a slow connection.",
            "How many mods the Mod Manager downloads at the same time. Each is installed as its download ends. Fewer suits a "
            "slow connection.")) {
        const float span = static_cast<float>(max_parallel_downloads - 1);
        if (slider(S(row_control), static_cast<float>(settings.parallel_downloads - 1) / span, std::to_string(settings.parallel_downloads), [&] {
                return ImGui::SliderInt("##parallel_downloads", &settings.parallel_downloads, 1, max_parallel_downloads, "", ImGuiSliderFlags_AlwaysClamp);
            }))
            launcher.save();
        end_row();
    }
    row_switch("Keep the launcher open", "Hide while you play and come back when Skate closes.", settings.keep_open_after_launch,
               "Hide while you play and return when Skate closes. Turn off to close after a successful launch.");
    end_group();

    begin_group("UPDATES");
    {
        const bool updates_were = settings.updates, prereleases_were = settings.prereleases;
        row_switch("Install ReSkate updates", "Off keeps the ReSkate files you have, such as a test build.", settings.updates,
                   "Turn off to keep the ReSkate.dll and launcher you have, such as a test build someone gave you. Skate's own "
                   "Steam updates are not affected.");
        // Only while there is a pre-release to test (one newer than the latest release), or while
        // it is on, so it can always be turned off.
        if (settings.prereleases || launcher.prerelease_available())
            row_switch("Test pre-releases", "Get what is coming before it is released. Less tested.", settings.prereleases,
                       "Update to the newest pre-release on GitHub, to try what is coming before it is released. Pre-releases "
                       "are less tested. Turn off to go back to the latest release.");
        // Either flipped: checked again now, so PLAY or the update button changes to match.
        if (settings.updates != updates_were || settings.prereleases != prereleases_were) {
            launcher.save();
            launcher.check_again_if_update_choices_changed();
        }
    }
    end_group();

    begin_group("TROUBLESHOOTING");
    if (begin_row("Log level", "How much ReSkate.log records.", "How much ReSkate.log records. Use debug or trace when reporting a problem.")) {
        const float logs = S(110);
        ImGui::SetNextItemWidth(S(row_control) - logs - S(8));
        ImGui::Combo("##log_level", &settings.log_level, log_levels.data(), static_cast<int>(log_levels.size()));
        ImGui::SameLine(0, S(8));
        if (ImGui::Button("Open logs", ImVec2(logs, 0))) open_path(launcher.session().paths.logs);
        end_row();
    }
    row_switch("GPU crash diagnostics (DRED)", "Records more when the graphics driver crashes. Slightly slower.", settings.gpu_diagnostics);
    row_switch("Send crash reports", "Upload a crash dump and that session's log when ReSkate crashes.", settings.crash_reports,
               "When the launcher or Skate crashes, upload a crash dump and that session's log so the ReSkate developers can fix "
               "it. The launcher picks a change up when it next starts.");
    row_switch("Send performance timings", "How long loads took and each level's frame rate. Nothing about your account.",
               settings.performance_timings,
               "Send how long downloads, mod installs, the mod merge, level loads and joining a session took, and each level's "
               "average frame rate, so slow spots can be found and fixed. Nothing about your account is included. Needs crash "
               "reports on; the launcher picks a change up when it next starts.");
    if (begin_row("Privacy and terms", "What ReSkate collects and sends, and the rules for using it.", nullptr, S(300))) {
        const float each = (S(300) - S(8)) * 0.5f;
        if (ImGui::Button("Privacy Policy", ImVec2(each, 0))) ui.legal.open = Legal::privacy;
        ImGui::SameLine(0, S(8));
        if (ImGui::Button("Terms of Service", ImVec2(each, 0))) ui.legal.open = Legal::terms;
        end_row();
    }
    end_group();
}

// The pages: the launcher's own, then the game's (game_settings.h, which names its pages so).
constexpr std::array<const char*, 9> page_names{"GAME", "DISPLAY", "KEYS", "ADVANCED", "GRAPHICS", "AUDIO", "CAMERA", "CONTROLS", "REPLAY"};
constexpr int launcher_pages = 4;
// What is typed in the search, until Settings closes.
std::array<char, 64> g_search{};

} // namespace

const std::vector<launcher_start_maps::StartMap>& start_map_list(const launcher_app::Session& session, Ui& ui) {
    if (!ui.start_maps) {
        try {
            ui.start_maps = launcher_start_maps::start_maps(session.paths.directory);
        } catch (const std::exception&) {
            ui.start_maps.emplace();
        }
    }
    return *ui.start_maps;
}

std::string start_map_name(const Settings& settings, const launcher_app::Session& session, Ui& ui) {
    if (settings.start_map.empty()) return world_level_name(launcher_start_maps::san_vansterdam);
    const auto* chosen = launcher_start_maps::find_start_map(start_map_list(session, ui), settings.start_map);
    return chosen ? chosen->name : world_level_short_name(settings.start_map) + " (not installed)";
}

bool start_map_items(Settings& settings, const launcher_app::Session& session, Ui& ui) {
    const auto& maps = start_map_list(session, ui);
    const auto* chosen = settings.start_map.empty() ? nullptr : launcher_start_maps::find_start_map(maps, settings.start_map);
    bool changed = false, mods_shown = false;
    for (std::size_t index = 0; index < maps.size(); ++index) {
        const auto& map = maps[index];
        if (!map.mod.empty() && !mods_shown) {
            mods_shown = true;
            ImGui::SeparatorText("MAP MODS");
        }
        const bool san_vansterdam = map.asset == launcher_start_maps::san_vansterdam;
        const bool selected = san_vansterdam ? settings.start_map.empty() : &map == chosen;
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(map.name.c_str(), selected) && !selected) {
            settings.start_map = san_vansterdam ? std::string() : map.asset;
            changed = true;
        }
        if (!map.mod.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", map.mod.c_str());
        if (selected) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    return changed;
}

void settings_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, HWND window) {
    bool& open = ui.settings;
    // A page, like the mod manager: the same tiles along the top, the same way out.
    const auto frame = begin_page("##settings_panel", size);
    auto* draw = ImGui::GetWindowDrawList();
    auto& settings = launcher.settings();
    const bool binding = ui.binding != 0;
    ui.settings_tab = std::clamp(ui.settings_tab, 0, static_cast<int>(page_names.size()) - 1);
    const bool game_side = ui.settings_tab >= launcher_pages;

    window_buttons(draw, window, frame);
    const float edge = S(28);
    const float top = S(44), bottom = frame.y - S(24);
    const float tiles_tall = S(54), foot = S(34);

    // ------------------------------------------------ along the top: the title, whose settings, and the search
    const float title_size = S(34), title_width = S(232);
    page_title(draw, fonts, ImVec2(edge + S(2), top + (tiles_tall - title_size) * 0.5f), "SETTINGS", title_size);
    ImGui::SetCursorPos(ImVec2(edge + title_width, top));
    // Flattened, so a controller's D-pad crosses from the tiles into the page and back.
    ImGui::BeginChild("##settings_rail", ImVec2(frame.x - edge * 2 - title_width, tiles_tall), ImGuiChildFlags_NavFlattened);
    ImGui::BeginDisabled(binding);
    {
        // Each side opens on the page it was last on.
        static int last_page[2]{0, launcher_pages};
        last_page[game_side ? 1 : 0] = ui.settings_tab;
        const ImVec2 origin = ImGui::GetCursorPos();
        const float launcher_width = S(196), game_width = S(250), gap = S(8);
        if (nav_tile(fonts, launcher_width, "LAUNCHER", !game_side)) ui.settings_tab = last_page[0];
        // A controller starts on the open side's tile.
        if (!game_side) default_focus();
        ImGui::SetCursorPos(ImVec2(origin.x + launcher_width + gap, origin.y));
        if (nav_tile(fonts, game_width, "SKATE. SETTINGS", game_side, {}, false, "skate.'s own settings, the ones in its Settings menu"))
            ui.settings_tab = last_page[1];
        if (game_side) default_focus();
        // Ctrl+F jumps to the search, as it does in a browser.
        const float search = S(300);
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - search, origin.y + (tiles_tall - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(search);
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##settings_search", "Search settings  (Ctrl+F)", g_search.data(), g_search.size());
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    g_page = {};
    g_page.fonts = &fonts;
    g_page.limit = frame.x - edge * 2;
    g_page.query = lower(g_search.data());
    while (!g_page.query.empty() && g_page.query.back() == ' ') g_page.query.pop_back();
    const bool searching = !g_page.query.empty();

    // ------------------------------------------------ the open side's pages, as tabs; or what is searched for
    const float content_top = top + tiles_tall + S(16), content_bottom = bottom - foot - S(10);
    ImGui::SetCursorPos(ImVec2(edge, content_top));
    // Rows across the whole page, as the mod manager's list is. The page itself runs on to the
    // window's edge, so its scrollbar is there, clear of the boxes, which stop at the page's margin.
    ImGui::BeginChild("##settings_content", ImVec2(frame.x - edge - S(5), content_bottom - content_top), ImGuiChildFlags_NavFlattened);
    ImGui::BeginDisabled(binding);
    if (searching) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Settings that match \"%s\", from every page.", g_search.data());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) g_search.fill(0);
    } else {
        const int first = game_side ? launcher_pages : 0, last = game_side ? static_cast<int>(page_names.size()) : launcher_pages;
        const float tab_tall = ImGui::GetFrameHeight();
        for (int index = first; index < last; ++index) {
            // (As they are named on their tiles, which were in capitals: here in the case the rows are in.)
            std::string label = lower(page_names[static_cast<std::size_t>(index)]);
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            if (segment(fonts, std::format("##tab{}", index).c_str(), label.c_str(), {}, ui.settings_tab == index,
                        segment_place(static_cast<std::size_t>(index - first), static_cast<std::size_t>(last - first)), tab_tall))
                ui.settings_tab = index;
        }
        ImGui::NewLine();
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(2)));

    // A slim scrollbar, round at its ends: grey, lighter under the pointer, blue while it is dragged.
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, S(4));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.16f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.32f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(color::blue));
    ImGui::BeginChild("##settings_page", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_NavFlattened);
    // The game's own settings: read once, and said once why there are none or why they are held.
    bool running{};
    bool game_ready = false, game_asked = false;
    const auto game_page = [&](int index, bool notices) {
        if (!game_asked) {
            game_asked = true;
            game_ready = game_settings_ready(launcher, ui, running);
        }
        if (notices) {
            if (!game_ready) {
                notice(ui.game.error.empty() ? "skate. has not saved its settings on this PC yet. Start the game once and they appear here."
                                             : ui.game.error.c_str());
                return;
            }
            if (running) notice("Skate is running: close it to change these here.", color::warning);
            else notice("skate.'s own settings, the ones in its Settings menu. The game reads them when it starts.");
            if (!ui.game.error.empty()) notice(ui.game.error.c_str(), color::danger);
        }
        if (!game_ready) return;
        const std::string_view page = page_names[static_cast<std::size_t>(index)];
        game_settings_page(ui, page, running);
        if (page == "GRAPHICS") graphics_options(ui, running);
    };
    const auto draw_page = [&](int index) {
        switch (index) {
        case 0: page_game(launcher, ui, open); break;
        case 1: page_display(launcher); break;
        case 2:
            if (page_keys(settings, ui)) launcher.save();
            break;
        case 3: page_advanced(launcher, ui); break;
        default: game_page(index, !searching); break;
        }
    };
    if (searching) {
        for (int index = 0; index < static_cast<int>(page_names.size()); ++index) {
            g_page.heading = page_names[static_cast<std::size_t>(index)];
            draw_page(index);
        }
        g_page.heading = nullptr;
        if (!g_page.any) {
            ImGui::Dummy(ImVec2(0, S(8)));
            ImGui::TextDisabled("No setting matches. Try fewer letters, or a word from what the setting does.");
        }
    } else {
        draw_page(ui.settings_tab);
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    store_game_settings(ui);
    ImGui::EndChild();

    // ------------------------------------------------ the foot: the way back, and what build this is
    // (BACK is plain, as in the mod manager: blue is for what a page is there to do.)
    ImGui::SetCursorPos(ImVec2(edge, bottom - foot));
    ImGui::BeginDisabled(binding);
    if (ImGui::Button("\xe2\x86\x90  BACK", ImVec2(S(130), foot))) open = false;
    ImGui::EndDisabled();
    {
        const char* build = !update::binary_updates_enabled() ? "Development build: ReSkate files are never replaced."
                          : settings.updates ? "Release build: updates install automatically."
                                             : "Updates are off: ReSkate files are never replaced.";
        const float across = fonts.body->CalcTextSizeA(fonts.body->FontSize, FLT_MAX, 0, build).x;
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(frame.x - edge - across, bottom - foot + (foot - fonts.body->FontSize) * 0.5f),
                      color::muted, build);
    }

    if (!binding && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Escape lets go of the search first, and leaves the page after that.
        if (searching) g_search.fill(0);
        else open = false;
    }
    g_drag_allowed = !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::End();
    if (!open) {
        launcher.save();
        launcher.check_again_if_update_choices_changed();
        g_search.fill(0);
        // Read again the next time: the game may have run in between, and mods may have changed.
        ui.game.loaded = false;
        ui.start_maps.reset();
        ui.plugin_developer.reset();
    }
}

} // namespace dingosdk::launcher_gui::detail
