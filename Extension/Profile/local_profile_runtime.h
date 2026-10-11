#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include <string_view>
#include <vector>
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Game/World/world_layers.h"
#include "Engine/Game/World/world_controls.h"
#include "Engine/Game/Rendering/graphics_controls.h"
#include "Engine/Game/Profile/player_card.h"
#include "Engine/Game/Profile/progression.h"
#include "Engine/Game/Profile/object_persistence_model.h"
#include "Engine/Game/World/park_editor.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/UI/menu_scale.h"
#include "Engine/Core/Json/json.h"
#include <utility>
namespace dingosdk {
struct LocalProfileAccess {
    bool neighborhoods{};
    bool preset_slots{};
    bool bus_stops{};
    bool neighborhood_ranks{};
    bool cosmetics{};
};
LocalProfileAccess local_profile_access() noexcept;
bool local_profile_owns_entitlement(std::string_view id) noexcept;
// Called on the game-update thread after native item data becomes available.
void update_local_customization() noexcept;
// Cancel asset-backed service callbacks before native level teardown.
void local_profile_before_level_transition(unsigned next) noexcept;
using LocalLocationTravelQueue = bool (*)(const char* map);
void set_local_location_travel_queue(LocalLocationTravelQueue) noexcept;
std::uint32_t local_customization_selected_preset() noexcept;
// Whether a saved outfit can be loaded yet: a cosmetics catalog has been read
// this session to check it against, or nothing is saved. The game builds every
// slot up to the selected one when a slot is selected and does not ask again,
// so the first selection waits for this.
bool local_customization_outfits_loadable() noexcept;
void observe_local_customization_selection(std::int32_t index) noexcept;
struct LocalMissionRow {
    std::string id, group;
    int completed{-1}; // -1: no saved override; 0/1: explicit profile state.
};
struct LocalMissions {
    bool available{};
    std::vector<LocalMissionRow> rows;
    std::string feedback;
};
LocalMissions local_profile_missions();
ProgressionModel local_profile_progression();
ObjectPersistenceModel local_profile_object_persistence();
ParkEditorModel local_park_editor();
void set_park_mod_list_visible(bool visible) noexcept;
bool queue_local_park_surface(const EditorSurfaceRequest &);
bool queue_local_park_preview(const EditorPreviewRequest &);
bool queue_local_park_selection(const EditorSelectionRequest &);
bool queue_local_park_paste(const EditorPasteRequest &);
void tick_local_park_editor() noexcept;
ControllerBindingsModel local_profile_controller_bindings();
bool set_local_freecam_controller(bool);
bool local_freecam_controller();
bool set_local_freecam_controller_binding(std::uint32_t);
bool set_local_freecam_binding(std::uint32_t);
bool set_local_tp_to_freecam_binding(std::uint32_t);
// The player's binding for Yes (or No) in a dedicated server's vote; 0 clears it.
bool set_local_vote_binding(bool yes, std::uint32_t);
// One of action_binds (controller_bindings.h), by its place there; 0 clears it.
bool set_local_action_binding(std::size_t index, std::uint32_t);
bool set_local_noclip_binding(std::uint32_t);
bool set_local_forward_velocity_binding(std::uint32_t);
bool set_local_up_velocity_binding(std::uint32_t);
bool set_local_offboard_up_velocity_binding(std::uint32_t);
bool set_local_object_persistence(bool enabled);
bool clear_local_persisted_objects(std::string_view map);
bool delete_local_placed_object(std::string_view map, std::uint64_t token);
bool teleport_to_local_placed_object(std::string_view map, std::uint64_t token);
// Teleports the local skater to a world position (sent within 5 s, when the skater can be
// moved). False when the game's teleport is unavailable.
bool teleport_local_skater(const std::array<float, 3>& position);
// The highest collision surface straight down at (x, z) between world heights `top` and
// `bottom`, from the client physics world (the park editor's native ray). Empty when nothing
// is there yet (collision still streaming) or the query is unavailable. Client update thread.
std::optional<float> local_ground_height(float x, float z, float top, float bottom);
// Small allowlisted progression commands, executed on the game update thread.
bool set_local_progression(const std::vector<std::string>& arguments);
// Small ReSkate-owned booleans saved beside the profile under a "ReSkate."
// prefix. Absent until first written; never exposed to the game's own settings.
namespace profile_runtime {
std::optional<bool> local_preference(std::string_view key) noexcept;
void set_local_preference(std::string_view key, bool value) noexcept;
// The same "ReSkate." store for numbers and text. A key keeps the type it was
// first saved with. set_local_values saves several at once (one transaction).
std::optional<Json> local_value(std::string_view key) noexcept;
void set_local_values(const std::vector<std::pair<std::string, Json>>& values) noexcept;
// ReSkate menu scale, clamped to a legible range. Cached so the overlay can
// read it every frame without touching the database.
float local_menu_scale() noexcept;
bool set_local_menu_scale(float value) noexcept;
}
PlayerCardModel local_profile_player_card();
// Renames only the local RIP Card. An empty name restores the Steam name.
bool set_local_player_card_name(std::string_view name);
ParksModel local_profile_parks();
bool set_local_park(std::string_view lot, std::string_view layout);
bool load_random_local_parks();
bool set_local_park_randomize_on_launch(bool enabled);
// Transient lobby authority. Guest choices never overwrite saved preferences.
void set_lobby_park_mode(bool active, bool guest);
void apply_host_park_choices(const ParkChoices& choices);
WorldLayersModel local_profile_world_layers();
void apply_host_world_layers(bool forced, const WorldLayerChoices& choices);
// Runtime-only hint for a manifest-backed standalone level whose deliberately
// reduced donor graph has no canonical retail map anchor.
void set_local_world_layer_map_hint(WorldMap map) noexcept;
bool set_local_world_layer(std::string_view layer, std::string_view mode);
bool restore_local_world_layers(std::string_view map);
// Execute on the already-verified client update thread, including transitions.
WorldControlsModel local_profile_world_controls();
GraphicsControlsModel local_profile_graphics_controls();
void update_local_graphics_controls() noexcept;
bool set_local_graphics_control(std::string_view key, int value);
bool set_local_world_control(std::string_view key, float value);
bool set_local_atmosphere_control(std::string_view key, std::string_view value);
bool reset_local_atmosphere_controls(unsigned kind);
void update_local_world_controls(WorldMap map) noexcept;
// Execute on the game update thread, never from ImGui's Present callback.
bool set_local_mission_completed(std::string_view id, bool completed);
bool initialize_local_profile(std::uintptr_t image_base, bool authored_offline,
    const std::filesystem::path& path);
}
