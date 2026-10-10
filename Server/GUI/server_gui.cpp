// ReSkate Dedicated Server GUI
// Built for DingoSDK ReSkate Dedicated Server — Made with love by wxndr

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "server_gui.h"
#include "server_gui_renderer.h"
#include "../server_engine.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/UI/Overlay/skate_style.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/UI/Overlay/role_badge.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#ifdef small
#undef small
#endif
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace dingosdk::server_gui {

namespace fs = std::filesystem;
using namespace skate_theme;
using namespace dingosdk::server;

namespace {

HWND g_window = nullptr;
Renderer g_renderer;
GuiState g_state;
GuiFonts g_fonts;
float g_scale = 1.0f;
bool g_drag_allowed = true;

inline float S(float v) { return v * g_scale; }
constexpr ImU32 muted = skate_theme::grey_text;

// ============================================================================
// 1-1 In-Game Nametag, Role Badge, and Animated Gradient Architecture
// (Directly using game headers: custom_nametags.h, role_badge.h, nametag_gradient.h)
// ============================================================================
using namespace dingosdk::overlay::detail;
using namespace dingosdk::multiplayer;

// 1:1 In-Game Player Role & Tag Resolver (session_view.cpp)
inline std::pair<std::uint32_t, std::string> player_role(std::uint64_t id, bool admin) {
    if (!id) return {nametag_white, ""};
    if (const auto mark = multiplayer::identity_mark(id)) {
        switch (*mark) {
        case multiplayer::IdentityList::developer: return {nametag_developer, "Dev"};
        case multiplayer::IdentityList::content_creator: return {nametag_creator, "Content Creator"};
        case multiplayer::IdentityList::centrix: return {nametag_centrix, "Centrix"};
        case multiplayer::IdentityList::staff: return {nametag_staff, "Staff"};
        default: return {nametag_homie, "Homie"};
        }
    }
    if (admin) return {nametag_admin, "Admin"};
    return {nametag_white, ""};
}

void show_notification(const std::string& message, double duration = 3.5, ImU32 accent = 0) {
    if (message.empty()) return;
    const double until = ImGui::GetTime() + duration;
    g_state.feedback_text = message;
    g_state.feedback_until = until;

    g_state.notifications.insert(g_state.notifications.begin(), {message, until, accent});
    if (g_state.notifications.size() > 5) {
        g_state.notifications.resize(5);
    }
}

// --- Section Header with Blue Accent Bar (1-1 with ReSkate in-game menu) ---
void section(const char* text) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const auto at = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const float avail_x = ImGui::GetContentRegionAvail().x;
    draw->AddRectFilled(at, ImVec2(at.x + avail_x, at.y + S(2)), tile_light);
    draw->AddRectFilled(at, ImVec2(at.x + S(34), at.y + S(3)), blue);
    ImGui::Dummy(ImVec2(0, S(8)));
}

// --- Card Container with 4px Blue Left Accent (1-1 with ReSkate in-game menu) ---
void begin_card(const char* id, const char* title, const char* subtitle = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tile);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const auto at = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + S(4), at.y + ImGui::GetWindowHeight()), blue);

    if (title) {
        ImGui::PushFont(g_fonts.bold);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (subtitle) {
            ImGui::SameLine(0, S(10));
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted(subtitle);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
    }
}

void end_card() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(8)));
}

// --- Stat / Metric Display Row ---
void stat_row(const char* label, const std::string& value, ImU32 value_color = white) {
    const float col = ImGui::GetCursorPosX() + S(220);
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(col);
    ImGui::PushStyleColor(ImGuiCol_Text, value_color);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

// --- Pill Status Badge ---
void badge(const char* text, ImU32 bg_color, ImU32 text_color = black) {
    const auto size = g_fonts.bold->CalcTextSizeA(S(12), FLT_MAX, 0, text);
    const ImVec2 pad(S(8), S(4));
    const auto at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetFrameHeight();
    const float top = at.y + std::max(0.0f, (line - size.y - pad.y * 2) * 0.5f);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, std::max(line, size.y + pad.y * 2)));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x + pad.x * 2, top + size.y + pad.y * 2), bg_color, S(3));
    draw->AddText(g_fonts.bold, S(12), ImVec2(at.x + pad.x, top + pad.y), text_color, text);
}

// --- Feature Chip for Capability Tags ---
void feature_chip(const char* text) {
    const auto size = g_fonts.caption->CalcTextSizeA(S(11), FLT_MAX, 0, text);
    const ImVec2 pad(S(8), S(3));
    const auto at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetFrameHeight();
    const float item_h = size.y + pad.y * 2;
    const float top = at.y + std::max(0.0f, (line - item_h) * 0.5f);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, std::max(line, item_h)));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + size.x + pad.x * 2, top + item_h),
                        IM_COL32(36, 40, 48, 255), S(3));
    draw->AddRect(ImVec2(at.x, top), ImVec2(at.x + size.x + pad.x * 2, top + item_h),
                  IM_COL32(56, 62, 74, 255), S(3));
    draw->AddText(g_fonts.caption, S(11), ImVec2(at.x + pad.x, top + pad.y), white, text);
}

// --- Toggle Row (1-1 with skate_menu.cpp toggle_row) ---
bool toggle_row(const char* label, const char* hint, bool& value) {
    ImGui::PushID(label);
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = S(36);
    const bool clicked = ImGui::Selectable("##toggle", false, ImGuiSelectableFlags_None, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) value = !value;

    auto* draw = ImGui::GetWindowDrawList();
    const unsigned seed = static_cast<unsigned>(ImGui::GetItemID());
    rough_rect(draw, at, ImVec2(at.x + width, at.y + height - S(3)),
               hovered ? tile_light : tile_grey, seed, g_scale);

    draw->AddText(g_fonts.bold, S(15), ImVec2(at.x + S(12), at.y + S(8)), white, label);

    const char* state_str = value ? "ON" : "OFF";
    const float block = S(58);
    const ImVec2 box(at.x + width - block - S(6), at.y + S(6));
    draw->AddRectFilled(box, ImVec2(box.x + block, at.y + height - S(9)), value ? blue : IM_COL32(0, 0, 0, 90));
    const auto text_size = g_fonts.bold->CalcTextSizeA(S(13), FLT_MAX, 0, state_str);
    draw->AddText(g_fonts.bold, S(13),
                  ImVec2(box.x + (block - text_size.x) * 0.5f, box.y + (height - S(15) - text_size.y) * 0.5f),
                  value ? black : muted, state_str);

    if (hovered && hint && *hint) ImGui::SetTooltip("%s", hint);
    ImGui::PopID();
    return clicked;
}

// --- Category Tabs Row (1-1 with category_tabs from skate_menu.cpp) ---
void category_tabs(int& selected, const std::vector<const char*>& tabs, const char* id) {
    selected = std::clamp(selected, 0, static_cast<int>(tabs.size()) - 1);
    ImGui::PushID(id);
    const float gap = S(6);
    const float tab_width = (ImGui::GetContentRegionAvail().x - gap * (tabs.size() - 1)) / tabs.size();
    const float height = S(34);
    auto* draw = ImGui::GetWindowDrawList();

    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_width, height)) && selected != i) {
            selected = i;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool on = selected == i;
        rough_rect(draw, at, ImVec2(at.x + tab_width, at.y + height),
                   on ? blue : hovered ? tile_light : tile_grey, static_cast<unsigned>(i + 17), g_scale);
        const char* label = tabs[i];
        const auto size = g_fonts.bold->CalcTextSizeA(S(14), FLT_MAX, 0, label);
        draw->AddText(g_fonts.bold, S(14),
                      ImVec2(at.x + std::max(S(6), (tab_width - size.x) * 0.5f), at.y + (height - size.y) * 0.5f),
                      on ? black : white, label);
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, S(8)));
}

// --- Rough Rect Primary Button ---
bool rough_button(const char* label, ImVec2 size = ImVec2(0, 0), bool active = false) {
    const float width = size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x;
    const float height = size.y > 0 ? size.y : S(34);
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    auto* draw = ImGui::GetWindowDrawList();
    const unsigned seed = static_cast<unsigned>(ImGui::GetItemID());
    rough_rect(draw, at, ImVec2(at.x + width, at.y + height),
               active ? blue : hovered ? tile_light : tile_grey, seed, g_scale);

    const auto text_size = g_fonts.bold->CalcTextSizeA(S(13), FLT_MAX, 0, label);
    draw->AddText(g_fonts.bold, S(13),
                  ImVec2(at.x + std::max(S(6), (width - text_size.x) * 0.5f), at.y + (height - text_size.y) * 0.5f),
                  active ? black : white, label);
    return clicked;
}

// --- High-Visibility Colored Action Button ---
bool colored_action_button(const char* label, ImVec2 size = ImVec2(0, 0), ImU32 bg_col = tile_grey, ImU32 text_col = white, const char* tooltip = nullptr) {
    const float width = size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x;
    const float height = size.y > 0 ? size.y : S(30);
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    }

    auto* draw = ImGui::GetWindowDrawList();
    ImU32 fill = bg_col;
    if (hovered) {
        const int r = std::min(255, (int)((bg_col & 0xFF) * 1.18f));
        const int g = std::min(255, (int)(((bg_col >> 8) & 0xFF) * 1.18f));
        const int b = std::min(255, (int)(((bg_col >> 16) & 0xFF) * 1.18f));
        const int a = (bg_col >> 24) & 0xFF;
        fill = IM_COL32(r, g, b, a);
    }
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), fill, S(4.0f));

    const auto text_size = g_fonts.bold->CalcTextSizeA(S(13), FLT_MAX, 0, label);
    draw->AddText(g_fonts.bold, S(13),
                  ImVec2(at.x + std::max(S(6), (width - text_size.x) * 0.5f), at.y + (height - text_size.y) * 0.5f),
                  text_col, label);
    return clicked;
}

// --- Server Lifecycle Status Pill ---
void draw_server_status_pill(ServerState state) {
    const float seg_h = S(30.0f);
    const float rounding = S(4.0f);
    ImU32 bg_col = danger;
    ImU32 text_col = white;
    const char* text = "OFFLINE";

    switch (state) {
    case ServerState::Running:
        bg_col = good;
        text_col = black;
        text = "ONLINE";
        break;
    case ServerState::Starting:
        bg_col = warning;
        text_col = black;
        text = "STARTING...";
        break;
    case ServerState::Stopping:
        bg_col = warning;
        text_col = black;
        text = "STOPPING...";
        break;
    case ServerState::Error:
        bg_col = danger;
        text_col = white;
        text = "SERVER ERROR";
        break;
    case ServerState::Stopped:
    default:
        bg_col = danger;
        text_col = white;
        text = "OFFLINE";
        break;
    }

    const auto sz_text = g_fonts.bold->CalcTextSizeA(S(12.0f), FLT_MAX, 0.0f, text);
    const float pad_x = S(14.0f);
    const float total_w = sz_text.x + pad_x * 2.0f;

    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();

    ImGui::PushID("pill_server_status");
    ImGui::InvisibleButton("##status_pill_btn", ImVec2(total_w, seg_h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Server Lifecycle State: %s", text);
    }
    ImGui::PopID();

    draw->AddRectFilled(origin, ImVec2(origin.x + total_w, origin.y + seg_h), bg_col, rounding);
    if (hovered) {
        draw->AddRectFilled(origin, ImVec2(origin.x + total_w, origin.y + seg_h), IM_COL32(255, 255, 255, 30), rounding);
    }
    const float text_x = origin.x + (total_w - sz_text.x) * 0.5f;
    const float text_y = origin.y + (seg_h - sz_text.y) * 0.5f;
    draw->AddText(g_fonts.bold, S(12.0f), ImVec2(text_x, text_y), text_col, text);
}

// --- Live Player Count Pill ---
void draw_player_count_pill(unsigned count, unsigned max_p, bool is_online) {
    const float seg_h = S(30.0f);
    const float rounding = S(4.0f);

    char count_str[32];
    std::snprintf(count_str, sizeof(count_str), "%u / %u", count, max_p);

    const auto sz_label = g_fonts.bold->CalcTextSizeA(S(11.0f), FLT_MAX, 0.0f, "PLAYERS");
    const auto sz_count = g_fonts.bold->CalcTextSizeA(S(13.0f), FLT_MAX, 0.0f, count_str);

    const float dot_r = S(3.5f);
    const float pad_x = S(10.0f);
    const float gap_dot_label = S(8.0f);
    const float gap_label_sep = S(10.0f);
    const float gap_sep_count = S(10.0f);

    const float total_w = pad_x + (dot_r * 2.0f) + gap_dot_label + sz_label.x + gap_label_sep + 1.0f + gap_sep_count + sz_count.x + pad_x;

    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();

    ImGui::PushID("pill_player_count");
    const bool clicked = ImGui::InvisibleButton("##player_pill_btn", ImVec2(total_w, seg_h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (is_online) {
            ImGui::SetTooltip("Connected Players: %u / %u\nClick to query active players in console", count, max_p);
        } else {
            ImGui::SetTooltip("Server Offline (Capacity: %u players)\nClick to check status", max_p);
        }
    }
    if (clicked) {
        if (is_online) ServerEngine::instance().queue_command("players");
        else ServerEngine::instance().log("Server is offline. No players connected.", LogLevel::warning);
    }
    ImGui::PopID();

    // 1. Background rectangle (clean flat, no border outline)
    draw->AddRectFilled(origin, ImVec2(origin.x + total_w, origin.y + seg_h), hovered ? tile_light : tile_grey, rounding);

    // 2. Status Dot
    const ImU32 dot_col = is_online ? good : muted;
    const ImVec2 dot_center(origin.x + pad_x + dot_r, origin.y + seg_h * 0.5f);
    draw->AddCircleFilled(dot_center, dot_r, dot_col);

    // 3. Label "PLAYERS"
    const float label_x = origin.x + pad_x + (dot_r * 2.0f) + gap_dot_label;
    const float label_y = origin.y + (seg_h - sz_label.y) * 0.5f;
    draw->AddText(g_fonts.bold, S(11.0f), ImVec2(label_x, label_y), muted, "PLAYERS");

    // 4. Subtle vertical divider line
    const float sep_x = label_x + sz_label.x + gap_label_sep;
    draw->AddLine(ImVec2(sep_x, origin.y + S(4.0f)), ImVec2(sep_x, origin.y + seg_h - S(4.0f)), IM_COL32(26, 26, 28, 255), 1.0f);

    // 5. Count string "0 / 249"
    const float count_x = sep_x + 1.0f + gap_sep_count;
    const float count_y = origin.y + (seg_h - sz_count.y) * 0.5f;
    const ImU32 count_col = (is_online && count > 0) ? good : white;
    draw->AddText(g_fonts.bold, S(13.0f), ImVec2(count_x, count_y), count_col, count_str);
}

// --- Connected Server Control Block [ ▶ | ↻ | ■ ] ---
void draw_server_control_block(const ServerStatusSnapshot& stats) {
    const float seg_w = S(38.0f);
    const float seg_h = S(30.0f);
    const float total_w = seg_w * 3.0f;
    const float rounding = S(4.0f);

    const bool is_offline = (stats.state == ServerState::Stopped || stats.state == ServerState::Error);
    const bool is_active = !is_offline;

    const bool start_enabled = is_offline;
    const bool restart_enabled = is_active;
    const bool stop_enabled = is_active;

    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();

    // 1. Base container background using the UI's pre-existing tile_grey
    draw->AddRectFilled(origin, ImVec2(origin.x + total_w, origin.y + seg_h), tile_grey, rounding);

    // Segment 0: Start (▶ Green)
    ImGui::PushID("ctrl_seg_start");
    const bool p0_clicked = ImGui::InvisibleButton("##start", ImVec2(seg_w, seg_h));
    const bool start_hovered = ImGui::IsItemHovered();
    if (start_hovered && start_enabled) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (start_hovered) {
        ImGui::SetTooltip("%s", start_enabled ? "Start Server" : "Server is already active");
    }
    if (p0_clicked && start_enabled) {
        ServerEngine::instance().start_server_async();
        show_notification("Starting dedicated server session...", 3.5, good);
    }
    ImGui::PopID();

    // Segment 1: Restart (↻ Yellow)
    ImGui::SameLine(0, 0);
    ImGui::PushID("ctrl_seg_restart");
    const bool p1_clicked = ImGui::InvisibleButton("##restart", ImVec2(seg_w, seg_h));
    const bool restart_hovered = ImGui::IsItemHovered();
    if (restart_hovered && restart_enabled) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (restart_hovered) {
        ImGui::SetTooltip("%s", restart_enabled ? "Restart Server" : "Server is offline");
    }
    if (p1_clicked && restart_enabled) {
        ServerEngine::instance().restart_server_async();
        show_notification("Restarting dedicated server...", 3.5, warning);
    }
    ImGui::PopID();

    // Segment 2: Stop (■ Red)
    ImGui::SameLine(0, 0);
    ImGui::PushID("ctrl_seg_stop");
    const bool p2_clicked = ImGui::InvisibleButton("##stop", ImVec2(seg_w, seg_h));
    const bool stop_hovered = ImGui::IsItemHovered();
    if (stop_hovered && stop_enabled) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (stop_hovered) {
        ImGui::SetTooltip("%s", stop_enabled ? "Stop Server" : "Server is offline");
    }
    if (p2_clicked && stop_enabled) {
        ServerEngine::instance().stop_server_async();
        show_notification("Stopping server session...", 3.5, danger);
    }
    ImGui::PopID();

    // 2. Render segment filled backgrounds
    // --- Segment 0 (Start) ---
    const ImVec2 p0_min = origin;
    const ImVec2 p0_max(origin.x + seg_w, origin.y + seg_h);
    ImU32 icon0_color = grey_text;
    if (start_enabled) {
        draw->AddRectFilled(p0_min, p0_max, IM_COL32(38, 195, 95, 255), rounding, ImDrawFlags_RoundCornersLeft);
        if (start_hovered) {
            draw->AddRectFilled(p0_min, p0_max, IM_COL32(255, 255, 255, 40), rounding, ImDrawFlags_RoundCornersLeft);
        }
        icon0_color = white;
    } else if (start_hovered) {
        draw->AddRectFilled(p0_min, p0_max, tile_light, rounding, ImDrawFlags_RoundCornersLeft);
    }

    // --- Segment 1 (Restart) ---
    const ImVec2 p1_min(origin.x + seg_w, origin.y);
    const ImVec2 p1_max(origin.x + seg_w * 2.0f, origin.y + seg_h);
    ImU32 icon1_color = grey_text;
    if (restart_enabled) {
        draw->AddRectFilled(p1_min, p1_max, IM_COL32(245, 190, 25, 255), 0.0f, ImDrawFlags_RoundCornersNone);
        if (restart_hovered) {
            draw->AddRectFilled(p1_min, p1_max, IM_COL32(255, 255, 255, 45), 0.0f, ImDrawFlags_RoundCornersNone);
        }
        icon1_color = IM_COL32(20, 20, 20, 255);
    } else if (restart_hovered) {
        draw->AddRectFilled(p1_min, p1_max, tile_light, 0.0f, ImDrawFlags_RoundCornersNone);
    }

    // --- Segment 2 (Stop) ---
    const ImVec2 p2_min(origin.x + seg_w * 2.0f, origin.y);
    const ImVec2 p2_max(origin.x + total_w, origin.y + seg_h);
    ImU32 icon2_color = grey_text;
    if (stop_enabled) {
        draw->AddRectFilled(p2_min, p2_max, IM_COL32(230, 55, 55, 255), rounding, ImDrawFlags_RoundCornersRight);
        if (stop_hovered) {
            draw->AddRectFilled(p2_min, p2_max, IM_COL32(255, 255, 255, 40), rounding, ImDrawFlags_RoundCornersRight);
        }
        icon2_color = white;
    } else if (stop_hovered) {
        draw->AddRectFilled(p2_min, p2_max, tile_light, rounding, ImDrawFlags_RoundCornersRight);
    }

    // 3. Render vector icons
    // Start Icon (Triangle ▶)
    const ImVec2 c0(origin.x + seg_w * 0.5f, origin.y + seg_h * 0.5f);
    const float r0 = S(6.0f);
    draw->AddTriangleFilled(
        ImVec2(c0.x - r0 * 0.65f, c0.y - r0 * 0.95f),
        ImVec2(c0.x + r0 * 0.95f, c0.y),
        ImVec2(c0.x - r0 * 0.65f, c0.y + r0 * 0.95f),
        icon0_color
    );

    // Restart Icon (Circular Arrow ↻)
    const ImVec2 c1(origin.x + seg_w * 1.5f, origin.y + seg_h * 0.5f);
    const float r1 = S(6.0f);
    draw->PathArcTo(c1, r1, -2.6f, 1.8f, 16);
    draw->PathStroke(icon1_color, 0, S(2.2f));
    const float end_angle = 1.8f;
    const ImVec2 tip(c1.x + r1 * std::cos(end_angle), c1.y + r1 * std::sin(end_angle));
    draw->AddTriangleFilled(
        ImVec2(tip.x - S(2.5f), tip.y - S(4.5f)),
        tip,
        ImVec2(tip.x + S(4.5f), tip.y - S(1.5f)),
        icon1_color
    );

    // Stop Icon (Square ■)
    const ImVec2 c2(origin.x + seg_w * 2.5f, origin.y + seg_h * 0.5f);
    const float s2 = S(5.5f);
    draw->AddRectFilled(
        ImVec2(c2.x - s2, c2.y - s2),
        ImVec2(c2.x + s2, c2.y + s2),
        icon2_color,
        S(1.5f)
    );

    // 4. Clean vertical line separators splitting the 3 segments
    const ImU32 sep_color = IM_COL32(26, 26, 28, 255);
    draw->AddLine(ImVec2(origin.x + seg_w, origin.y), ImVec2(origin.x + seg_w, origin.y + seg_h), sep_color, 1.0f);
    draw->AddLine(ImVec2(origin.x + seg_w * 2.0f, origin.y), ImVec2(origin.x + seg_w * 2.0f, origin.y + seg_h), sep_color, 1.0f);
}

// --- Fonts Loader ---
ImFont* embedded_font(const wchar_t* name, float size) {
    static const ImWchar ranges[]{0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2190, 0x21FF, 0};
    const auto instance = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(instance, name, MAKEINTRESOURCEW(10));
    if (!resource) return nullptr;
    const auto loaded = LoadResource(instance, resource);
    void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = size >= 32 ? 1 : 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(SizeofResource(instance, resource)),
                                                      S(size), &config, ranges);
}

ImFont* file_font(const fs::path& path, float size) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), S(size));
}

void load_fonts(const fs::path& root_folder) {
    auto& io = ImGui::GetIO();
    io.Fonts->TexGlyphPadding = 2;

    const auto font_dir = root_folder / "source" / "External" / "fonts";
    const auto ext_dir = root_folder / "External" / "fonts";

    const auto load = [&](const wchar_t* res_name, const char* file_name, const char* fallback_sys, float size) -> ImFont* {
        if (auto* f = embedded_font(res_name, size)) return f;
        if (auto* f = file_font(font_dir / file_name, size)) return f;
        if (auto* f = file_font(ext_dir / file_name, size)) return f;
        std::array<char, MAX_PATH> win_dir{};
        if (GetWindowsDirectoryA(win_dir.data(), static_cast<UINT>(win_dir.size()))) {
            const auto win_font = fs::path(win_dir.data()) / "Fonts" / fallback_sys;
            if (auto* f = file_font(win_font, size)) return f;
        }
        ImFontConfig cfg;
        cfg.SizePixels = S(size);
        return io.Fonts->AddFontDefault(&cfg);
    };

    io.Fonts->AddFontDefault();
    g_fonts.body = load(L"FONT_BODY", "Montserrat-SemiBold.ttf", "segoeui.ttf", 15.0f);
    g_fonts.caption = load(L"FONT_BODY", "Montserrat-SemiBold.ttf", "segoeui.ttf", 12.0f);
    g_fonts.bold = load(L"FONT_HEADING", "Montserrat-ExtraBold.ttf", "segoeuib.ttf", 16.0f);
    g_fonts.heading = load(L"FONT_HEADING", "Montserrat-ExtraBold.ttf", "segoeuib.ttf", 22.0f);
    g_fonts.title = load(L"FONT_BRUSH", "PermanentMarker-Regular.ttf", "arialbi.ttf", 42.0f);

    std::array<char, MAX_PATH> win_dir{};
    if (GetWindowsDirectoryA(win_dir.data(), static_cast<UINT>(win_dir.size()))) {
        g_fonts.mono = file_font(fs::path(win_dir.data()) / "Fonts" / "consola.ttf", 14.0f);
    }
    if (!g_fonts.mono) g_fonts.mono = g_fonts.body;

    io.FontDefault = g_fonts.body;
    io.Fonts->Build();
}

void apply_style() {
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 0;
    style.GrabRounding = 0;
    style.PopupRounding = 0;
    style.ScrollbarRounding = 0;
    style.WindowBorderSize = 0;
    style.FrameBorderSize = 0;

    style.WindowPadding = ImVec2(S(24), S(20));
    style.FramePadding = ImVec2(S(10), S(7));
    style.ItemSpacing = ImVec2(S(10), S(8));
    style.ScrollbarSize = S(10);

    auto* c = style.Colors;
    const auto rgb = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };

    c[ImGuiCol_Text] = rgb(white);
    c[ImGuiCol_TextDisabled] = rgb(muted);
    c[ImGuiCol_WindowBg] = rgb(IM_COL32(14, 15, 18, 255));
    c[ImGuiCol_ChildBg] = rgb(tile);
    c[ImGuiCol_PopupBg] = rgb(tile);
    c[ImGuiCol_Border] = rgb(tile_light);
    c[ImGuiCol_FrameBg] = rgb(tile_grey);
    c[ImGuiCol_FrameBgHovered] = rgb(tile_light);
    c[ImGuiCol_FrameBgActive] = rgb(blue_active);
    c[ImGuiCol_TitleBg] = rgb(IM_COL32(10, 10, 11, 255));
    c[ImGuiCol_TitleBgActive] = rgb(IM_COL32(10, 10, 11, 255));
    c[ImGuiCol_Button] = rgb(tile_grey);
    c[ImGuiCol_ButtonHovered] = rgb(tile_light);
    c[ImGuiCol_ButtonActive] = rgb(blue_active);
    c[ImGuiCol_Header] = rgb(tile_grey);
    c[ImGuiCol_HeaderHovered] = rgb(tile_light);
    c[ImGuiCol_HeaderActive] = rgb(blue);
    c[ImGuiCol_CheckMark] = rgb(blue);
    c[ImGuiCol_SliderGrab] = rgb(blue);
    c[ImGuiCol_SliderGrabActive] = rgb(blue_hover);
    c[ImGuiCol_ScrollbarBg] = rgb(IM_COL32(16, 17, 20, 255));
    c[ImGuiCol_ScrollbarGrab] = rgb(tile_light);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(blue);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(blue_hover);
    c[ImGuiCol_Separator] = rgb(tile_light);
    c[ImGuiCol_TextSelectedBg] = rgb(IM_COL32(1, 131, 255, 115));
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;

    switch (message) {
    case WM_NCCALCSIZE: {
        if (wparam == TRUE) {
            // Fill entire window rectangle with client area, eliminating default titlebar/borders
            return 0;
        }
        break;
    }
    case WM_NCHITTEST: {
        POINT pt{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(window, &pt);
        RECT rc;
        GetClientRect(window, &rc);

        const bool maximized = IsZoomed(window);

        // Edge and corner resize hit testing when not maximized
        if (!maximized) {
            const int border = static_cast<int>(S(8.0f));

            if (pt.y < border && pt.x < border) return HTTOPLEFT;
            if (pt.y < border && pt.x >= rc.right - border) return HTTOPRIGHT;
            if (pt.y >= rc.bottom - border && pt.x < border) return HTBOTTOMLEFT;
            if (pt.y >= rc.bottom - border && pt.x >= rc.right - border) return HTBOTTOMRIGHT;

            if (pt.y < border) return HTTOP;
            if (pt.y >= rc.bottom - border) return HTBOTTOM;
            if (pt.x < border) return HTLEFT;
            if (pt.x >= rc.right - border) return HTRIGHT;
        }

        // Drag window from top bar (leaving room on the right for window controls)
        if (g_drag_allowed && pt.y >= 0 && pt.y < S(48) && pt.x < (rc.right - S(112.0f))) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = static_cast<LONG>(S(850.0f));
        mmi->ptMinTrackSize.y = static_cast<LONG>(S(550.0f));

        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (monitor) {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(monitor, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
        }
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_renderer.resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// ============================================================================
// TAB RENDERERS
// ============================================================================

void draw_overview_tab(const ServerStatusSnapshot& stats) {
    section("NETWORK & SESSION OVERVIEW");

    ImGui::Columns(2, "overview_cols", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.50f);

    begin_card("card_session", "SESSION IDENTITY", "Host configuration and live status");
    stat_row("Server Status", stats.state_text,
             stats.state == ServerState::Running ? good : (stats.state == ServerState::Error ? danger : warning));
    stat_row("Server Name", stats.server_name);
    stat_row("Current Map", stats.map_name);

    char buf[128];
    if (stats.state == ServerState::Running) {
        stat_row("Connected Players", std::to_string(stats.player_count) + " / " + std::to_string(stats.max_players));
        std::snprintf(buf, sizeof(buf), "%u TPS / %.1f TPS", stats.target_tps, stats.actual_tps);
        stat_row("Target / Measured TPS", buf);
        std::snprintf(buf, sizeof(buf), "%.2f ms", stats.frame_time_ms);
        stat_row("Frame Latency", buf);

        const auto hours = stats.uptime_seconds / 3600;
        const auto mins = (stats.uptime_seconds % 3600) / 60;
        const auto secs = stats.uptime_seconds % 60;
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", hours, mins, secs);
        stat_row("Server Uptime", buf);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted("Session Join Code");
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetCursorPosX() + S(220));
        ImGui::PushFont(g_fonts.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, blue);
        ImGui::TextUnformatted(stats.invite_code.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine(0, S(8));
        const bool just_copied = ImGui::GetTime() < g_state.copied_until;
        if (rough_button(just_copied ? "COPIED" : "COPY", ImVec2(S(60), S(20)))) {
            ImGui::SetClipboardText(stats.invite_code.c_str());
            g_state.copied_until = ImGui::GetTime() + 3.0;
        }
    } else {
        stat_row("Player Capacity", "0 / " + std::to_string(stats.max_players) + " (Offline)");
        stat_row("Target Engine TPS", std::to_string(stats.target_tps) + " TPS (Dedicated Standard)");
        stat_row("Session Join Code", "None (Server Stopped)", muted);
    }
    end_card();

    begin_card("card_network", "STEAM TRANSPORT & PORTS", "Valve Steam SDR Networking");
    stat_row("Public IP Address", stats.state == ServerState::Running
             ? (stats.public_ip.empty() ? "Detecting..." : stats.public_ip)
             : "Offline (Unbound)");
    stat_row("Game Port (UDP)", std::to_string(stats.port));
    stat_row("Query Port (UDP)", std::to_string(stats.query_port));
    stat_row("Host SteamID64", stats.state == ServerState::Running
             ? std::to_string(stats.steam_id)
             : (stats.has_steam_token ? "Configured in GSLT Token" : "Anonymous (Generated on Boot)"));
    stat_row("Steam GSLT Token", stats.has_steam_token ? "CONFIGURED (Persistent ID)" : "NONE (Anonymous Session)",
             stats.has_steam_token ? good : (stats.is_listed ? warning : muted));
    stat_row("Password Protected", stats.has_password ? "YES" : "NO", stats.has_password ? warning : good);
    stat_row("Listed in Browser", stats.is_listed ? "YES" : "NO", stats.is_listed ? good : muted);
    end_card();

    ImGui::NextColumn();

    begin_card("card_audio", "AUDIO & VOICE CHAT", "Voice and communication rules");
    stat_row("Proximity Voice Chat", stats.voice_allowed ? "ENABLED" : "DISABLED", stats.voice_allowed ? good : danger);
    std::snprintf(buf, sizeof(buf), "%.0f meters", stats.voice_range);
    stat_row("Voice Broadcast Range", buf);
    stat_row("Word Blacklist Automod", "ACTIVE (~425 terms)", good);
    stat_row("Chat Rate Limiter", "ACTIVE (Burst protection)", good);
    end_card();

    begin_card("card_gameplay", "GAMEPLAY & SECURITY RULES", "Authoritative verification rules");
    const auto& engine_cfg = ServerEngine::instance().config();
    stat_row("Speed Anti-Cheat", engine_cfg.speed_check == "kick" ? "ENFORCED (KICK)" : (engine_cfg.speed_check == "warn" ? "ACTIVE (WARN)" : "DISABLED"), engine_cfg.speed_check != "off" ? good : muted);
    stat_row("Scoring Anti-Cheat", engine_cfg.score_check == "kick" ? "ENFORCED (KICK)" : (engine_cfg.score_check == "warn" ? "ACTIVE (WARN)" : "DISABLED"), engine_cfg.score_check != "off" ? good : muted);
    char bone_buf[32];
    std::snprintf(bone_buf, sizeof(bone_buf), "%.1fx factor", stats.bone_scale_limit);
    stat_row("Bone Scale Limit", bone_buf, good);
    stat_row("Physics Tuning", engine_cfg.enforce_tuning ? "ENFORCED (Standard)" : "UNRESTRICTED", engine_cfg.enforce_tuning ? good : warning);
    stat_row("Noclip & Boosts", engine_cfg.noclip ? "ALLOWED" : "RESTRICTED", engine_cfg.noclip ? good : warning);
    stat_row("Player Parties", engine_cfg.parties ? ("ENABLED (Up to " + std::to_string(engine_cfg.party_size) + " players)") : "DISABLED", engine_cfg.parties ? good : muted);
    end_card();

    ImGui::Columns(1);
}

void draw_players_tab(const std::vector<Host::PlayerInfo>& players) {
    const std::string players_title = "CONNECTED PLAYERS (" + std::to_string(players.size()) + " ONLINE)";
    section(players_title.c_str());

    auto* draw = ImGui::GetWindowDrawList();

    // Search bar & summary ribbon
    const float search_w = S(280.0f);
    const float search_h = S(32.0f);
    const float pad_y = std::max(0.0f, (search_h - ImGui::GetFontSize()) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), pad_y));
    ImGui::SetNextItemWidth(search_w);
    ImGui::InputTextWithHint("##player_filter", "Search by name or SteamID64...", g_state.player_search, sizeof(g_state.player_search));
    ImGui::PopStyleVar();

    const ImVec2 search_min = ImGui::GetItemRectMin();
    const ImVec2 search_max = ImGui::GetItemRectMax();
    const float actual_h = search_max.y - search_min.y;

    if (g_state.player_search[0]) {
        ImGui::SameLine(0, S(8));
        if (rough_button("CLEAR", ImVec2(S(70), actual_h))) {
            g_state.player_search[0] = '\0';
        }
    }

    ImGui::SameLine(0, S(16));
    const auto stats = ServerEngine::instance().status_snapshot();
    const bool is_online = (stats.state == ServerState::Running);
    const unsigned cur_players = static_cast<unsigned>(players.size());
    const unsigned server_cap = stats.max_players;

    char count_str[32];
    std::snprintf(count_str, sizeof(count_str), "%u / %u", cur_players, server_cap);

    const auto sz_label = g_fonts.bold->CalcTextSizeA(S(11.0f), FLT_MAX, 0.0f, "PLAYERS");
    const auto sz_count = g_fonts.bold->CalcTextSizeA(S(13.0f), FLT_MAX, 0.0f, count_str);

    const float dot_r = S(3.5f);
    const float pad_x = S(10.0f);
    const float gap_dot_label = S(8.0f);
    const float gap_label_sep = S(10.0f);
    const float gap_sep_count = S(10.0f);

    const float count_badge_w = pad_x + (dot_r * 2.0f) + gap_dot_label + sz_label.x + gap_label_sep + 1.0f + gap_sep_count + sz_count.x + pad_x;
    const ImVec2 badge_origin(ImGui::GetCursorScreenPos().x, search_min.y);

    ImGui::PushID("players_tab_count_badge");
    ImGui::InvisibleButton("##players_tab_badge_btn", ImVec2(count_badge_w, actual_h));
    const bool badge_hovered = ImGui::IsItemHovered();
    if (badge_hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Connected Players: %u / %u\nMax Capacity: %u", cur_players, server_cap, server_cap);
    }
    ImGui::PopID();

    // 1. Background rectangle
    draw->AddRectFilled(badge_origin, ImVec2(badge_origin.x + count_badge_w, badge_origin.y + actual_h), badge_hovered ? tile_light : tile_grey, S(4.0f));

    // 2. Status Dot
    const ImU32 dot_col = is_online ? good : muted;
    const ImVec2 dot_center(badge_origin.x + pad_x + dot_r, badge_origin.y + actual_h * 0.5f);
    draw->AddCircleFilled(dot_center, dot_r, dot_col);

    // 3. Label "PLAYERS"
    const float label_x = badge_origin.x + pad_x + (dot_r * 2.0f) + gap_dot_label;
    const float label_y = badge_origin.y + (actual_h - sz_label.y) * 0.5f;
    draw->AddText(g_fonts.bold, S(11.0f), ImVec2(label_x, label_y), muted, "PLAYERS");

    // 4. Subtle vertical divider line
    const float sep_x = label_x + sz_label.x + gap_label_sep;
    draw->AddLine(ImVec2(sep_x, badge_origin.y + S(5.0f)), ImVec2(sep_x, badge_origin.y + actual_h - S(5.0f)), IM_COL32(26, 26, 28, 255), 1.0f);

    // 5. Count string "1 / 249"
    const float count_x = sep_x + 1.0f + gap_sep_count;
    const float count_y = badge_origin.y + (actual_h - sz_count.y) * 0.5f;
    const ImU32 count_col = (is_online && cur_players > 0) ? good : white;
    draw->AddText(g_fonts.bold, S(13.0f), ImVec2(count_x, count_y), count_col, count_str);

    ImGui::SetCursorScreenPos(ImVec2(badge_origin.x + count_badge_w, badge_origin.y + actual_h));

    ImGui::Dummy(ImVec2(0, S(10)));

    if (players.empty()) {
        begin_card("card_no_players", "NO PLAYERS CONNECTED", "Session is ready for incoming client connections");
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted("No players are currently connected to the dedicated server.");
        ImGui::TextUnformatted("Share your Join Code or host IP in the community server browser to invite players!");
        ImGui::PopStyleColor();
        end_card();
        return;
    }

    std::string search_query = g_state.player_search;
    std::transform(search_query.begin(), search_query.end(), search_query.begin(), ::tolower);

    int visible_count = 0;

    for (size_t i = 0; i < players.size(); ++i) {
        const auto& p = players[i];

        // Filter matching
        if (!search_query.empty()) {
            std::string name_lower = p.name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::string id_str = std::to_string(p.id);
            if (name_lower.find(search_query) == std::string::npos && id_str.find(search_query) == std::string::npos) {
                continue;
            }
        }
        visible_count++;

        ImGui::PushID(static_cast<int>(p.id));

        const auto card_pos = ImGui::GetCursorScreenPos();
        const float card_w = ImGui::GetContentRegionAvail().x;
        const float card_h = S(62.0f);

        const bool is_hovered = ImGui::IsMouseHoveringRect(card_pos, ImVec2(card_pos.x + card_w, card_pos.y + card_h));

        // 1. Draw card background tile
        draw->AddRectFilled(card_pos, ImVec2(card_pos.x + card_w, card_pos.y + card_h),
                            is_hovered ? tile_light : tile_grey, S(4.0f));

        // 2. Role Accent & Left 4px Stripe (1-1 in-game role color & animated gradient)
        const auto [role_col, role_tag] = player_role(p.id, p.admin);
        const bool has_badge = !role_tag.empty();
        const double cur_time = ImGui::GetTime();
        const ImU32 stripe_col = has_badge ? animated_nametag_colour(role_col, cur_time) : IM_COL32(58, 195, 95, 255);

        draw->AddRectFilled(card_pos, ImVec2(card_pos.x + S(4.0f), card_pos.y + card_h), stripe_col, S(4.0f), ImDrawFlags_RoundCornersLeft);

        // 3. Right Action Buttons [ KICK | BAN ] with bold colors
        const float btn_w = S(84.0f);
        const float btn_h = S(32.0f);
        const float btn_gap = S(8.0f);
        const float total_btns_w = (btn_w * 2.0f) + btn_gap;

        const float btns_start_x = card_pos.x + card_w - total_btns_w - S(16.0f);
        const float btns_y = card_pos.y + (card_h - btn_h) * 0.5f;

        // Button 1: Kick (Vivid Amber/Warning)
        const ImVec2 kick_pos(btns_start_x, btns_y);
        ImGui::SetCursorScreenPos(kick_pos);
        const bool kick_clicked = ImGui::InvisibleButton("##btn_kick", ImVec2(btn_w, btn_h));
        const bool kick_hovered = ImGui::IsItemHovered();
        if (kick_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Kick %s from the server session", p.name.c_str());
        }
        const ImU32 kick_bg = kick_hovered ? IM_COL32(255, 185, 45, 255) : IM_COL32(235, 155, 30, 255);
        draw->AddRectFilled(kick_pos, ImVec2(kick_pos.x + btn_w, kick_pos.y + btn_h), kick_bg, S(4.0f));
        const auto sz_kick = g_fonts.bold->CalcTextSizeA(S(13.0f), FLT_MAX, 0.0f, "KICK");
        draw->AddText(g_fonts.bold, S(13.0f),
                      ImVec2(kick_pos.x + (btn_w - sz_kick.x) * 0.5f, kick_pos.y + (btn_h - sz_kick.y) * 0.5f),
                      black, "KICK");

        if (kick_clicked) {
            g_state.target_player_id = p.id;
            g_state.target_player_name = p.name;
            g_state.is_kick_modal = true;
            g_state.show_player_modal = true;
            g_state.target_action_reason[0] = '\0';
        }

        // Button 2: Ban (Vivid Crimson/Danger)
        const ImVec2 ban_pos(btns_start_x + btn_w + btn_gap, btns_y);
        ImGui::SetCursorScreenPos(ban_pos);
        const bool ban_clicked = ImGui::InvisibleButton("##btn_ban", ImVec2(btn_w, btn_h));
        const bool ban_hovered = ImGui::IsItemHovered();
        if (ban_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Ban %s from the server and add to bans list", p.name.c_str());
        }
        const ImU32 ban_bg = ban_hovered ? IM_COL32(250, 70, 70, 255) : IM_COL32(225, 45, 45, 255);
        draw->AddRectFilled(ban_pos, ImVec2(ban_pos.x + btn_w, ban_pos.y + btn_h), ban_bg, S(4.0f));
        const auto sz_ban = g_fonts.bold->CalcTextSizeA(S(13.0f), FLT_MAX, 0.0f, "BAN");
        draw->AddText(g_fonts.bold, S(13.0f),
                      ImVec2(ban_pos.x + (btn_w - sz_ban.x) * 0.5f, ban_pos.y + (btn_h - sz_ban.y) * 0.5f),
                      white, "BAN");

        if (ban_clicked) {
            g_state.target_player_id = p.id;
            g_state.target_player_name = p.name;
            g_state.is_kick_modal = false;
            g_state.show_player_modal = true;
            g_state.target_action_reason[0] = '\0';
        }

        // 4. Left Content: Clean, Centered Two-Row Layout
        const float row1_center_y = card_pos.y + S(19.0f);
        const float row2_y = card_pos.y + S(44.0f);
        float cur_x = card_pos.x + S(18.0f);

        // Row 1: Authentic 1-1 In-Game Nametag Layout [ Role Badge ] [ Player Name (Gradient Shimmer) ] [ Ping Pill ] [ Mute Status ]
        const float tag_font_size = S(15.0f);
        const auto name_sz = g_fonts.bold->CalcTextSizeA(tag_font_size, FLT_MAX, 0.0f, p.name.c_str());

        // Role badge sizing & vertical bounds
        const float badge_font_size = tag_font_size * 0.78f;
        const auto badge_extent = g_fonts.bold->CalcTextSizeA(badge_font_size, FLT_MAX, 0.0f, role_tag.c_str());
        const float badge_h = std::round(badge_extent.y + tag_font_size * 0.22f);
        const float badge_y = std::round(row1_center_y - badge_h * 0.5f);
        const float badge_radius = tag_font_size * 0.22f;

        // 1:1 In-Game Role Badge (Dev, Staff, Centrix, Content Creator, Homie, Admin) - Drawn BEFORE the name!
        if (has_badge) {
            const float badge_w = role_badge_width(g_fonts.bold, tag_font_size, role_tag);
            draw_role_badge(draw, g_fonts.bold, tag_font_size, ImVec2(cur_x, badge_y), badge_h, role_tag, role_col, 1.0f);
            cur_x += badge_w + S(8.0f);
        }

        // 1:1 In-Game Name Text with drop shadow and animated gradient shimmer (perfectly baseline-aligned)
        const float name_y = std::round(badge_y + (badge_h - badge_extent.y) * 0.5f + (badge_font_size - tag_font_size) * 0.76f);
        const ImVec2 name_at(cur_x, name_y);
        draw->AddText(g_fonts.bold, tag_font_size, ImVec2(name_at.x + 1.0f, name_at.y + 1.0f), IM_COL32(0, 0, 0, 190), p.name.c_str());
        const int name_vert = draw->VtxBuffer.Size;
        draw->AddText(g_fonts.bold, tag_font_size, name_at, has_badge ? role_col : white, p.name.c_str());
        shade_nametag_gradient(draw, name_vert, name_at.x, name_sz.x, role_col, cur_time);
        cur_x += name_sz.x + S(12.0f);

        // Ping Badge Pill (matching role badge height, radius, and vertical alignment)
        char ping_str[32];
        std::snprintf(ping_str, sizeof(ping_str), "%u ms", p.ping);
        const ImU32 ping_col = p.ping < 60 ? good : (p.ping < 130 ? warning : danger);
        const float ping_font_size = S(11.0f);
        const auto ping_sz = g_fonts.bold->CalcTextSizeA(ping_font_size, FLT_MAX, 0.0f, ping_str);
        const float ping_w = ping_sz.x + S(14.0f);
        draw->AddRectFilled(ImVec2(cur_x, badge_y), ImVec2(cur_x + ping_w, badge_y + badge_h), ping_col, badge_radius);
        draw->AddText(g_fonts.bold, ping_font_size, ImVec2(cur_x + (ping_w - ping_sz.x) * 0.5f, badge_y + (badge_h - ping_sz.y) * 0.5f), black, ping_str);
        cur_x += ping_w + S(8.0f);

        // Moderation badges if muted
        if (p.voice_muted) {
            const auto m_sz = g_fonts.bold->CalcTextSizeA(S(10.0f), FLT_MAX, 0.0f, "VOICE MUTED");
            const float m_w = m_sz.x + S(12.0f);
            draw->AddRectFilled(ImVec2(cur_x, badge_y), ImVec2(cur_x + m_w, badge_y + badge_h), warning, badge_radius);
            draw->AddText(g_fonts.bold, S(10.0f), ImVec2(cur_x + (m_w - m_sz.x) * 0.5f, badge_y + (badge_h - m_sz.y) * 0.5f), black, "VOICE MUTED");
            cur_x += m_w + S(6.0f);
        }
        if (p.text_muted) {
            const auto m_sz = g_fonts.bold->CalcTextSizeA(S(10.0f), FLT_MAX, 0.0f, "CHAT MUTED");
            const float m_w = m_sz.x + S(12.0f);
            draw->AddRectFilled(ImVec2(cur_x, badge_y), ImVec2(cur_x + m_w, badge_y + badge_h), warning, badge_radius);
            draw->AddText(g_fonts.bold, S(10.0f), ImVec2(cur_x + (m_w - m_sz.x) * 0.5f, badge_y + (badge_h - m_sz.y) * 0.5f), black, "CHAT MUTED");
            cur_x += m_w + S(6.0f);
        }

        // Row 2: SteamID64 (Clickable to Copy) + Position Coordinates
        cur_x = card_pos.x + S(18.0f);

        std::string steam_str = std::to_string(p.id);
        std::string steam_lbl = "SteamID: " + steam_str;
        const auto steam_sz = g_fonts.mono->CalcTextSizeA(S(12.0f), FLT_MAX, 0.0f, steam_lbl.c_str());

        ImGui::SetCursorScreenPos(ImVec2(cur_x, row2_y - steam_sz.y * 0.5f));
        ImGui::PushID("steam_copy_hitbox");
        const bool steam_clicked = ImGui::InvisibleButton("##steam_copy", steam_sz);
        const bool steam_hovered = ImGui::IsItemHovered();
        if (steam_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("SteamID64: %s (Click to copy to clipboard)", steam_str.c_str());
        }
        if (steam_clicked) {
            ImGui::SetClipboardText(steam_str.c_str());
            show_notification("Copied SteamID: " + steam_str, 2.5, blue);
        }
        ImGui::PopID();

        draw->AddText(g_fonts.mono, S(12.0f), ImVec2(cur_x, row2_y - steam_sz.y * 0.5f),
                      steam_hovered ? white : muted, steam_lbl.c_str());
        cur_x += steam_sz.x + S(24.0f);

        char pos_str[64];
        std::snprintf(pos_str, sizeof(pos_str), "Pos: (%.0f, %.0f, %.0f)", p.x, p.y, p.z);
        char pos_copy_str[64];
        std::snprintf(pos_copy_str, sizeof(pos_copy_str), "%.2f, %.2f, %.2f", p.x, p.y, p.z);
        const auto pos_sz = g_fonts.mono->CalcTextSizeA(S(12.0f), FLT_MAX, 0.0f, pos_str);

        ImGui::SetCursorScreenPos(ImVec2(cur_x, row2_y - pos_sz.y * 0.5f));
        ImGui::PushID("pos_copy_hitbox");
        const bool pos_clicked = ImGui::InvisibleButton("##pos_copy", pos_sz);
        const bool pos_hovered = ImGui::IsItemHovered();
        if (pos_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Coordinates: %s (Click to copy to clipboard)", pos_copy_str);
        }
        if (pos_clicked) {
            ImGui::SetClipboardText(pos_copy_str);
            show_notification("Copied Position: " + std::string(pos_copy_str), 2.5, blue);
        }
        ImGui::PopID();

        draw->AddText(g_fonts.mono, S(12.0f), ImVec2(cur_x, row2_y - pos_sz.y * 0.5f),
                      pos_hovered ? white : muted, pos_str);

        // Advance layout cursor past this card
        ImGui::SetCursorScreenPos(ImVec2(card_pos.x, card_pos.y + card_h + S(8.0f)));
        ImGui::PopID();
    }

    if (visible_count == 0 && !search_query.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::Text("No connected players match \"%s\".", g_state.player_search);
        ImGui::PopStyleColor();
    }
}

void draw_world_tab(const ServerStatusSnapshot& stats) {
    section("MAP & DESTINATION SWITCHER");

    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::Text("Current Active Map: %s", stats.map_name.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const auto& map_list = levels();
    if (!map_list.empty()) {
        for (const auto& level : map_list) {
            const bool active = (stats.map_name == level.name || map_setting(stats.map_name) == map_setting(level.name));
            if (rough_button(level.name.c_str(), ImVec2(S(200), S(34)), active)) {
                ServerEngine::instance().queue_command("map " + level.name);
                show_notification("Loading map: " + level.name, 3.0, good);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    } else {
        const std::vector<std::pair<const char*, const char*>> fallback_maps = {
            {"San Vansterdam", "San Vansterdam"},
            {"Isle of Grom", "Isle of Grom"},
            {"Super Ultra Mega Resort", "Super Ultra Mega Resort"}
        };
        for (const auto& [label, dest] : fallback_maps) {
            const bool active = (stats.map_name == dest);
            if (rough_button(label, ImVec2(S(200), S(34)), active)) {
                ServerEngine::instance().queue_command(std::string("map ") + dest);
                show_notification(std::string("Loading map: ") + label, 3.0, good);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    ImGui::Spacing();
    ImGui::PushItemWidth(S(320));
    ImGui::InputTextWithHint("##custom_map", "Enter custom map path / mod name...", g_state.custom_map, sizeof(g_state.custom_map));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (rough_button("LOAD MAP", ImVec2(S(120), S(32)))) {
        if (g_state.custom_map[0]) {
            ServerEngine::instance().queue_command(std::string("map ") + g_state.custom_map);
            show_notification(std::string("Switching to map: ") + g_state.custom_map, 3.0, blue);
            g_state.custom_map[0] = '\0';
        }
    }

    section("TIME OF DAY PRESETS");
    const std::vector<std::pair<const char*, const char*>> times = {
        {"Default", "default"},
        {"Morning (08:00)", "morning"},
        {"Noon (12:00)", "noon"},
        {"Afternoon (15:00)", "afternoon"},
        {"Evening (17:30)", "evening"},
        {"Night (00:00)", "night"},
        {"Weather Day", "weatherday"},
        {"Weather Night", "weathernight"}
    };

    for (const auto& [label, preset] : times) {
        if (rough_button(label, ImVec2(S(135), S(32)))) {
            ServerEngine::instance().queue_command(std::string("tod ") + preset);
            show_notification(std::string("Time of Day: ") + label, 2.5, blue);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    section("GAMEPLAY & PHYSICS TOGGLES");
    auto& cfg = ServerEngine::instance().config();
    bool nobail_val = cfg.no_bail;
    bool boosts_val = cfg.boosts;
    bool noclip_val = cfg.noclip;
    bool placement_val = (cfg.object_placement != ObjectPlacement::nobody);
    bool layer_sync_val = cfg.world_layer_sync;
    bool tuning_val = cfg.enforce_tuning;

    if (toggle_row("NoBail Mode", "Allows players to continue skating without ragdolling on impacts", nobail_val)) {
        cfg.no_bail = nobail_val;
        ServerEngine::instance().queue_command(std::string("nobail ") + (nobail_val ? "on" : "off"));
        show_notification(nobail_val ? "NoBail enabled" : "NoBail disabled", 2.5, blue);
    }
    if (toggle_row("Ramp Boosts Allowed", "Allows physics velocity speed boosts on ramps and quarter pipes", boosts_val)) {
        cfg.boosts = boosts_val;
        ServerEngine::instance().queue_command(std::string("boosts ") + (boosts_val ? "on" : "off"));
        show_notification(boosts_val ? "Ramp boosts enabled" : "Ramp boosts disabled", 2.5, blue);
    }
    if (toggle_row("NoClip Free-Cam Allowed", "Allows free flying camera mode for players", noclip_val)) {
        cfg.noclip = noclip_val;
        ServerEngine::instance().queue_command(std::string("noclip ") + (noclip_val ? "on" : "off"));
        show_notification(noclip_val ? "NoClip enabled" : "NoClip disabled", 2.5, blue);
    }
    if (toggle_row("Park Editor Object Placement", "Allows dropping ramps, rails, and props in the world", placement_val)) {
        cfg.object_placement = placement_val ? ObjectPlacement::everyone : ObjectPlacement::nobody;
        ServerEngine::instance().queue_command(std::string("placement ") + (placement_val ? "everyone" : "nobody"));
        show_notification(placement_val ? "Object placement enabled" : "Object placement disabled", 2.5, blue);
    }
    if (toggle_row("World Layer Sync", "Synchronizes level destruction and object layers across clients", layer_sync_val)) {
        cfg.world_layer_sync = layer_sync_val;
        ServerEngine::instance().queue_command(std::string("layer-sync ") + (layer_sync_val ? "on" : "off"));
        show_notification(layer_sync_val ? "World layer sync enabled" : "World layer sync disabled", 2.5, blue);
    }
    if (toggle_row("Tuning Enforcement", "Enforces server-authoritative physics tuning on connected clients", tuning_val)) {
        cfg.enforce_tuning = tuning_val;
        ServerEngine::instance().queue_command(std::string("tuning ") + (tuning_val ? "on" : "off"));
        show_notification(tuning_val ? "Tuning enforced" : "Tuning free", 2.5, blue);
    }
}

void draw_moderation_tab() {
    section("MODERATION & ACCESS CONTROLS");

    category_tabs(g_state.moderation_subtab,
                  {"SERVER BANS", "SERVER ADMINISTRATORS"},
                  "mod_tabs");

    if (g_state.moderation_subtab == 0) {
        // SERVER BANS
        const auto& bans = ServerEngine::instance().config().bans;
        const std::string ban_count = std::to_string(bans.size()) + (bans.size() == 1 ? " banned player" : " banned players");
        begin_card("card_bans_list", "BANNED PLAYERS", ban_count.c_str());

        if (bans.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted("Nobody is currently banned from this server. Ban a player from the Players tab, or add one below.");
            ImGui::PopStyleColor();
        } else {
            if (ImGui::BeginTable("bans_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Player Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("SteamID64", ImGuiTableColumnFlags_WidthFixed, S(160));
                ImGui::TableSetupColumn("Banned Date", ImGuiTableColumnFlags_WidthFixed, S(130));
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, S(100));
                ImGui::TableHeadersRow();

                for (const auto& ban : bans) {
                    ImGui::TableNextRow();

                    // Name
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushFont(g_fonts.bold);
                    ImGui::TextUnformatted(ban.name.empty() ? "Unknown Player" : ban.name.c_str());
                    ImGui::PopFont();

                    // SteamID
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushFont(g_fonts.mono);
                    ImGui::Text("%llu", ban.id);
                    ImGui::PopFont();

                    // Date
                    ImGui::TableSetColumnIndex(2);
                    std::array<char, 32> date{};
                    const auto added_t = static_cast<std::time_t>(ban.added);
                    std::tm local_tm{};
                    if (ban.added > 0 && localtime_s(&local_tm, &added_t) == 0) {
                        std::strftime(date.data(), date.size(), "%d %b %Y", &local_tm);
                    }
                    ImGui::TextUnformatted(date[0] ? date.data() : "-");

                    // Action
                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushID(static_cast<int>(ban.id));
                    if (rough_button("Unban", ImVec2(S(80), S(22)))) {
                        ServerEngine::instance().queue_command("unban " + std::to_string(ban.id));
                        show_notification("Unbanned player: " + std::to_string(ban.id), 3.5, good);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        end_card();

        // Ban by Steam ID Form (1-1 with in-game menu)
        begin_card("card_ban_add", "BAN BY STEAM ID", "Add a SteamID64 to the server ban list");
        ImGui::PushItemWidth(S(320));
        ImGui::InputTextWithHint("SteamID64", "7656119... (17 digits)", g_state.mod_steam_id, sizeof(g_state.mod_steam_id));
        ImGui::InputTextWithHint("Player Name / Reason", "Optional player identifier", g_state.mod_reason, sizeof(g_state.mod_reason));
        ImGui::PopItemWidth();

        ImGui::Spacing();
        if (rough_button("BAN PLAYER", ImVec2(S(160), S(32)))) {
            const std::string typed_id = g_state.mod_steam_id;
            const std::string typed_reason = g_state.mod_reason;
            if (!typed_id.empty()) {
                ServerEngine::instance().queue_command("ban " + typed_id + " " + typed_reason);
                show_notification("Issued ban for SteamID: " + typed_id, 3.5, danger);
                g_state.mod_steam_id[0] = '\0';
                g_state.mod_reason[0] = '\0';
            }
        }
        end_card();

    } else if (g_state.moderation_subtab == 1) {
        // SERVER ADMINISTRATORS
        const auto& admins = ServerEngine::instance().config().admins;
        begin_card("card_admins", "SERVER ADMINISTRATORS", "Users with authoritative in-game admin permissions");
        if (admins.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted("No administrators currently configured.");
            ImGui::PopStyleColor();
        } else {
            for (const auto admin_id : admins) {
                ImGui::AlignTextToFramePadding();
                ImGui::PushFont(g_fonts.mono);
                ImGui::Text("%llu", admin_id);
                ImGui::PopFont();
                ImGui::SameLine(0, S(16));
                ImGui::PushID(static_cast<int>(admin_id));
                if (rough_button("Revoke Admin", ImVec2(S(120), S(24)))) {
                    ServerEngine::instance().queue_command("admin remove " + std::to_string(admin_id));
                    show_notification("Revoked admin: " + std::to_string(admin_id), 3.0, warning);
                }
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        static char s_new_admin[64]{};
        ImGui::PushItemWidth(S(260));
        ImGui::InputTextWithHint("##new_admin_id", "SteamID64 (e.g. 7656119...)", s_new_admin, sizeof(s_new_admin));
        ImGui::PopItemWidth();
        ImGui::SameLine(0, S(10));
        if (rough_button("GRANT ADMIN", ImVec2(S(140), S(30)))) {
            if (s_new_admin[0]) {
                ServerEngine::instance().queue_command(std::string("admin add ") + s_new_admin);
                show_notification("Added administrator: " + std::string(s_new_admin), 3.5, good);
                s_new_admin[0] = '\0';
            }
        }
        end_card();
    }
}

void draw_interactive_telemetry_graph(const ServerStatusSnapshot& stats) {
    const auto& history = ServerEngine::instance().telemetry_history();

    // 1. Time scale selector
    category_tabs(g_state.telemetry_time_scale,
                  {"LAST 60 SECONDS", "LAST 5 MINUTES", "LAST 15 MINUTES", "LAST 1 HOUR"},
                  "telemetry_timescale_tabs");

    // 2. Metric selector
    category_tabs(g_state.telemetry_metric,
                  {"TICK RATE (TPS)", "FRAME LATENCY (MS)", "PHYSICAL MEMORY (MB)"},
                  "telemetry_metric_tabs");

    // Determine sample window
    size_t sample_window = 60;
    if (g_state.telemetry_time_scale == 1) sample_window = 300;
    else if (g_state.telemetry_time_scale == 2) sample_window = 900;
    else if (g_state.telemetry_time_scale == 3) sample_window = 3600;

    std::vector<float> metric_values;
    std::vector<uint32_t> player_counts;
    std::vector<uint64_t> timestamps;

    const size_t available = history.size();
    const size_t take_count = std::min(available, sample_window);
    const size_t start_idx = available > take_count ? available - take_count : 0;

    metric_values.reserve(take_count);
    player_counts.reserve(take_count);
    timestamps.reserve(take_count);

    for (size_t i = start_idx; i < available; ++i) {
        const auto& s = history[i];
        timestamps.push_back(s.uptime_seconds);
        player_counts.push_back(s.player_count);

        if (g_state.telemetry_metric == 0) {
            metric_values.push_back(s.tps);
        } else if (g_state.telemetry_metric == 1) {
            metric_values.push_back(s.frame_time_ms);
        } else {
            metric_values.push_back(s.memory_mb);
        }
    }

    // Fallback if no rolling samples yet
    if (metric_values.empty()) {
        timestamps.push_back(0);
        player_counts.push_back(stats.player_count);
        if (g_state.telemetry_metric == 0) metric_values.push_back(static_cast<float>(stats.actual_tps));
        else if (g_state.telemetry_metric == 1) metric_values.push_back(static_cast<float>(stats.frame_time_ms));
        else metric_values.push_back(static_cast<float>(ServerEngine::instance().process_memory_mb()));
    }

    // Key Statistics
    const float current_val = metric_values.back();
    float sum = 0.0f;
    float min_val = metric_values.front();
    float max_val = metric_values.front();

    for (float v : metric_values) {
        sum += v;
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
    }
    const float avg_val = sum / static_cast<float>(metric_values.size());

    // 3. KPI Statistical Summary Ribbon
    begin_card("card_telemetry_kpi", nullptr, nullptr);
    {
        const float card_w = ImGui::GetContentRegionAvail().x;
        const float col_w = card_w / 5.0f;
        auto* draw = ImGui::GetWindowDrawList();

        auto draw_kpi_cell = [&](int index, const char* label, const std::string& val_str, ImU32 col) {
            const auto origin = ImGui::GetCursorScreenPos();
            const ImVec2 cell_pos(origin.x + col_w * index, origin.y);
            draw->AddText(g_fonts.caption, S(11), cell_pos, muted, label);
            draw->AddText(g_fonts.bold, S(16), ImVec2(cell_pos.x, cell_pos.y + S(16)), col, val_str.c_str());
            if (index < 4) {
                draw->AddLine(ImVec2(cell_pos.x + col_w - S(10), cell_pos.y + S(4)),
                              ImVec2(cell_pos.x + col_w - S(10), cell_pos.y + S(36)),
                              IM_COL32(45, 48, 56, 255), 1.0f);
            }
        };

        char s_cur[32], s_avg[32], s_min[32], s_max[32], s_stab[32];
        ImU32 col_cur = good, col_avg = good, col_max = good;

        if (g_state.telemetry_metric == 0) {
            // TPS
            std::snprintf(s_cur, sizeof(s_cur), "%.1f TPS", current_val);
            std::snprintf(s_avg, sizeof(s_avg), "%.1f TPS", avg_val);
            std::snprintf(s_min, sizeof(s_min), "%.1f TPS", min_val);
            std::snprintf(s_max, sizeof(s_max), "%.1f TPS", max_val);
            const float stab = avg_val > 0.0f ? std::clamp((avg_val / 20.0f) * 100.0f, 0.0f, 100.0f) : 0.0f;
            std::snprintf(s_stab, sizeof(s_stab), "%.1f%% Optimal", stab);
            col_cur = current_val >= 19.0f ? good : (current_val >= 15.0f ? warning : danger);
            col_avg = avg_val >= 19.0f ? good : (avg_val >= 15.0f ? warning : danger);
            col_max = good;
        } else if (g_state.telemetry_metric == 1) {
            // Frame Latency (ms)
            std::snprintf(s_cur, sizeof(s_cur), "%.2f ms", current_val);
            std::snprintf(s_avg, sizeof(s_avg), "%.2f ms", avg_val);
            std::snprintf(s_min, sizeof(s_min), "%.2f ms", min_val);
            std::snprintf(s_max, sizeof(s_max), "%.2f ms", max_val);
            std::snprintf(s_stab, sizeof(s_stab), "P99: %.2f ms", max_val);
            col_cur = current_val < 5.0f ? good : (current_val < 20.0f ? warning : danger);
            col_avg = avg_val < 5.0f ? good : (avg_val < 20.0f ? warning : danger);
            col_max = max_val < 10.0f ? good : (max_val < 30.0f ? warning : danger);
        } else {
            // Memory (MB)
            std::snprintf(s_cur, sizeof(s_cur), "%.1f MB", current_val);
            std::snprintf(s_avg, sizeof(s_avg), "%.1f MB", avg_val);
            std::snprintf(s_min, sizeof(s_min), "%.1f MB", min_val);
            std::snprintf(s_max, sizeof(s_max), "%.1f MB", max_val);
            std::snprintf(s_stab, sizeof(s_stab), "Delta: %+.1f MB", current_val - metric_values.front());
            col_cur = current_val < 500.0f ? good : (current_val < 1000.0f ? warning : danger);
            col_avg = good;
            col_max = good;
        }

        draw_kpi_cell(0, "CURRENT VALUE", s_cur, col_cur);
        draw_kpi_cell(1, "ROLLING AVERAGE", s_avg, col_avg);
        draw_kpi_cell(2, "MINIMUM DIP", s_min, (g_state.telemetry_metric == 0 && min_val < 18.0f) ? warning : muted);
        draw_kpi_cell(3, "PEAK SPIKE", s_max, col_max);
        draw_kpi_cell(4, "LOOP STABILITY", s_stab, good);

        ImGui::Dummy(ImVec2(card_w, S(42)));
    }
    end_card();

    // 4. Vector Performance Graph Canvas
    const float graph_w = ImGui::GetContentRegionAvail().x;
    const float graph_h = S(190.0f);
    const auto canvas_pos = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();

    ImGui::PushID("telemetry_graph_canvas");
    ImGui::InvisibleButton("##graph_hitbox", ImVec2(graph_w, graph_h));
    const bool is_canvas_hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    // Background tile
    draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + graph_w, canvas_pos.y + graph_h), IM_COL32(14, 15, 18, 255));
    draw->AddRect(canvas_pos, ImVec2(canvas_pos.x + graph_w, canvas_pos.y + graph_h), IM_COL32(35, 38, 46, 255));

    float axis_min = 0.0f;
    float axis_max = 25.0f;
    std::vector<std::pair<float, std::string>> grid_levels;

    if (g_state.telemetry_metric == 0) {
        axis_min = 0.0f;
        axis_max = 25.0f;
        grid_levels = {
            {20.0f, "20 TPS (Target)"},
            {15.0f, "15 TPS"},
            {10.0f, "10 TPS"},
            {5.0f, "5 TPS"},
            {0.0f, "0 TPS"}
        };
    } else if (g_state.telemetry_metric == 1) {
        axis_min = 0.0f;
        const float peak_headroom = std::max(0.5f, max_val * 1.35f);
        if (peak_headroom <= 2.0f) {
            axis_max = 2.0f;
            grid_levels = {
                {2.0f, "2.0 ms"},
                {1.5f, "1.5 ms"},
                {1.0f, "1.0 ms"},
                {0.5f, "0.5 ms"},
                {0.0f, "0 ms"}
            };
        } else if (peak_headroom <= 5.0f) {
            axis_max = 5.0f;
            grid_levels = {
                {5.0f, "5.0 ms"},
                {3.75f, "3.75 ms"},
                {2.5f, "2.5 ms"},
                {1.25f, "1.25 ms"},
                {0.0f, "0 ms"}
            };
        } else if (peak_headroom <= 10.0f) {
            axis_max = 10.0f;
            grid_levels = {
                {10.0f, "10 ms"},
                {7.5f, "7.5 ms"},
                {5.0f, "5.0 ms"},
                {2.5f, "2.5 ms"},
                {0.0f, "0 ms"}
            };
        } else if (peak_headroom <= 25.0f) {
            axis_max = 25.0f;
            grid_levels = {
                {25.0f, "25 ms"},
                {20.0f, "20 ms"},
                {15.0f, "15 ms"},
                {10.0f, "10 ms"},
                {5.0f, "5.0 ms"},
                {0.0f, "0 ms"}
            };
        } else {
            axis_max = std::max(50.0f, std::ceil(peak_headroom / 10.0f) * 10.0f);
            grid_levels = {
                {50.0f, "50 ms (Budget)"},
                {axis_max, std::format("{:.0f} ms", axis_max)},
                {axis_max * 0.75f, std::format("{:.0f} ms", axis_max * 0.75f)},
                {axis_max * 0.50f, std::format("{:.0f} ms", axis_max * 0.50f)},
                {axis_max * 0.25f, std::format("{:.0f} ms", axis_max * 0.25f)},
                {0.0f, "0 ms"}
            };
        }
    } else {
        axis_min = 0.0f;
        axis_max = std::max(200.0f, std::ceil(max_val * 1.25f / 50.0f) * 50.0f);
        grid_levels = {
            {axis_max, std::format("{:.0f} MB", axis_max)},
            {axis_max * 0.75f, std::format("{:.0f} MB", axis_max * 0.75f)},
            {axis_max * 0.50f, std::format("{:.0f} MB", axis_max * 0.50f)},
            {axis_max * 0.25f, std::format("{:.0f} MB", axis_max * 0.25f)},
            {0.0f, "0 MB"}
        };
    }

    float max_lbl_w = 0.0f;
    for (const auto& [level, label] : grid_levels) {
        const auto sz = g_fonts.caption->CalcTextSizeA(S(10), FLT_MAX, 0, label.c_str());
        if (sz.x > max_lbl_w) max_lbl_w = sz.x;
    }

    const float pad_left = max_lbl_w + S(18.0f);
    const float pad_right = S(18.0f);
    const float pad_top = S(18.0f);
    const float pad_bottom = S(26.0f);

    const float plot_w = graph_w - pad_left - pad_right;
    const float plot_h = graph_h - pad_top - pad_bottom;

    for (const auto& [level, label] : grid_levels) {
        if (level < axis_min || level > axis_max) continue;
        const float norm_y = (level - axis_min) / (axis_max - axis_min);
        const float line_y = canvas_pos.y + pad_top + plot_h * (1.0f - norm_y);

        const bool is_target_level = (g_state.telemetry_metric == 0 && level == 20.0f) || (g_state.telemetry_metric == 1 && level == 50.0f);
        const ImU32 line_col = is_target_level ? IM_COL32(1, 131, 255, 120) : IM_COL32(32, 35, 42, 255);
        const ImU32 txt_col = is_target_level ? blue : IM_COL32(90, 95, 105, 255);

        draw->AddLine(ImVec2(canvas_pos.x + pad_left, line_y),
                      ImVec2(canvas_pos.x + pad_left + plot_w, line_y),
                      line_col, 1.0f);

        const auto lbl_sz = g_fonts.caption->CalcTextSizeA(S(10), FLT_MAX, 0, label.c_str());
        draw->AddText(g_fonts.caption, S(10),
                      ImVec2(canvas_pos.x + pad_left - lbl_sz.x - S(6), line_y - lbl_sz.y * 0.5f),
                      txt_col, label.c_str());
    }

    const size_t count = metric_values.size();
    std::vector<ImVec2> points;
    points.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.0f;
        const float px = canvas_pos.x + pad_left + plot_w * t;
        const float val = metric_values[i];
        const float norm_y = std::clamp((val - axis_min) / (axis_max - axis_min), 0.0f, 1.0f);
        const float py = canvas_pos.y + pad_top + plot_h * (1.0f - norm_y);
        points.push_back(ImVec2(px, py));
    }

    if (points.size() >= 2) {
        std::vector<ImVec2> fill_poly;
        fill_poly.reserve(points.size() + 2);
        fill_poly.push_back(ImVec2(points.front().x, canvas_pos.y + pad_top + plot_h));
        for (const auto& pt : points) fill_poly.push_back(pt);
        fill_poly.push_back(ImVec2(points.back().x, canvas_pos.y + pad_top + plot_h));

        draw->AddConvexPolyFilled(fill_poly.data(), static_cast<int>(fill_poly.size()), IM_COL32(1, 131, 255, 30));
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), blue, 0, S(2.0f));
    } else if (points.size() == 1) {
        draw->AddCircleFilled(points[0], S(4.0f), blue);
    }

    const char* t_start = g_state.telemetry_time_scale == 0 ? "-60s" : (g_state.telemetry_time_scale == 1 ? "-5m" : (g_state.telemetry_time_scale == 2 ? "-15m" : "-1h"));
    const char* t_mid = g_state.telemetry_time_scale == 0 ? "-30s" : (g_state.telemetry_time_scale == 1 ? "-2.5m" : (g_state.telemetry_time_scale == 2 ? "-7.5m" : "-30m"));
    const char* t_now = "Now";

    draw->AddText(g_fonts.caption, S(10), ImVec2(canvas_pos.x + pad_left, canvas_pos.y + pad_top + plot_h + S(4)), muted, t_start);
    const auto sz_mid = g_fonts.caption->CalcTextSizeA(S(10), FLT_MAX, 0, t_mid);
    draw->AddText(g_fonts.caption, S(10), ImVec2(canvas_pos.x + pad_left + (plot_w - sz_mid.x) * 0.5f, canvas_pos.y + pad_top + plot_h + S(4)), muted, t_mid);
    const auto sz_now = g_fonts.caption->CalcTextSizeA(S(10), FLT_MAX, 0, t_now);
    draw->AddText(g_fonts.caption, S(10), ImVec2(canvas_pos.x + pad_left + plot_w - sz_now.x, canvas_pos.y + pad_top + plot_h + S(4)), good, t_now);

    const auto io = ImGui::GetIO();
    if (is_canvas_hovered && count > 0 && io.MousePos.x >= canvas_pos.x + pad_left && io.MousePos.x <= canvas_pos.x + pad_left + plot_w) {
        const float rel_x = (io.MousePos.x - (canvas_pos.x + pad_left)) / plot_w;
        const size_t hover_idx = std::clamp(static_cast<size_t>(std::round(rel_x * (count - 1))), static_cast<size_t>(0), count - 1);

        const ImVec2 pt = points[hover_idx];
        const float val = metric_values[hover_idx];
        const uint32_t p_count = player_counts[hover_idx];

        draw->AddLine(ImVec2(pt.x, canvas_pos.y + pad_top), ImVec2(pt.x, canvas_pos.y + pad_top + plot_h), IM_COL32(255, 255, 255, 140), 1.0f);
        draw->AddCircleFilled(pt, S(5.0f), IM_COL32(255, 255, 255, 255));
        draw->AddCircleFilled(pt, S(3.5f), blue);

        char hud_val[64], hud_time[64], hud_players[32];
        const int secs_ago = static_cast<int>((count - 1 - hover_idx));
        if (secs_ago == 0) std::snprintf(hud_time, sizeof(hud_time), "Live (Current)");
        else if (secs_ago < 60) std::snprintf(hud_time, sizeof(hud_time), "-%ds ago", secs_ago);
        else std::snprintf(hud_time, sizeof(hud_time), "-%dm %ds ago", secs_ago / 60, secs_ago % 60);

        if (g_state.telemetry_metric == 0) std::snprintf(hud_val, sizeof(hud_val), "TPS: %.1f", val);
        else if (g_state.telemetry_metric == 1) std::snprintf(hud_val, sizeof(hud_val), "Latency: %.2f ms", val);
        else std::snprintf(hud_val, sizeof(hud_val), "Memory: %.1f MB", val);

        std::snprintf(hud_players, sizeof(hud_players), "Players: %u", p_count);

        const auto sz_v = g_fonts.bold->CalcTextSizeA(S(13), FLT_MAX, 0, hud_val);
        const auto sz_t = g_fonts.caption->CalcTextSizeA(S(11), FLT_MAX, 0, hud_time);
        const auto sz_p = g_fonts.caption->CalcTextSizeA(S(11), FLT_MAX, 0, hud_players);

        const float hud_w = std::max({sz_v.x, sz_t.x, sz_p.x}) + S(20.0f);
        const float hud_h = S(58.0f);

        float hud_x = pt.x + S(12.0f);
        if (hud_x + hud_w > canvas_pos.x + graph_w - S(10.0f)) {
            hud_x = pt.x - hud_w - S(12.0f);
        }
        float hud_y = pt.y - hud_h * 0.5f;
        if (hud_y < canvas_pos.y + pad_top) hud_y = canvas_pos.y + pad_top;
        if (hud_y + hud_h > canvas_pos.y + graph_h - S(8.0f)) hud_y = canvas_pos.y + graph_h - hud_h - S(8.0f);

        draw->AddRectFilled(ImVec2(hud_x, hud_y), ImVec2(hud_x + hud_w, hud_y + hud_h), IM_COL32(22, 25, 32, 245), S(4));
        draw->AddRect(ImVec2(hud_x, hud_y), ImVec2(hud_x + hud_w, hud_y + hud_h), IM_COL32(60, 65, 78, 255), S(4));
        draw->AddRectFilled(ImVec2(hud_x, hud_y), ImVec2(hud_x + S(3), hud_y + hud_h), blue, S(4), ImDrawFlags_RoundCornersLeft);

        draw->AddText(g_fonts.bold, S(13), ImVec2(hud_x + S(10), hud_y + S(6)), white, hud_val);
        draw->AddText(g_fonts.caption, S(11), ImVec2(hud_x + S(10), hud_y + S(24)), muted, hud_time);
        draw->AddText(g_fonts.caption, S(11), ImVec2(hud_x + S(10), hud_y + S(38)), good, hud_players);
    }

    ImGui::Dummy(ImVec2(0, S(14)));
}

void draw_telemetry_tab() {
    section("SERVER TELEMETRY & PERFORMANCE METRICS");

    const auto stats = ServerEngine::instance().status_snapshot();
    const double mem_mb = ServerEngine::instance().process_memory_mb();
    const auto players = ServerEngine::instance().player_list();

    // Render the interactive Vector Performance Graph
    draw_interactive_telemetry_graph(stats);

    ImGui::Columns(2, "stock_telemetry_cards", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.50f);

        // Card 1: Server Tick & Engine Health
        begin_card("card_tick_health", "SERVER TICK & ENGINE HEALTH", "Tick pacing and loop performance");
        stat_row("Target Engine Rate", std::to_string(stats.target_tps) + " TPS (Dedicated Standard)");
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f TPS", stats.actual_tps);
        stat_row("Measured Tick Rate", stats.state == ServerState::Running ? buf : "0.0 TPS (Offline)", stats.state == ServerState::Running ? good : muted);
        std::snprintf(buf, sizeof(buf), "%.2f ms", stats.frame_time_ms);
        stat_row("Frame Execution Latency", stats.state == ServerState::Running ? buf : "0.00 ms");
        const double budget_pct = (stats.frame_time_ms / (1000.0 / 20.0)) * 100.0;
        std::snprintf(buf, sizeof(buf), "%.1f%% of 50ms tick budget", budget_pct);
        stat_row("Tick Budget Share", stats.state == ServerState::Running ? buf : "0.0%");
        stat_row("Engine Health Status", stats.state == ServerState::Running ? (stats.frame_time_ms < 5.0 ? "OPTIMAL (Sub-millisecond)" : "STABLE") : "OFFLINE", stats.state == ServerState::Running ? good : muted);
        end_card();

        // Card 2: Steam SDR Networking & Protocols
        begin_card("card_steam_proto", "STEAM SDR NETWORKING & TRAFFIC", "Valve SDR transport and stream batching");
        stat_row("Transport Architecture", "Valve Steam Datagram Relay (SDR)", good);
        stat_row("Pose Compression", "PoseBatch Tier 0-3 + DeltaCodec", good);
        stat_row("Audio Stream Codec", "SoundCodec (Opus 20ms Frame Rate)", good);
        stat_row("Wire Encoding", "Byte-Packed BitStream (Optimized)", good);
        stat_row("Direct UDP Fallback", "ENABLED (Direct Port 27015)", good);
        end_card();

        ImGui::NextColumn();

        // Card 3: Process Threads & Memory
        begin_card("card_process_mem", "SERVER PROCESS & MEMORY", "Host execution environment metrics");
        std::snprintf(buf, sizeof(buf), "%.1f MB Working Set", mem_mb);
        stat_row("Physical Memory (RAM)", buf);
        stat_row("Worker Thread Dispatch", "Multi-Threaded (Up to 8 cores)", good);
        stat_row("Monotonic Clock", "High-Resolution Monotonic Epoch", good);
        stat_row("Release Build Platform", "Windows x64 (MSVC Release)", good);
        end_card();

        // Card 4: Traffic & Relay Protocol
        begin_card("card_traffic_stats", "TRAFFIC & RELAY PROTOCOL", "Network routing and connection state");
        stat_row("Direct UDP Port", "Port " + std::to_string(stats.port));
        stat_row("Query Port", "Port " + std::to_string(stats.query_port));
        stat_row("Active Player Streams", std::to_string(stats.player_count) + " stream(s)");
        stat_row("Steam Relay Status", stats.state == ServerState::Running ? "CONNECTED (Active Relay Cluster)" : "OFFLINE", stats.state == ServerState::Running ? good : muted);
        end_card();

        ImGui::Columns(1);

        ImGui::Dummy(ImVec2(0, S(8)));

        // Live Client Network Performance Table
        section("LIVE CLIENT NETWORK MONITOR");
        if (players.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, muted);
            ImGui::TextUnformatted("No players currently connected. Live client ping and network telemetry will appear once players join.");
            ImGui::PopStyleColor();
        } else {
            if (ImGui::BeginTable("client_perf_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("SteamID64", ImGuiTableColumnFlags_WidthFixed, S(140));
                ImGui::TableSetupColumn("Player Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Round-Trip Ping", ImGuiTableColumnFlags_WidthFixed, S(120));
                ImGui::TableSetupColumn("World Position", ImGuiTableColumnFlags_WidthFixed, S(130));
                ImGui::TableSetupColumn("Status & Role", ImGuiTableColumnFlags_WidthFixed, S(110));
                ImGui::TableHeadersRow();

                for (const auto& p : players) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushFont(g_fonts.mono);
                    ImGui::Text("%llu", p.id);
                    ImGui::PopFont();

                    ImGui::TableSetColumnIndex(1);
                    const auto [t_role_col, t_role_tag] = player_role(p.id, p.admin);
                    const float f_sz = S(13.0f);
                    const auto line_ht = ImGui::GetTextLineHeight();
                    const auto c_pos = ImGui::GetCursorScreenPos();
                    auto* t_draw = ImGui::GetWindowDrawList();
                    float t_cur_x = c_pos.x;
                    if (!t_role_tag.empty()) {
                        draw_role_badge(t_draw, g_fonts.bold, f_sz, ImVec2(t_cur_x, c_pos.y), line_ht, t_role_tag, t_role_col, 1.0f);
                        t_cur_x += role_badge_width(g_fonts.bold, f_sz, t_role_tag) + S(6.0f);
                    }
                    const auto t_namesz = g_fonts.bold->CalcTextSizeA(f_sz, FLT_MAX, 0.0f, p.name.c_str());
                    t_draw->AddText(g_fonts.bold, f_sz, ImVec2(t_cur_x + 1.0f, c_pos.y + 1.0f), IM_COL32(0, 0, 0, 180), p.name.c_str());
                    const int t_name_v = t_draw->VtxBuffer.Size;
                    t_draw->AddText(g_fonts.bold, f_sz, ImVec2(t_cur_x, c_pos.y), (!t_role_tag.empty()) ? t_role_col : white, p.name.c_str());
                    shade_nametag_gradient(t_draw, t_name_v, t_cur_x, t_namesz.x, t_role_col, ImGui::GetTime());
                    ImGui::Dummy(ImVec2(t_cur_x - c_pos.x + t_namesz.x, line_ht));

                    ImGui::TableSetColumnIndex(2);
                    const auto ping_col = p.ping < 60 ? good : p.ping < 130 ? warning : danger;
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ping_col), "%u ms", p.ping);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushFont(g_fonts.mono);
                    ImGui::Text("%.0f, %.0f, %.0f", p.x, p.y, p.z);
                    ImGui::PopFont();

                    ImGui::TableSetColumnIndex(4);
                    if (!t_role_tag.empty()) {
                        badge(t_role_tag.c_str(), animated_nametag_colour(t_role_col, ImGui::GetTime()), black);
                    } else {
                        badge("PLAYER", good, black);
                    }
                }
                ImGui::EndTable();
            }
        }
    }


static void render_colored_log_text(const std::string& text, ImU32 default_col, float indent_x = -1.0f) {
    if (text.empty()) {
        ImGui::NewLine();
        return;
    }

    struct LogToken {
        enum class Type { Word, Space, Newline };
        Type type;
        std::string text;
        ImU32 color;
    };

    std::vector<LogToken> tokens;
    tokens.reserve(text.size() / 4 + 4);

    const bool has_carets = (text.find('^') != std::string::npos);
    ImU32 current_col = has_carets ? skate_theme::white : default_col;

    auto get_token_color = [&](const std::string& word, ImU32 base_col) -> ImU32 {
        if (has_carets) return base_col;
        if (word.empty()) return base_col;

        // Bracketed Tag Coloring
        if (word.front() == '[' && word.back() == ']') {
            std::string tag = word;
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            if (tag == "[+]" || tag == "[join]" || tag == "[connect]") return IM_COL32(80, 230, 100, 255);
            if (tag == "[-]" || tag == "[leave]" || tag == "[left]" || tag == "[disconnect]" || tag == "[kicked]" || tag == "[ban]" || tag == "[banned]") return IM_COL32(255, 95, 95, 255);
            if (tag == "[chat]") return IM_COL32(255, 215, 100, 255);
            if (tag == "[cmd]" || tag == "[command]") return IM_COL32(85, 175, 255, 255);
            if (tag == "[admin]") return IM_COL32(215, 130, 255, 255);
            if (tag == "[map]") return IM_COL32(90, 235, 180, 255);
            if (tag == "[steam]" || tag == "[direct]") return IM_COL32(115, 185, 255, 255);
            if (tag == "[network]" || tag == "[traffic]") return IM_COL32(75, 220, 240, 255);
            if (tag == "[words]" || tag == "[afk]" || tag == "[objects]") return IM_COL32(255, 175, 75, 255);
            if (tag == "[updates]" || tag == "[update]") return IM_COL32(255, 215, 60, 255);
            if (tag == "[error]" || tag == "[fatal]" || tag == "[fail]") return IM_COL32(255, 80, 80, 255);
            if (tag == "[warning]" || tag == "[warn]") return IM_COL32(255, 205, 50, 255);
            return IM_COL32(180, 185, 195, 255);
        }

        // Keywords and semantic tokens
        if (word == "joined" || word == "connected" || word == "joined,") return IM_COL32(80, 230, 100, 255);
        if (word == "left" || word == "kicked:" || word == "kicked" || word == "banned" || word == "removed") return IM_COL32(255, 95, 95, 255);
        if (word == "admin" || word == "admin,") return IM_COL32(215, 130, 255, 255);
        if (word.starts_with("http://") || word.starts_with("https://")) return IM_COL32(95, 180, 255, 255);

        // SteamID (17 digits starting with 7656119)
        if (word.size() >= 17 && word.starts_with("7656119")) {
            bool all_digits = true;
            for (size_t c = 0; c < 17; ++c) {
                if (!std::isdigit(static_cast<unsigned char>(word[c]))) { all_digits = false; break; }
            }
            if (all_digits) return IM_COL32(130, 220, 255, 255);
        }

        return base_col;
    };

    std::string cur_buf;
    LogToken::Type cur_type = LogToken::Type::Word;

    auto flush_token = [&](LogToken::Type t) {
        if (!cur_buf.empty()) {
            ImU32 col = (t == LogToken::Type::Word) ? get_token_color(cur_buf, current_col) : current_col;
            tokens.push_back({ t, cur_buf, col });
            cur_buf.clear();
        }
    };

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '^' && i + 1 < text.size()) {
            char code = text[i + 1];
            if ((code >= '0' && code <= '9') || code == 'r' || code == 'R') {
                flush_token(cur_type);
                switch (code) {
                    case '1': current_col = IM_COL32(255, 80, 80, 255); break;   // Red
                    case '2': current_col = IM_COL32(80, 230, 100, 255); break;  // Green
                    case '3': current_col = IM_COL32(255, 220, 60, 255); break;  // Yellow
                    case '4': current_col = IM_COL32(80, 160, 255, 255); break;  // Blue
                    case '5': current_col = IM_COL32(60, 215, 255, 255); break;  // Cyan
                    case '6': current_col = IM_COL32(220, 120, 255, 255); break; // Magenta
                    case '7': current_col = skate_theme::white; break;            // White / Default
                    case '8': current_col = IM_COL32(255, 160, 50, 255); break;  // Orange
                    case '9': current_col = IM_COL32(140, 145, 155, 255); break; // Grey
                    case '0':
                    case 'r':
                    case 'R': current_col = skate_theme::white; break;
                }
                ++i;
                continue;
            }
        }

        if (text[i] == '\n') {
            flush_token(cur_type);
            tokens.push_back({ LogToken::Type::Newline, "\n", current_col });
            continue;
        }

        if (text[i] == ' ' || text[i] == '\t') {
            if (cur_type != LogToken::Type::Space) {
                flush_token(cur_type);
                cur_type = LogToken::Type::Space;
            }
            cur_buf += text[i];
        } else {
            if (cur_type != LogToken::Type::Word) {
                flush_token(cur_type);
                cur_type = LogToken::Type::Word;
            }
            cur_buf += text[i];
            // Split breakable punctuation so URLs and comma-separated lists wrap at punctuation marks
            if (text[i] == ',' || text[i] == '/' || text[i] == '&' || text[i] == ';' || text[i] == '?') {
                flush_token(cur_type);
            }
        }
    }
    flush_token(cur_type);

    if (tokens.empty()) {
        ImGui::NewLine();
        return;
    }

    const float window_left_x = ImGui::GetWindowPos().x;
    const float window_w = ImGui::GetWindowWidth();
    const float scrollbar_w = ImGui::GetStyle().ScrollbarSize;
    const float padding_x = ImGui::GetStyle().WindowPadding.x;
    // Right margin limit for text rendering
    const float max_screen_x = window_left_x + window_w - padding_x - scrollbar_w - S(6.0f);

    float start_screen_x = ImGui::GetCursorScreenPos().x;
    float indent_screen_x = (indent_x >= 0.0f) ? (window_left_x + indent_x) : start_screen_x;
    if (max_screen_x - indent_screen_x < S(160.0f)) {
        indent_screen_x = window_left_x + padding_x;
    }

    float current_screen_x = start_screen_x;
    bool first_on_line = true;
    size_t idx = 0;

    while (idx < tokens.size()) {
        if (tokens[idx].type == LogToken::Type::Newline) {
            if (first_on_line) {
                ImGui::NewLine();
            }
            ImGui::SetCursorScreenPos(ImVec2(indent_screen_x, ImGui::GetCursorScreenPos().y));
            current_screen_x = indent_screen_x;
            first_on_line = true;
            ++idx;
            continue;
        }

        if (tokens[idx].type == LogToken::Type::Space) {
            if (first_on_line) {
                ++idx;
                continue;
            }
            float sp_w = ImGui::CalcTextSize(tokens[idx].text.c_str()).x;
            if (current_screen_x + sp_w <= max_screen_x) {
                ImGui::PushStyleColor(ImGuiCol_Text, tokens[idx].color);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(tokens[idx].text.c_str());
                ImGui::PopStyleColor();
                current_screen_x += sp_w;
            } else {
                // Trailing space overflows; cursor is already on next line after previous TextUnformatted
                ImGui::SetCursorScreenPos(ImVec2(indent_screen_x, ImGui::GetCursorScreenPos().y));
                current_screen_x = indent_screen_x;
                first_on_line = true;
            }
            ++idx;
            continue;
        }

        // Word token
        float tok_w = ImGui::CalcTextSize(tokens[idx].text.c_str()).x;
        if (!first_on_line && current_screen_x + tok_w > max_screen_x) {
            // TextUnformatted on previous item already advanced CursorPos.y to the next row!
            // Directly align cursor on that next row without inserting an empty line:
            ImGui::SetCursorScreenPos(ImVec2(indent_screen_x, ImGui::GetCursorScreenPos().y));
            current_screen_x = indent_screen_x;
            first_on_line = true;
        }

        if (tok_w > (max_screen_x - indent_screen_x)) {
            const auto& str = tokens[idx].text;
            ImGui::PushStyleColor(ImGuiCol_Text, tokens[idx].color);
            size_t pos = 0;
            while (pos < str.size()) {
                float avail = max_screen_x - current_screen_x;
                size_t take = 0;
                float sub_w = 0.0f;
                while (pos + take < str.size()) {
                    char ch = str[pos + take];
                    char ch_buf[2] = { ch, '\0' };
                    float ch_w = ImGui::CalcTextSize(ch_buf).x;
                    if (take > 0 && sub_w + ch_w > avail) break;
                    sub_w += ch_w;
                    ++take;
                }
                if (take == 0) take = 1;

                std::string sub = str.substr(pos, take);
                if (!first_on_line) {
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ImGui::TextUnformatted(sub.c_str());
                current_screen_x += ImGui::CalcTextSize(sub.c_str()).x;
                first_on_line = false;
                pos += take;

                if (pos < str.size()) {
                    // Next row for remaining characters of long token
                    ImGui::SetCursorScreenPos(ImVec2(indent_screen_x, ImGui::GetCursorScreenPos().y));
                    current_screen_x = indent_screen_x;
                    first_on_line = true;
                }
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, tokens[idx].color);
            if (!first_on_line) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::TextUnformatted(tokens[idx].text.c_str());
            ImGui::PopStyleColor();
            current_screen_x += tok_w;
            first_on_line = false;
        }

        ++idx;
    }
}

void draw_console_tab(const ServerStatusSnapshot& stats) {
    section("LIVE SERVER TERMINAL & ACTIVITY STREAM");

    // Lifecycle Action Bar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tile);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::BeginChild("console_control_bar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const auto at = ImGui::GetWindowPos();
    ImU32 bar_accent = blue;
    if (stats.state == ServerState::Running) bar_accent = good;
    else if (stats.state == ServerState::Error) bar_accent = danger;
    else if (stats.state == ServerState::Starting || stats.state == ServerState::Stopping) bar_accent = warning;
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + S(4), at.y + ImGui::GetWindowHeight()), bar_accent);

    // Quick Command Buttons (Left Side)
    const bool is_online = stats.state == ServerState::Running;
    if (rough_button("status", ImVec2(S(75), S(30)))) {
        if (is_online) ServerEngine::instance().queue_command("status");
        else ServerEngine::instance().log("Server is offline. Click START to boot the server lobby.", LogLevel::warning);
    }
    ImGui::SameLine(0, S(6));
    if (rough_button("players", ImVec2(S(75), S(30)))) {
        if (is_online) ServerEngine::instance().queue_command("players");
        else ServerEngine::instance().log("Server is offline. No players connected.", LogLevel::warning);
    }
    ImGui::SameLine(0, S(6));
    if (rough_button("help", ImVec2(S(75), S(30)))) {
        if (is_online) ServerEngine::instance().queue_command("help");
        else ServerEngine::instance().log("Server is offline. Click START or type 'start' to boot.", LogLevel::info);
    }
    ImGui::SameLine(0, S(14));
    ImGui::Checkbox("Auto-Scroll", &g_state.auto_scroll);

    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(4)));

    // Error Alert Box (if in error state)
    if (stats.state == ServerState::Error) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(40, 16, 18, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
        ImGui::BeginChild("console_err_box", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        const auto eat = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(eat, ImVec2(eat.x + S(4), eat.y + ImGui::GetWindowHeight()), danger);

        ImGui::PushFont(g_fonts.bold);
        ImGui::PushStyleColor(ImGuiCol_Text, danger);
        ImGui::TextUnformatted("SERVER ERROR DETECTED:");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::PushFont(g_fonts.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 180, 255));
        ImGui::TextWrapped("%s", stats.last_error.empty() ? "Unknown startup failure." : stats.last_error.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::BulletText("Port Conflict: Make sure another server is not running on UDP 27015/27016.");
        ImGui::BulletText("Check steam_token or network connection if Steam sign-in timed out.");
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, S(4)));
    }

    // Console output window
    const float footer_height = S(110.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10, 11, 14, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(2)));
    ImGui::BeginChild("console_output", ImVec2(0, -footer_height), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const auto logs = ServerEngine::instance().log_entries();
    ImGui::PushFont(g_fonts.mono);

    for (const auto& entry : logs) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(110, 115, 125, 255));
        ImGui::Text("[%s] ", entry.timestamp.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const float indent_x = ImGui::GetCursorPosX();

        ImU32 text_col = white;
        switch (entry.level) {
        case LogLevel::error: text_col = danger; break;
        case LogLevel::warning: text_col = warning; break;
        case LogLevel::success: text_col = good; break;
        case LogLevel::chat: text_col = IM_COL32(255, 215, 100, 255); break;
        case LogLevel::command: text_col = blue; break;
        default: text_col = white; break;
        }

        render_colored_log_text(entry.text, text_col, indent_x);
    }

    if (g_state.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, S(4)));

    // Command input bar
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - S(100));
    const bool enter_pressed = ImGui::InputTextWithHint("##cmd_in", "Type command (e.g. status, kick <player>, ban <id>, map <dest>, help)...",
                                                        g_state.console_input, sizeof(g_state.console_input),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    if (rough_button("SEND", ImVec2(S(90), S(34))) || enter_pressed) {
        if (g_state.console_input[0]) {
            std::string cmd = g_state.console_input;
            if (ServerEngine::instance().state() != ServerState::Running) {
                ServerEngine::instance().log("[cmd] " + cmd, LogLevel::command);
                if (cmd == "start" || cmd == "boot") {
                    ServerEngine::instance().start_server_async();
                } else {
                    ServerEngine::instance().log("Server is offline. Start the server to execute live console commands.", LogLevel::warning);
                }
            } else {
                ServerEngine::instance().queue_command(cmd);
            }
            g_state.command_history.push_back(cmd);
            g_state.history_index = -1;
            g_state.console_input[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    ImGui::Dummy(ImVec2(0, S(6)));

    // Bottom Action & Status Toolbar
    const float cur_bar_y = ImGui::GetCursorPosY();
    const float bar_h = S(30.0f);
    const auto origin_bar = ImGui::GetCursorScreenPos();

    // Left Side: Live Status Pill + Player Count Pill + Session Metadata
    draw_server_status_pill(stats.state);
    ImGui::SameLine(0, S(8));
    draw_player_count_pill(stats.player_count, stats.max_players, stats.state == ServerState::Running);

    if (stats.state == ServerState::Running) {
        ImGui::SameLine(0, S(16));
        const auto meta_start_pos = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        const float font_size = S(12.0f);

        float cur_x = meta_start_pos.x;

        // 1. Draw "MAP:"
        const auto sz_map_lbl = g_fonts.bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "MAP:");
        draw->AddText(g_fonts.bold, font_size, ImVec2(cur_x, origin_bar.y + (bar_h - sz_map_lbl.y) * 0.5f), muted, "MAP:");
        cur_x += sz_map_lbl.x + S(6.0f);

        // 2. Draw Map Name (in Blue)
        const std::string map_str = stats.map_name.empty() ? "San Vansterdam" : stats.map_name;
        const auto sz_map_val = g_fonts.bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, map_str.c_str());
        draw->AddText(g_fonts.bold, font_size, ImVec2(cur_x, origin_bar.y + (bar_h - sz_map_val.y) * 0.5f), blue, map_str.c_str());
        cur_x += sz_map_val.x + S(20.0f);

        // 3. Draw "JOIN:" + Clickable Join Code
        if (!stats.invite_code.empty()) {
            const auto sz_join_lbl = g_fonts.bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "JOIN:");
            draw->AddText(g_fonts.bold, font_size, ImVec2(cur_x, origin_bar.y + (bar_h - sz_join_lbl.y) * 0.5f), muted, "JOIN:");
            cur_x += sz_join_lbl.x + S(6.0f);

            const bool just_copied = (ImGui::GetTime() < g_state.copied_until);
            const char* code_display = just_copied ? "[COPIED]" : stats.invite_code.c_str();
            const ImU32 code_col = just_copied ? good : white;

            const auto sz_code = g_fonts.bold->CalcTextSizeA(font_size, FLT_MAX, 0.0f, code_display);

            // Interactive hit-test region over join code
            ImGui::SetCursorScreenPos(ImVec2(cur_x, origin_bar.y));
            ImGui::PushID("btn_copy_join_code");
            const bool code_clicked = ImGui::InvisibleButton("##copy_code", ImVec2(sz_code.x, bar_h));
            const bool code_hovered = ImGui::IsItemHovered();
            if (code_hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Click to copy Join Code: %s", stats.invite_code.c_str());
            }
            if (code_clicked) {
                ImGui::SetClipboardText(stats.invite_code.c_str());
                g_state.copied_until = ImGui::GetTime() + 2.5;
                show_notification("Copied Join Code: " + stats.invite_code, 2.5, good);
            }
            ImGui::PopID();

            draw->AddText(g_fonts.bold, font_size, ImVec2(cur_x, origin_bar.y + (bar_h - sz_code.y) * 0.5f), code_col, code_display);
            cur_x += sz_code.x;
        }

        ImGui::SetCursorScreenPos(ImVec2(cur_x, origin_bar.y));
    }

    // Right Side: Server Control Block [ ▶ | ↻ | ■ ] (+ Clear Error if applicable)
    const float seg_w = S(38.0f);
    const float total_block_w = seg_w * 3.0f;
    const float clear_w = S(110.0f);
    const float clear_gap = S(10.0f);

    float right_elements_w = total_block_w;
    if (stats.state == ServerState::Error) {
        right_elements_w += clear_w + clear_gap;
    }

    const float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w > right_elements_w) {
        ImGui::SameLine(ImGui::GetCursorPosX() + (avail_w - right_elements_w));
    } else {
        ImGui::SameLine(0, S(10));
    }

    ImGui::SetCursorPosY(cur_bar_y);

    if (stats.state == ServerState::Error) {
        if (rough_button("CLEAR ERROR", ImVec2(clear_w, S(30)))) {
            ServerEngine::instance().clear_error();
        }
        ImGui::SameLine(0, clear_gap);
    }

    draw_server_control_block(stats);
}

struct SettingsForm {
    char name[128]{};
    char password[64]{};
    char welcome[256]{};
    int max_players{16};
    bool listed{true};
    bool autostart{false};
    bool auto_update{true};
    bool activity_log{true};

    int port{27015};
    int query_port{27016};
    char steam_token[128]{};
    bool use_steam_relay{true};
    int send_rate{900};
    int crowd_budget{600};

    bool boosts{true};
    bool no_bail{true};
    bool noclip{true};
    bool parties{true};
    int party_size{8};
    bool voice_chat{true};
    float voice_range{300.0f};
    int object_placement_idx{0};
    int object_limit{100};

    int speed_check_idx{1};
    int score_check_idx{1};
    bool enforce_tuning{true};
    float bone_scale_limit{2.0f};
    bool global_bans{true};

    bool initialized{false};
};

static SettingsForm s_form;

static void sync_form_from_config(SettingsForm& form, const ServerConfig& cfg) {
    strncpy_s(form.name, cfg.name.c_str(), sizeof(form.name) - 1);
    strncpy_s(form.password, cfg.password.c_str(), sizeof(form.password) - 1);
    strncpy_s(form.welcome, cfg.welcome.c_str(), sizeof(form.welcome) - 1);
    form.max_players = static_cast<int>(cfg.max_players);
    form.listed = cfg.listed;
    form.autostart = cfg.autostart;
    form.auto_update = cfg.auto_update;
    form.activity_log = cfg.activity_log;

    form.port = static_cast<int>(cfg.port);
    form.query_port = static_cast<int>(cfg.query_port);
    strncpy_s(form.steam_token, cfg.steam_token.c_str(), sizeof(form.steam_token) - 1);
    form.use_steam_relay = cfg.use_steam_relay;
    form.send_rate = static_cast<int>(cfg.send_rate);
    form.crowd_budget = static_cast<int>(cfg.crowd_budget);

    form.boosts = cfg.boosts;
    form.no_bail = cfg.no_bail;
    form.noclip = cfg.noclip;
    form.parties = cfg.parties;
    form.party_size = static_cast<int>(cfg.party_size);
    form.voice_chat = cfg.voice_chat;
    form.voice_range = cfg.voice_range;
    form.object_placement_idx = cfg.object_placement == ObjectPlacement::everyone ? 0 : (cfg.object_placement == ObjectPlacement::host_only ? 1 : 2);
    form.object_limit = static_cast<int>(cfg.object_limit);

    form.speed_check_idx = cfg.speed_check == "off" ? 0 : (cfg.speed_check == "kick" ? 2 : 1);
    form.score_check_idx = cfg.score_check == "off" ? 0 : (cfg.score_check == "kick" ? 2 : 1);
    form.enforce_tuning = cfg.enforce_tuning;
    form.bone_scale_limit = cfg.bone_scale_limit;
    form.global_bans = cfg.global_bans;

    form.initialized = true;
}

void draw_settings_tab(ServerStatusSnapshot& stats) {
    (void)stats;
    section("SERVER CONFIGURATION & PREFERENCES");

    if (!s_form.initialized) {
        sync_form_from_config(s_form, ServerEngine::instance().config());
    }

    // Top Action Bar
    if (rough_button("APPLY & SAVE SETTINGS", ImVec2(S(220), S(34)))) {
        auto& cfg = ServerEngine::instance().config();
        cfg.name = s_form.name;
        cfg.password = s_form.password;
        cfg.welcome = s_form.welcome;
        cfg.max_players = static_cast<unsigned>(std::clamp(s_form.max_players, 1, 249));
        cfg.tps = dedicated_tps;
        cfg.listed = s_form.listed;
        cfg.autostart = s_form.autostart;
        cfg.auto_update = s_form.auto_update;
        cfg.activity_log = s_form.activity_log;

        cfg.port = static_cast<uint16_t>(std::clamp(s_form.port, 1024, 65535));
        cfg.query_port = static_cast<uint16_t>(std::clamp(s_form.query_port, 1024, 65535));
        cfg.steam_token = s_form.steam_token;
        cfg.use_steam_relay = s_form.use_steam_relay;
        cfg.send_rate = static_cast<unsigned>(std::clamp(s_form.send_rate, 128, 16384));
        cfg.crowd_budget = static_cast<unsigned>(std::clamp(s_form.crowd_budget, 0, 20000));

        cfg.boosts = s_form.boosts;
        cfg.no_bail = s_form.no_bail;
        cfg.noclip = s_form.noclip;
        cfg.parties = s_form.parties;
        cfg.party_size = static_cast<unsigned>(std::clamp(s_form.party_size, 2, 8));
        cfg.voice_chat = s_form.voice_chat;
        cfg.voice_range = std::clamp(s_form.voice_range, 50.0f, 1000.0f);
        cfg.object_placement = s_form.object_placement_idx == 0 ? ObjectPlacement::everyone : (s_form.object_placement_idx == 1 ? ObjectPlacement::host_only : ObjectPlacement::nobody);
        cfg.object_limit = static_cast<unsigned>(std::clamp(s_form.object_limit, 0, 500));

        cfg.speed_check = s_form.speed_check_idx == 0 ? "off" : (s_form.speed_check_idx == 2 ? "kick" : "warn");
        cfg.score_check = s_form.score_check_idx == 0 ? "off" : (s_form.score_check_idx == 2 ? "kick" : "warn");
        cfg.enforce_tuning = s_form.enforce_tuning;
        cfg.bone_scale_limit = std::clamp(s_form.bone_scale_limit, 1.0f, 8.0f);
        cfg.global_bans = s_form.global_bans;

        try {
            save_config(cfg);
            if (ServerEngine::instance().state() == ServerState::Running) {
                ServerEngine::instance().queue_command("name " + cfg.name);
                ServerEngine::instance().queue_command(cfg.password.empty() ? "password off" : ("password " + cfg.password));
                ServerEngine::instance().queue_command(cfg.welcome.empty() ? "welcome off" : ("welcome " + cfg.welcome));
                ServerEngine::instance().queue_command(std::string("listed ") + (cfg.listed ? "on" : "off"));
                ServerEngine::instance().queue_command(std::string("boosts ") + (cfg.boosts ? "on" : "off"));
                ServerEngine::instance().queue_command(std::string("nobail ") + (cfg.no_bail ? "on" : "off"));
                ServerEngine::instance().queue_command(std::string("noclip ") + (cfg.noclip ? "on" : "off"));
                ServerEngine::instance().queue_command(std::string("tuning ") + (cfg.enforce_tuning ? "on" : "off"));
                ServerEngine::instance().queue_command(std::string("voice ") + (cfg.voice_chat ? "on" : "off"));
                ServerEngine::instance().queue_command("voice-range " + std::to_string(static_cast<int>(cfg.voice_range)));
                ServerEngine::instance().queue_command(std::string("placement ") + (cfg.object_placement == ObjectPlacement::everyone ? "everyone" : (cfg.object_placement == ObjectPlacement::host_only ? "admins" : "nobody")));
                ServerEngine::instance().queue_command("speed-check " + cfg.speed_check);
                ServerEngine::instance().queue_command("score-check " + cfg.score_check);
                ServerEngine::instance().queue_command("bone-scale " + std::to_string(static_cast<int>(cfg.bone_scale_limit)));
            }
            show_notification("Configuration saved to ReSkateServer.json & synchronized.", 4.0, good);
        } catch (const std::exception& e) {
            show_notification(std::string("Save failed: ") + e.what(), 4.0, danger);
        }
    }
    ImGui::SameLine(0, S(10));
    if (rough_button("RELOAD FROM DISK", ImVec2(S(180), S(34)))) {
        try {
            auto cfg_path = ServerEngine::instance().folder() / "ReSkateServer.json";
            auto loaded = load_config(cfg_path);
            ServerEngine::instance().config() = loaded;
            sync_form_from_config(s_form, loaded);
            show_notification("Reloaded configuration from ReSkateServer.json.", 3.5, blue);
        } catch (const std::exception& e) {
            show_notification(std::string("Reload failed: ") + e.what(), 4.0, danger);
        }
    }
    ImGui::SameLine(0, S(10));
    if (rough_button("OPEN CONFIG FILE", ImVec2(S(170), S(34)))) {
        auto cfg_path = (ServerEngine::instance().folder() / "ReSkateServer.json").string();
        ShellExecuteA(nullptr, "open", cfg_path.c_str(), nullptr, nullptr, SW_SHOW);
    }

    ImGui::Dummy(ImVec2(0, S(8)));

    ImGui::Columns(2, "settings_cols", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.50f);

    // Column 1: Server Identity
    begin_card("card_cfg_server", "SERVER IDENTITY SETTINGS", "Configure your public server profile");
    ImGui::PushItemWidth(S(360));
    ImGui::InputText("Server Name", s_form.name, sizeof(s_form.name));
    ImGui::InputTextWithHint("Password", "Leave empty for public server", s_form.password, sizeof(s_form.password));
    ImGui::InputTextWithHint("Welcome Message", "Sent to players when joining...", s_form.welcome, sizeof(s_form.welcome));
    ImGui::SliderInt("Max Players", &s_form.max_players, 1, 249);
    ImGui::Checkbox("List in Public Server Browser", &s_form.listed);
    ImGui::Checkbox("Autostart Server with GUI", &s_form.autostart);
    ImGui::Checkbox("Auto-Update New Releases", &s_form.auto_update);
    ImGui::Checkbox("Activity Log in Console", &s_form.activity_log);
    ImGui::PopItemWidth();
    end_card();

    // Column 1: Anti-Cheat & Rules
    begin_card("card_cfg_anticheat", "ANTI-CHEAT & PHYSICS ENFORCEMENT", "Authoritative verification rules");
    ImGui::PushItemWidth(S(360));
    const char* check_modes[] = { "Disabled (Off)", "Warn Admins", "Kick Player" };
    ImGui::Combo("Speed-Hack Action", &s_form.speed_check_idx, check_modes, IM_ARRAYSIZE(check_modes));
    ImGui::Combo("Scoring Mod Action", &s_form.score_check_idx, check_modes, IM_ARRAYSIZE(check_modes));
    ImGui::Checkbox("Enforce Server Physics Tuning", &s_form.enforce_tuning);
    ImGui::SliderFloat("Bone Scale Limit", &s_form.bone_scale_limit, 1.0f, 8.0f, "%.1fx");
    ImGui::Checkbox("Enforce Global Ban List", &s_form.global_bans);
    ImGui::PopItemWidth();
    end_card();

    ImGui::NextColumn();

    // Column 2: Network & Ports
    begin_card("card_cfg_network", "NETWORK & PORT BINDINGS", "Steam SDR UDP ports and token");
    ImGui::PushItemWidth(S(360));
    ImGui::InputInt("Game Port (UDP)", &s_form.port);
    ImGui::InputInt("Query Port (UDP)", &s_form.query_port);
    ImGui::InputText("Steam GSLT Token", s_form.steam_token, sizeof(s_form.steam_token));
    ImGui::Checkbox("Route via Steam SDR Relays", &s_form.use_steam_relay);
    ImGui::SliderInt("Send Rate (KB/s)", &s_form.send_rate, 128, 4096);
    ImGui::SliderInt("Crowd Pose Budget", &s_form.crowd_budget, 300, 2000);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::Text("Target Tick Rate: ");
    ImGui::SameLine();
    ImGui::PopStyleColor();
    badge("20 TPS (Dedicated Standard)", good, black);
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("Fixed at 20 TPS by ReSkate dedicated server protocol specification for network bandwidth and crowd pose stability.");
    ImGui::PopStyleColor();

    ImGui::PopItemWidth();
    end_card();

    // Column 2: Gameplay Permissions
    begin_card("card_cfg_gameplay", "PLAYER PERMISSIONS & PROXIMITY VOICE", "Allowed player features");
    ImGui::PushItemWidth(S(360));
    ImGui::Checkbox("Allow Proximity Voice Chat", &s_form.voice_chat);
    if (s_form.voice_chat) {
        ImGui::SliderFloat("Voice Range (Meters)", &s_form.voice_range, 50.0f, 1000.0f, "%.0f m");
    }
    ImGui::Checkbox("Allow Ramp Boosts", &s_form.boosts);
    ImGui::Checkbox("Allow NoBail Mode", &s_form.no_bail);
    ImGui::Checkbox("Allow NoClip Free-Cam", &s_form.noclip);
    ImGui::Checkbox("Allow Player Parties", &s_form.parties);
    if (s_form.parties) {
        ImGui::SliderInt("Max Party Size", &s_form.party_size, 2, 8);
    }
    const char* placement_modes[] = { "Everyone", "Admins Only", "Nobody" };
    ImGui::Combo("Object Placement", &s_form.object_placement_idx, placement_modes, IM_ARRAYSIZE(placement_modes));
    ImGui::SliderInt("Object Limit Per Player", &s_form.object_limit, 0, 500);
    ImGui::PopItemWidth();
    end_card();

    // Updates & Releases Card
    begin_card("card_cfg_updates", "UPDATES & RELEASES", "Check and install official releases");
    stat_row("Installed Server Build", "ReSkate v2.0.3 (Protocol " + std::to_string(multiplayer::protocol_version) + ")", good);
    const auto update = ServerEngine::instance().update_info();
    stat_row("Release Status", update.message,
             update.status == ServerEngine::UpdateInfo::Status::Available ? warning : (update.status == ServerEngine::UpdateInfo::Status::Error ? danger : good));

    ImGui::Spacing();
    if (rough_button("CHECK FOR UPDATES", ImVec2(S(180), S(30)))) {
        ServerEngine::instance().check_for_updates_async();
        show_notification("Checking GitHub for latest release...", 3.0, blue);
    }
    if (update.available) {
        ImGui::SameLine(0, S(10));
        if (rough_button("INSTALL UPDATE", ImVec2(S(160), S(30)))) {
            ServerEngine::instance().install_update_async();
            show_notification("Installing update from GitHub...", 4.0, warning);
        }
    }
    end_card();

    // About & Credits Card
    begin_card("card_cfg_about", "ABOUT DEDICATED SERVER GUI", "System credits and build info");
    stat_row("Dedicated Server GUI", "Made with love by wxndr", good);
    stat_row("UI Framework", "Dear ImGui (DirectX 12 / Win32)", good);
    end_card();

    ImGui::Columns(1);
}

static void draw_blue_heart(ImDrawList* draw, ImVec2 center, float r, ImU32 color) {
    const float w = r;
    const float h = r * 0.9f;
    const ImVec2 bottom_tip(center.x, center.y + h * 0.75f);
    draw->AddCircleFilled(ImVec2(center.x - w * 0.38f, center.y - h * 0.2f), w * 0.44f, color, 12);
    draw->AddCircleFilled(ImVec2(center.x + w * 0.38f, center.y - h * 0.2f), w * 0.44f, color, 12);
    draw->AddTriangleFilled(
        ImVec2(center.x - w * 0.76f, center.y - h * 0.1f),
        ImVec2(center.x + w * 0.76f, center.y - h * 0.1f),
        bottom_tip,
        color
    );
}

// --- Main Frame Render ---
void render_frame() {
    auto stats = ServerEngine::instance().status_snapshot();
    const auto players = ServerEngine::instance().player_list();

    const auto io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("MainServerWindow", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    auto* draw = ImGui::GetWindowDrawList();
    const float sidebar_width = S(210.0f);

    // Sidebar Background (Ultra-dark slate)
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(sidebar_width, size.y), IM_COL32(10, 10, 12, 255));

    // Brushed Wordmark "RESKATE" rotated -4.0 deg with shadow
    {
        const int start = draw->VtxBuffer.Size;
        const ImVec2 at(S(20), S(16));
        draw->AddText(g_fonts.title, S(38), ImVec2(at.x + S(2), at.y + S(2)), IM_COL32(0, 0, 0, 160), "RESKATE");
        draw->AddText(g_fonts.title, S(38), at, white, "RESKATE");
        const auto extent = g_fonts.title->CalcTextSizeA(S(38), FLT_MAX, 0, "RESKATE");
        rotate_since(draw, start, -4.0f, ImVec2(at.x + extent.x * 0.5f, at.y + extent.y * 0.5f));
    }

    draw->AddText(g_fonts.caption, S(11), ImVec2(S(22), S(66)), muted, "SERVER MANAGEMENT");

    // Sidebar Navigation Tabs
    std::vector<std::pair<const char*, Tab>> tabs = {
        {"CONSOLE", Tab::Console},
        {"OVERVIEW", Tab::Overview},
        {"PLAYERS", Tab::Players},
        {"WORLD", Tab::World},
        {"MODERATION", Tab::Moderation},
        {"TELEMETRY", Tab::Telemetry},
        {"SETTINGS", Tab::Settings}
    };

    ImGui::SetCursorPos(ImVec2(S(12), S(96)));
    ImGui::BeginChild("navigation", ImVec2(sidebar_width - S(24), size.y - S(170)), ImGuiChildFlags_None);

    const float tab_h = S(40);
    for (size_t i = 0; i < tabs.size(); ++i) {
        const auto& [name, tab] = tabs[i];
        const bool selected = g_state.current_tab == tab;
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::InvisibleButton("##nav", ImVec2(sidebar_width - S(24), tab_h)) && !selected) {
            g_state.current_tab = tab;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const ImVec2 end(at.x + sidebar_width - S(24), at.y + tab_h - S(4));
        if (selected || hovered) {
            rough_rect(draw, at, end, selected ? blue : tile_grey, static_cast<unsigned>(i + 5), g_scale);
        }

        draw->AddText(g_fonts.bold, S(15), ImVec2(at.x + S(14), at.y + (tab_h - S(4) - S(16)) * 0.5f),
                      selected ? black : white, name);

        // Badge pill in tab if players connected
        if (tab == Tab::Players && stats.player_count > 0) {
            const std::string cnt = std::to_string(stats.player_count);
            const auto pill_sz = g_fonts.caption->CalcTextSizeA(S(11), FLT_MAX, 0, cnt.c_str());
            const ImVec2 ppos(end.x - S(12) - pill_sz.x - S(8), at.y + (tab_h - S(4) - S(18)) * 0.5f);
            draw->AddRectFilled(ppos, ImVec2(ppos.x + pill_sz.x + S(8), ppos.y + S(18)),
                                selected ? black : good, S(3));
            draw->AddText(g_fonts.caption, S(11), ImVec2(ppos.x + S(4), ppos.y + S(2)),
                          selected ? white : black, cnt.c_str());
        }
    }
    ImGui::EndChild();

    // Bottom Connected Status Element (Solid Red for Offline, Solid Green for Online)
    {
        const float status_w = sidebar_width - S(24.0f);
        const float status_h = S(26.0f);
        const ImVec2 status_pos(S(12), size.y - S(52));

        ImU32 bg_col = danger; // Full Red for Offline
        ImU32 text_col = white;
        const char* status_label = "OFFLINE";

        if (stats.state == ServerState::Running) {
            bg_col = good; // Full Green for Online
            text_col = black;
            status_label = "ONLINE";
        } else if (stats.state == ServerState::Starting) {
            bg_col = warning; // Full Amber for Starting
            text_col = black;
            status_label = "STARTING...";
        } else if (stats.state == ServerState::Stopping) {
            bg_col = warning; // Full Amber for Stopping
            text_col = black;
            status_label = "STOPPING...";
        } else if (stats.state == ServerState::Error) {
            bg_col = danger; // Darker Red for Error
            text_col = white;
            status_label = "SERVER ERROR";
        }

        // 1. Draw solid rounded status badge
        draw->AddRectFilled(status_pos, ImVec2(status_pos.x + status_w, status_pos.y + status_h), bg_col, S(4.0f));

        // 2. Centered Status Text (Clean and perfectly centered)
        const auto sz_txt = g_fonts.bold->CalcTextSizeA(S(12.0f), FLT_MAX, 0, status_label);
        const float text_x = status_pos.x + (status_w - sz_txt.x) * 0.5f;
        const float text_y = status_pos.y + (status_h - sz_txt.y) * 0.5f;
        draw->AddText(g_fonts.bold, S(12.0f), ImVec2(text_x, text_y), text_col, status_label);
    }

    // Sidebar Footer (Blue heart + Interactive author profile link)
    {
        const float heart_x = S(14.0f);
        const float heart_y = size.y - S(14.0f);
        draw_blue_heart(draw, ImVec2(heart_x + S(4.0f), heart_y), S(4.5f), skate_theme::blue);

        const ImVec2 prefix_pos(heart_x + S(14.0f), size.y - S(20.0f));
        draw->AddText(g_fonts.caption, S(10.5f), prefix_pos, IM_COL32(130, 130, 130, 180), "GUI made with love by ");
        const auto prefix_sz = g_fonts.caption->CalcTextSizeA(S(10.5f), FLT_MAX, 0, "GUI made with love by ");
        const ImVec2 name_pos(prefix_pos.x + prefix_sz.x, prefix_pos.y);
        const auto name_sz = g_fonts.caption->CalcTextSizeA(S(10.5f), FLT_MAX, 0, "wxndr");

        ImGui::SetCursorScreenPos(name_pos);
        ImGui::PushID("sidebar_author_link");
        if (ImGui::InvisibleButton("##wxndr_link", name_sz)) {
            ShellExecuteW(nullptr, L"open", L"https://steamcommunity.com/id/1wxndr/", nullptr, nullptr, SW_SHOWNORMAL);
        }
        const bool link_hovered = ImGui::IsItemHovered();
        if (link_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open wxndr's Steam Profile (https://steamcommunity.com/id/1wxndr/)");
        }
        ImGui::PopID();

        const ImU32 name_col = link_hovered ? skate_theme::blue : IM_COL32(190, 190, 190, 220);
        draw->AddText(g_fonts.caption, S(10.5f), name_pos, name_col, "wxndr");
        if (link_hovered) {
            draw->AddLine(ImVec2(name_pos.x, name_pos.y + name_sz.y), ImVec2(name_pos.x + name_sz.x, name_pos.y + name_sz.y), name_col, 1.0f);
        }
    }

    // Main Header: Brushed Title rotated -3.0 deg without subtitle text
    const char* active_tab_name = "CONSOLE";
    switch (g_state.current_tab) {
    case Tab::Console: active_tab_name = "CONSOLE"; break;
    case Tab::Overview: active_tab_name = "OVERVIEW"; break;
    case Tab::Players: active_tab_name = "PLAYERS"; break;
    case Tab::World: active_tab_name = "WORLD"; break;
    case Tab::Moderation: active_tab_name = "MODERATION"; break;
    case Tab::Telemetry: active_tab_name = "TELEMETRY"; break;
    case Tab::Settings: active_tab_name = "SETTINGS"; break;
    default: break;
    }

    {
        const int start = draw->VtxBuffer.Size;
        const ImVec2 at(sidebar_width + S(24), S(14));
        draw->AddText(g_fonts.title, S(40), ImVec2(at.x + S(2), at.y + S(2)), IM_COL32(0, 0, 0, 160), active_tab_name);
        draw->AddText(g_fonts.title, S(40), at, white, active_tab_name);
        const auto extent = g_fonts.title->CalcTextSizeA(S(40), FLT_MAX, 0, active_tab_name);
        rotate_since(draw, start, -3.0f, ImVec2(at.x + extent.x * 0.5f, at.y + extent.y * 0.5f));
    }

    // Top Right Window Controls (Minimize, Maximize / Restore, Close)
    const bool is_maximized = IsZoomed(g_window);
    ImGui::SetCursorPos(ImVec2(size.x - S(112), S(12)));
    if (ImGui::Button("_", ImVec2(S(28), S(28)))) {
        ShowWindow(g_window, SW_MINIMIZE);
    }
    ImGui::SameLine();
    if (ImGui::Button(is_maximized ? "=" : "[ ]", ImVec2(S(28), S(28)))) {
        ShowWindow(g_window, is_maximized ? SW_RESTORE : SW_MAXIMIZE);
    }
    ImGui::SameLine();
    if (ImGui::Button("X", ImVec2(S(28), S(28)))) {
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    }

    // Main Content Area (Lighter grey background with internal safe padding so "Network" is never on the border)
    ImGui::SetCursorPos(ImVec2(sidebar_width + S(24), S(76)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    ImGuiWindowFlags content_flags = ImGuiWindowFlags_None;
    if (g_state.current_tab == Tab::Console) {
        content_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }
    ImGui::BeginChild("tab_content", ImVec2(size.x - sidebar_width - S(48), size.y - S(96)), ImGuiChildFlags_AlwaysUseWindowPadding, content_flags);
    ImGui::PopStyleVar();

    switch (g_state.current_tab) {
    case Tab::Console: draw_console_tab(stats); break;
    case Tab::Overview: draw_overview_tab(stats); break;
    case Tab::Players: draw_players_tab(players); break;
    case Tab::World: draw_world_tab(stats); break;
    case Tab::Moderation: draw_moderation_tab(); break;
    case Tab::Telemetry: draw_telemetry_tab(); break;
    case Tab::Settings: draw_settings_tab(stats); break;
    default: break;
    }

    ImGui::EndChild();

    // Top-Right Stacking Floating Toast Notifications (latest at top, stacking down)
    const double now = ImGui::GetTime();

    // Clean up expired notifications
    for (auto it = g_state.notifications.begin(); it != g_state.notifications.end();) {
        if (now >= it->feedback_until) {
            it = g_state.notifications.erase(it);
        } else {
            ++it;
        }
    }

    if (!g_state.notifications.empty()) {
        auto* fg = ImGui::GetForegroundDrawList();
        float current_y = S(48.0f);
        const float toast_h = S(28.0f);
        const float gap_y = S(6.0f);

        for (auto& toast : g_state.notifications) {
            const double time_left = toast.feedback_until - now;
            const float alpha = static_cast<float>(std::clamp(time_left / 0.4, 0.0, 1.0));

            const auto txt_sz = g_fonts.bold->CalcTextSizeA(S(12.0f), FLT_MAX, 0, toast.message.c_str());
            const float toast_w = txt_sz.x + S(28.0f);
            const ImVec2 pos(size.x - toast_w - S(20.0f), current_y);

            // Click to dismiss early
            if (io.MouseClicked[0]) {
                const auto m = io.MousePos;
                if (m.x >= pos.x && m.x <= pos.x + toast_w && m.y >= pos.y && m.y <= pos.y + toast_h) {
                    toast.feedback_until = 0.0;
                }
            }

            // Toast body (clean sharp rectangle, no border outline, no drop shadow)
            fg->AddRectFilled(pos, ImVec2(pos.x + toast_w, pos.y + toast_h),
                              IM_COL32(26, 28, 34, static_cast<int>(245 * alpha)), 0.0f);

            // Left status accent bar (flush vertical rectangle stripe)
            ImU32 accent = toast.accent != 0 ? toast.accent : blue;
            if (toast.accent == 0 && (toast.message.find("fail") != std::string::npos ||
                                      toast.message.find("Error") != std::string::npos ||
                                      toast.message.find("failed") != std::string::npos)) {
                accent = danger;
            }
            const ImVec4 col = ImGui::ColorConvertU32ToFloat4(accent);
            fg->AddRectFilled(pos, ImVec2(pos.x + S(4.0f), pos.y + toast_h),
                              ImColor(col.x, col.y, col.z, col.w * alpha), 0.0f);

            // Notification message text
            fg->AddText(g_fonts.bold, S(12.0f),
                        ImVec2(pos.x + S(14.0f), pos.y + (toast_h - txt_sz.y) * 0.5f),
                        ImColor(245, 245, 245, static_cast<int>(255 * alpha)),
                        toast.message.c_str());

            current_y += toast_h + gap_y;
        }
    }

    // Player Moderation Confirmation Modal (Kick / Ban)
    if (g_state.show_player_modal) {
        ImGui::OpenPopup("Confirm Player Action###player_action_modal");
        g_state.show_player_modal = false;
    }

    if (ImGui::BeginPopupModal("Confirm Player Action###player_action_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* action_name = g_state.is_kick_modal ? "Kick Player" : "Ban Player";
        ImGui::PushFont(g_fonts.heading);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", action_name);
        ImGui::PopFont();
        ImGui::Spacing();

        ImGui::Text("Target: %s (SteamID64: %llu)", g_state.target_player_name.c_str(), g_state.target_player_id);
        ImGui::Spacing();

        ImGui::PushItemWidth(S(340.0f));
        ImGui::InputTextWithHint("Reason", "Enter reason for moderation...", g_state.target_action_reason, sizeof(g_state.target_action_reason));
        ImGui::PopItemWidth();
        ImGui::Spacing();

        const float m_btn_w = S(150.0f);
        const float m_btn_h = S(32.0f);
        const auto m_btn_pos = ImGui::GetCursorScreenPos();
        const bool confirm_clicked = ImGui::InvisibleButton("##confirm_mod_btn", ImVec2(m_btn_w, m_btn_h));
        const bool confirm_hovered = ImGui::IsItemHovered();
        if (confirm_hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        auto* m_draw = ImGui::GetWindowDrawList();

        const ImU32 m_btn_bg = g_state.is_kick_modal
            ? (confirm_hovered ? IM_COL32(255, 185, 45, 255) : IM_COL32(235, 155, 30, 255))
            : (confirm_hovered ? IM_COL32(250, 70, 70, 255) : IM_COL32(225, 45, 45, 255));
        const ImU32 m_btn_fg = g_state.is_kick_modal ? black : white;
        const char* m_btn_txt = g_state.is_kick_modal ? "CONFIRM KICK" : "CONFIRM BAN";

        m_draw->AddRectFilled(m_btn_pos, ImVec2(m_btn_pos.x + m_btn_w, m_btn_pos.y + m_btn_h), m_btn_bg, S(4.0f));
        const auto sz_mtxt = g_fonts.bold->CalcTextSizeA(S(13.0f), FLT_MAX, 0.0f, m_btn_txt);
        m_draw->AddText(g_fonts.bold, S(13.0f),
                        ImVec2(m_btn_pos.x + (m_btn_w - sz_mtxt.x) * 0.5f, m_btn_pos.y + (m_btn_h - sz_mtxt.y) * 0.5f),
                        m_btn_fg, m_btn_txt);

        if (confirm_clicked) {
            std::string r = g_state.target_action_reason[0] ? g_state.target_action_reason : (g_state.is_kick_modal ? "Kicked from GUI" : "Banned from GUI");
            if (g_state.is_kick_modal) {
                ServerEngine::instance().queue_command("kick " + std::to_string(g_state.target_player_id) + " \"" + r + "\"");
                show_notification("Kicked player: " + g_state.target_player_name, 3.5, warning);
            } else {
                ServerEngine::instance().queue_command("ban " + std::to_string(g_state.target_player_id) + " \"" + r + "\"");
                show_notification("Banned player: " + g_state.target_player_name, 4.0, danger);
            }
            g_state.target_action_reason[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, S(10));
        if (rough_button("CANCEL", ImVec2(S(100), m_btn_h))) {
            g_state.target_action_reason[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace

int run_gui(HINSTANCE instance, int cmd_show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);

    constexpr float design_w = 1260.0f;
    constexpr float design_h = 860.0f;
    const int max_w = static_cast<int>((work.right - work.left) * 0.95f);
    const int max_h = static_cast<int>((work.bottom - work.top) * 0.95f);
    const int width = std::min(static_cast<int>(S(design_w)), max_w);
    const int height = std::min(static_cast<int>(S(design_h)), max_h);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ReSkateServerGUI";
    RegisterClassExW(&wc);

    const HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, wc.lpszClassName, L"ReSkate Dedicated Server",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, nullptr
    );

    if (!window) return 1;
    g_window = window;

    const DWORD corners = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(window, &margins);
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;

    // Detect server root folder
    std::wstring mod_path(32768, L'\0');
    const auto len = GetModuleFileNameW(nullptr, mod_path.data(), static_cast<DWORD>(mod_path.size()));
    mod_path.resize(len);
    const fs::path root_folder = fs::path(mod_path).parent_path();

    load_fonts(root_folder);
    apply_style();

    ImGui_ImplWin32_Init(window);
    if (!g_renderer.init(window)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        MessageBoxW(nullptr, L"Direct3D 12 could not be initialized for the server GUI.", L"DirectX Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, (cmd_show == SW_HIDE || cmd_show == 0) ? SW_NORMAL : cmd_show);
    UpdateWindow(window);

    // Initialize server environment and inspect configuration
    const bool env_ok = ServerEngine::instance().load_environment(root_folder, root_folder / "ReSkateServer.json", true);
    if (env_ok && ServerEngine::instance().config().autostart) {
        ServerEngine::instance().start_server_async();
    }

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (IsIconic(window)) {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        render_frame();

        ImGui::Render();
        g_renderer.render();
    }

    ServerEngine::instance().stop();
    g_renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(window);
    return 0;
}

} // namespace dingosdk::server_gui
