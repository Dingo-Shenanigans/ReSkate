#pragma once

#include "overlay.h"
#include "park_editor.h"
#include "Engine/Scripting/custom_scripts.h"
#include <imgui.h>
#include <optional>
#include <map>

namespace dingosdk::overlay {
struct AtmosphereEdit {
    AtmosphereValue value;
    std::optional<AtmosphereValue> pending;
    bool waiting{}, editing{}, dirty{};
    double deadline{};
};
struct SkateMenu {
    int page = 0;
    int multiplayer_tab = 0;
    bool multiplayer_session_seen{};
    bool multiplayer_host_seeded{};  // host drafts filled from the saved host settings
    int multiplayer_capacity = multiplayer_lobby_player_limit;
    unsigned multiplayer_tps = multiplayer_default_tps;
    int multiplayer_visibility = 1;
    int multiplayer_lobbies_frame = -2;  // last frame the Lobbies tab drew
    bool multiplayer_password_enabled = false;
    bool multiplayer_same_map_only = false;
    bool multiplayer_dedicated_only = false;
    std::array<char, 321> server_command{}; // an admin's command for the dedicated server
    std::map<std::string, std::pair<bool, double>> map_pool_pending; // asset -> {ticked, until}, until the server agrees
    std::optional<int> map_rotation_pending;                         // the rotation slider, likewise
    double map_rotation_until{};
    bool ban_confirm_on_server{};           // the Ban popup bans from a dedicated server
    int multiplayer_lobby_sort = 0;
    float multiplayer_code_height = 0;  // what the join-by-code card took last frame
    // Bans: the manual add form, and the player a Ban button is asking about.
    std::array<char, 24> ban_id{};
    std::array<char, 65> ban_name{};
    std::uint64_t ban_confirm_id{};
    std::string ban_confirm_name;
    bool ban_confirm_requested{};
    bool multiplayer_password_popup_requested = false, multiplayer_password_show = false;
    std::optional<MultiplayerLobby> multiplayer_password_lobby;
    std::string multiplayer_password_code, multiplayer_password_error;
    std::array<char,65> multiplayer_host_password{}, multiplayer_join_password{};
    std::array<char,96> multiplayer_join_code{};
    std::array<char,128> multiplayer_lobby_search{};
    std::array<char,129> multiplayer_lobby_name{};
    MultiplayerDistances multiplayer_distance_draft, multiplayer_distance_applied;
    bool multiplayer_distance_initialized{}, multiplayer_distance_hosting{};
    int voice_bind_capture{};
    ControllerComboCapture voice_controller_capture;
    double voice_capture_until{}, voice_pending_until{};
    std::optional<VoiceSettings> voice_pending;
    std::optional<float> voice_range_pending;  // the host's range slider while dragged / until the model agrees
    double voice_range_until{};
    std::map<std::uint64_t, std::pair<float, double>> voice_volume_pending;
    std::array<bool, 256> voice_keys_down{};
    std::string multiplayer_distance_lobby;
    int recording_bind = 0;
    ControllerComboCapture bind_capture;
    double bind_capture_until = 0;
    int last_menu_frame = -1;
    // Live while the scale slider is held; the saved value is authoritative otherwise.
    float scale = default_menu_scale;
    bool scale_editing{};
    // Selected tab on each page; see the Page list in skate_menu.cpp.
    int map_tab = 0, world_tab = 0, build_tab = 0, skater_tab = 0, settings_tab = 0, mods_tab = 0;
    // STYLE page and style editor screen. The slider and the dragged keyframe keep their own value until the game has it.
    struct Styling {
        int trick = 2, key = 0, edit_joint = -1, drag_key = -1, replay_trick = 0;
        float time = 0.5f, drag_time{};
        double drag_until{}, pending_until{}, preview_off_sent{}, asked_at{}, closing_until{}, drawn_at{};
        std::string asked; // the trick that the editor last asked to show
        std::array<char, 41> preset_name{}; // typed for a new preset
        double delete_until{};              // a first click on delete counts until then
        int pending_key = -1;               // a new keyframe, selected when the game has it
        std::array<float, 4> orbit{};       // camera turn, zoom and lift not yet sent
        style::Target edit_target;
        std::array<float, 3> edit{};
        double edit_until{}, edit_sent{};
    } styling;
    bool loose_files_settings_loaded{}, loose_files_saved{true};
    bool custom_scripts_scanned{};
    std::vector<custom_scripts::Script> custom_script_rows;
    std::string custom_script_error;
    std::array<std::optional<int>, 3> graphics_pending{};
    std::array<double, 3> graphics_pending_until{};
    std::array<char, 128> object_search{};
    int world_layer_category = 0;
    std::array<char, 128> world_layer_search{};
    int progression_tab = 0;
    int bus_stop_map = 0;
    int challenge_type = 0;
    std::array<char, 128> challenge_search{};
    bool score_dirty = false;
    std::int64_t score_edit = 0, score_cap_edit = 1;
    int score_level_edit = 1;
    std::array<int, 4> district_edit{};
    std::array<bool, 4> district_dirty{};
    std::string destination = "levels/game/BAM_LevelRoot/BAM_LevelRoot";
    std::string feedback;
    std::array<char, 128> mission_search{};
    int mission_group = 0;
    ParkChoices park_edit, park_seen;
    std::array<char,128> atmosphere_search{};
    std::array<AtmosphereEdit, dingosdk::atmosphere_controls.size()> atmosphere_edit{};
    double feedback_until = 0;
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;    // brushed page titles
    ImFont* heading = nullptr;  // tile and section headers
    ImFont* mono = nullptr;
};
void load_skate_fonts(SkateMenu& menu);
// The style editor screen, shown while Model::debug.style_editor is set.
void draw_style_editor(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool exit_requested);
// True while the style editor screen is asked for or open.
bool style_editor_wanted() noexcept;
void draw_skate_menu(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks,
                     bool& visible);
}
