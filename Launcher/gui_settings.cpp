#include "gui_internal.h"

// The Settings panel.
namespace dingosdk::launcher_gui::detail {
namespace {

// A muted explanation line.
void setting_note(const char* text) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

// A checkbox with its label and a muted explanation stacked beside the box,
// so the explanation sits right under the label instead of under the box.
void setting_check(const char* label, const char* note, bool& value) {
    ImGui::PushID(label);
    const auto start = ImGui::GetCursorPos();
    ImGui::Checkbox("##check", &value);
    const float below_box = ImGui::GetCursorPosY();
    // Not SameLine: that would drop the text to the box's frame padding. The
    // label's top lines up with the box's top.
    ImGui::SetCursorPos(ImVec2(start.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x, start.y));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, S(1)));
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemClicked()) value = !value;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", note);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    if (ImGui::GetCursorPosY() < below_box) ImGui::SetCursorPosY(below_box);
    ImGui::PopID();
    ImGui::Spacing();
    ImGui::Spacing();
}

void section_caption(const Fonts& fonts, const char* text) {
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("%s", text);
    ImGui::PopFont();
}

// Settings > KEYS: the keys that open ReSkate's menu and console in game.
// Returns true when a key changed, so the caller can save straight away.
bool key_bindings(const Fonts& fonts, Settings& settings, Ui& ui) {
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
    section_caption(fonts, "IN-GAME KEYS");
    setting_note("Click a key, then press the one you want. Esc cancels.");
    const auto row = [&](int which, const char* label, const char* detail, int key) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(S(200));
        const bool waiting = ui.binding == which;
        const auto text = (waiting ? std::string("Press a key...")
                                   : launcher::key_name(static_cast<unsigned>(key))) + "##key" + std::to_string(which);
        if (waiting) push_primary_button();
        if (ImGui::Button(text.c_str(), ImVec2(-1, 0)) && !waiting) {
            ui.binding = which;
            ui.key_error.clear();
            g_captured_key = 0;
            g_capturing_key = true;
        }
        if (waiting) pop_primary_button();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(200));
        ImGui::TextDisabled("%s", detail);
        ImGui::Spacing();
    };
    row(1, "ReSkate menu", "Opens and closes the ReSkate menu.", settings.menu_key);
    row(2, "Console", "Opens and closes the command console.", settings.console_key);
    if (!ui.key_error.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "%s", ui.key_error.c_str());
    ImGui::Spacing();
    const bool defaults = settings.menu_key == static_cast<int>(launcher::default_menu_key) &&
                          settings.console_key == static_cast<int>(launcher::default_console_key);
    ImGui::BeginDisabled(defaults || ui.binding);
    if (ImGui::Button("Reset to defaults")) {
        settings.menu_key = static_cast<int>(launcher::default_menu_key);
        settings.console_key = static_cast<int>(launcher::default_console_key);
        ui.key_error.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    setting_note("The chosen keys stop reaching the game while it runs, so avoid keys you skate with.");
    return changed;
}

} // namespace

void settings_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, HWND window) {
    bool& open = ui.settings;
    // A page, like the mod manager: the same rail, the same way out.
    const auto frame = begin_page("##settings_panel", size);
    auto* draw = ImGui::GetWindowDrawList();
    auto& settings = launcher.settings();
    const auto& paths = launcher.session().paths;
    const bool busy = launcher.busy();
    const bool binding = ui.binding != 0;

    window_buttons(draw, window, frame);
    const float rail_x = S(28), rail_width = S(236);
    const float content_x = rail_x + rail_width + S(26);
    const float top = S(52), bottom = frame.y - S(24);
    const float title_size = S(44);
    page_title(draw, fonts, ImVec2(rail_x + S(2), top - S(4)), "SETTINGS", title_size);

    // ------------------------------------------------ rail
    const float rail_top = top + title_size + S(20);
    ImGui::SetCursorPos(ImVec2(rail_x, rail_top));
    // Flattened, so a controller's D-pad crosses from the rail into the page and back.
    ImGui::BeginChild("##settings_rail", ImVec2(rail_width, bottom - rail_top), ImGuiChildFlags_NavFlattened);
    static constexpr std::array<const char*, 4> names{"GAME", "DISPLAY", "KEYS", "ADVANCED"};
    ImGui::BeginDisabled(binding);
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        if (nav_tile(fonts, rail_width, names[static_cast<std::size_t>(i)], ui.settings_tab == i))
            ui.settings_tab = i;
        // A controller starts on the open tab's tile.
        if (ui.settings_tab == i) default_focus();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(38));
    push_primary_button();
    if (ImGui::Button("\xe2\x86\x90  BACK", ImVec2(-1, S(34)))) open = false;
    pop_primary_button();
    ImGui::EndDisabled();
    ImGui::EndChild();

    // ------------------------------------------------ the open tab
    // Kept to a readable column: settings are sentences, not a table.
    ImGui::SetCursorPos(ImVec2(content_x, top));
    ImGui::BeginChild("##settings_content",
        ImVec2(std::min(S(760), frame.x - content_x - S(28)), bottom - top), ImGuiChildFlags_NavFlattened);
    const float footer = ImGui::GetTextLineHeight() + S(16);
    ImGui::BeginChild("##settings_page", ImVec2(0, ImGui::GetWindowHeight() - footer), ImGuiChildFlags_NavFlattened);
    switch (ui.settings_tab) {
    case 0: {
        section_caption(fonts, "GAME FOLDER");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(utf8(paths.directory.wstring()).c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Open folder")) open_path(paths.directory);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Verify game files")) { open = false; open_sign_in(launcher, ui, true); }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::Spacing();
        section_caption(fonts, "PLAY");
        setting_check("Offline mode", "Play without Steam as Unknown Player; multiplayer is hidden. "
                      "Used automatically whenever Steam isn't running.", settings.offline);
        setting_check("Loose files", "Export the game's scripts/ and config/ beside Skate.exe so you can edit them.",
                      settings.loose_files);
        ImGui::Spacing();
        section_caption(fonts, "EXTRA GAME ARGUMENTS");
        std::array<char, 1024> arguments{};
        std::copy_n(settings.arguments.begin(), std::min(settings.arguments.size(), arguments.size() - 1), arguments.begin());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##arguments", "Arguments for Skate.exe", arguments.data(), arguments.size()))
            settings.arguments = arguments.data();
        setting_note("Passed to Skate.exe as they are.");
        break;
    }
    case 1:
        section_caption(fonts, "WINDOW");
        setting_check("Windowed", "Start in a window of this size instead of the game's saved display settings.",
                      settings.windowed);
        ImGui::BeginDisabled(!settings.windowed);
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetNextItemWidth(S(110));
        ImGui::InputInt("##width", &settings.width, 0);
        ImGui::SameLine();
        ImGui::TextDisabled("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(110));
        ImGui::InputInt("##height", &settings.height, 0);
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::EndDisabled();
        settings.width = std::clamp(settings.width, 320, 16384);
        settings.height = std::clamp(settings.height, 200, 16384);
        break;
    case 2:
        if (key_bindings(fonts, settings, ui)) launcher.save();
        break;
    default:
        section_caption(fonts, "LAUNCHER");
        setting_check("Keep the launcher open after launch",
                      "Hide while you play and return when Skate closes. Turn off to close after a successful launch.",
                      settings.keep_open_after_launch);
        setting_check("Install ReSkate updates",
                      "Turn off to keep the ReSkate.dll and launcher you have, such as a test build someone gave you. "
                      "Skate's own Steam updates are not affected.",
                      settings.updates);
        ImGui::Spacing();
        section_caption(fonts, "TROUBLESHOOTING");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Log level");
        ImGui::SameLine(S(200));
        ImGui::SetNextItemWidth(S(160));
        ImGui::Combo("##log_level", &settings.log_level, log_levels.data(), static_cast<int>(log_levels.size()));
        ImGui::SameLine();
        if (ImGui::Button("Open logs")) open_path(paths.logs);
        setting_note("How much ReSkate.log records. Use debug or trace when reporting a problem.");
        ImGui::Spacing();
        setting_check("GPU crash diagnostics (DRED)", "Records extra detail when the graphics driver crashes. Slightly slower.",
                      settings.gpu_diagnostics);
        setting_check("Send crash reports",
                      "When the launcher or Skate crashes, upload a crash dump and that session's log so the ReSkate "
                      "developers can fix it. The launcher picks a change up when it next starts.",
                      settings.crash_reports);
        break;
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer + S(8));
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("%s", !update::binary_updates_enabled() ? "Development build: ReSkate files are never replaced."
                              : settings.updates ? "Release build: updates install automatically."
                                                 : "Updates are off: ReSkate files are never replaced.");
    ImGui::PopFont();
    ImGui::EndChild();

    if (!binding && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open = false;
    g_drag_allowed = !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::End();
    if (!open) launcher.save();
}

} // namespace dingosdk::launcher_gui::detail
