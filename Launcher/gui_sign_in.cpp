#include "gui_internal.h"

namespace dingosdk::launcher_gui::detail {

// Shown once when the launcher opens without Steam, so nobody reaches the main
// menu as Unknown Player wondering why multiplayer is missing.
void steam_offline_window(const Fonts& fonts, ImVec2 size, Ui& ui) {
    const auto panel = begin_panel("##steam_offline_panel", size, ImVec2(S(440), S(300)));
    panel_title(fonts, "STEAM IS NOT RUNNING");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Steam is not running, or no account is signed in. PLAY still works, but Skate starts in "
                        "offline mode: you play as Unknown Player and multiplayer is hidden.");
    ImGui::Spacing();
    ImGui::TextDisabled("To play as your own account, start Steam, sign in, and open ReSkate again.");
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    if (ImGui::Button("OK", ImVec2(S(110), 0))) ui.steam_offline = false;
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
