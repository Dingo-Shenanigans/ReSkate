#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef small
#undef small
#endif
#include <imgui.h>
#include <string>
#include <vector>

namespace dingosdk::server_gui {

struct GuiFonts {
    ImFont* body{nullptr};
    ImFont* caption{nullptr};
    ImFont* bold{nullptr};
    ImFont* heading{nullptr};
    ImFont* title{nullptr};
    ImFont* mono{nullptr};
};

enum class Tab {
    Console,
    Overview,
    Players,
    World,
    Moderation,
    Telemetry,
    Settings,
    Count
};

struct ToastNotification {
    std::string message;
    double feedback_until{0.0};
    ImU32 accent{0};
};

struct GuiState {
    Tab current_tab{Tab::Console};
    int moderation_subtab{0};

    // Console state
    char console_input[512]{};
    std::vector<std::string> command_history;
    int history_index{-1};
    bool auto_scroll{true};
    std::string console_filter;

    // Moderation form
    char mod_steam_id[64]{};
    int mod_type_index{0}; // 0 = Voice, 1 = Text, 2 = Both, 3 = Ban
    int mod_duration{60};  // minutes
    char mod_reason[128]{"Violating server guidelines"};

    // World & Map
    char custom_map[128]{};

    // Players Search Filter
    char player_search[64]{};

    // Telemetry & Performance Monitor state
    int telemetry_time_scale{1}; // 0 = 60s, 1 = 5m, 2 = 15m, 3 = 1h
    int telemetry_metric{0};     // 0 = TPS & Tick Health, 1 = Latency & Frame Time, 2 = Memory & Heap

    // Moderation Confirmation Modal State
    bool show_player_modal{false};
    bool is_kick_modal{false};
    std::uint64_t target_player_id{0};
    std::string target_player_name;
    char target_action_reason[128]{"Violating server guidelines"};

    // Notification feedback banner & stacking queue
    std::string feedback_text;
    double feedback_until{0.0};
    std::vector<ToastNotification> notifications;

    // Copy notice
    double copied_until{0.0};
};

int run_gui(HINSTANCE instance, int cmd_show);

} // namespace dingosdk::server_gui

