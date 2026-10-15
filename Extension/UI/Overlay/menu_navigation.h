#pragma once
#include <imgui_internal.h>

namespace dingosdk::overlay::menu {
// Own cancel before NewFrame so ImGui cannot cancel a child/popup and then
// let the menu interpret the same press as another Back action.
inline void claim_menu_back(bool enabled) {
    if (enabled) ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, ImHashStr("ReSkate menu back"));
}
inline void focus_menu_panel(ImGuiWindow* window, bool initialize = false) {
    if (!window) return;
    ImGui::FocusWindow(window);
    ImGui::NavInitWindow(window, initialize);
    ImGui::SetNavCursorVisibleAfterMove();
}
inline void menu_back(ImGuiWindow* sidebar, ImGuiWindow* content) {
    auto& g = *ImGui::GetCurrentContext();
    if (!ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) return;
    if (g.ActiveId) { ImGui::ClearActiveID(); return; }
    if (!g.OpenPopupStack.empty()) {
        const auto* popup = g.OpenPopupStack.back().Window;
        if (popup && !(popup->Flags & ImGuiWindowFlags_Modal))
            ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
        return;
    }
    auto* window = g.NavWindow;
    if (window && content && window != content) {
        for (auto* parent = window->ParentWindow; parent; parent = parent->ParentWindow) {
            if (parent == content) {
                // Back out of one nested panel to its parent/tab controls.
                auto* target = window->ParentWindow;
                ImGui::FocusWindow(target);
                ImGui::SetNavID(window->ChildId, ImGuiNavLayer_Main, target->NavRootFocusScopeId,
                               ImGui::WindowRectAbsToRel(target, window->Rect()));
                ImGui::SetNavCursorVisibleAfterMove();
                return;
            }
        }
    }
    // The right-hand page is the outermost panel. Back returns to its sidebar
    // selection; another Back there leaves the menu open.
    focus_menu_panel(sidebar);
}
} // namespace dingosdk::overlay::menu
