#pragma once
#include "request_scheduler.h"
#include "Engine/Core/Console/console_activity.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/runtime.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Settings/named_settings.h"
#include "Engine/Game/World/custom_level_manifest.h"
#include "Engine/Game/World/world_model.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk {
struct SkaterSlotOverrideObservation;
struct MainMissionOverrideObservation;
struct FixedStopEntitlementProviderObservation;
struct NeighborhoodUnlockOverrideObservation;
}

// Shared by the runtime's translation units only. Everything here runs inside
// the one Runtime instance guarded by Runtime::mutex.
namespace dingosdk::runtime::detail {
namespace engine = dingosdk::addr::engine;
namespace rt = dingosdk::addr::runtime;
inline constexpr std::uintptr_t highest = memory::highest_user_address;
using memory::read;
using Tick = void (*)(std::uintptr_t, std::uintptr_t);
using Request = dingosdk::runtime::LoadRequest;
using ConsoleRequest = dingosdk::runtime::ConsoleRequest;

using NamedSettings = std::vector<dingosdk::NamedSettingModel>;
struct Runtime {
    std::uintptr_t base{};
    Tick original_tick{};
    std::mutex mutex;
    dingosdk::overlay::Model model;
    dingosdk::runtime::RequestScheduler requests;
    dingosdk::overlay::DebugModel debug_model;
    dingosdk::overlay::OfflineFeatureModel offline_model;
    // Increased under the mutex with every write of model, debug_model and
    // offline_model: readers holding a copy skip the parts that have not changed.
    std::uint64_t model_revision{1}, debug_revision{1}, offline_revision{1};
    // Engine settings as the menu and console last received them: an immutable
    // snapshot (thousands of rows) that readers copy outside the mutex. Its
    // revision is named_settings_revision() at publication.
    std::shared_ptr<const NamedSettings> named_settings;
    std::uint64_t named_settings_revision{};
    // Game thread only: the previous snapshot, refilled in place once no reader holds it.
    std::shared_ptr<NamedSettings> named_settings_spare;
    ULONGLONG next_named_settings_publish{};
    // Set by the overlay while it shows the menu or console: until then the game
    // thread refreshes engine settings quickly and publishes them.
    std::atomic<ULONGLONG> named_settings_wanted_until{};
    bool named_context_ready{};
    dingosdk::CustomLevelManifest custom_levels;
    // Patch/reskate-levels.json beside the DLL and under the data root, read after the mods'.
    std::vector<std::filesystem::path> patch_level_manifests;
    ULONGLONG next_debug{}, flight_unavailable_since{};
    std::string flight_unavailable_issue, flight_unavailable_logged;
    dingosdk::ControllerComboLatch freecam_controller_bind_latch;
    dingosdk::ControllerComboLatch freecam_bind_latch;
    dingosdk::ControllerComboLatch tp_to_freecam_bind_latch;
    dingosdk::ControllerComboLatch noclip_bind_latch;
    dingosdk::ControllerComboLatch forward_velocity_bind_latch;
    dingosdk::ControllerComboLatch up_velocity_bind_latch;
    dingosdk::ControllerComboLatch offboard_up_velocity_bind_latch;
    dingosdk::ControllerComboLatch vote_yes_bind_latch, vote_no_bind_latch;
    // The number keys 1 to 6, which answer a dedicated server's poll while one is running.
    std::array<dingosdk::ControllerComboLatch, 6> poll_answer_latches;
    std::array<dingosdk::ControllerComboLatch, dingosdk::action_binds.size()> action_bind_latches;
    ULONGLONG next_offline{};
    bool transition_seen{}, initialized{}, catalog_logged{},
        native_tick_ready{}, native_loading_logging{}, fixed_stop_entitlement_route_ready{}, menu_load_queued{}, startup_load_queued{};
    std::atomic<bool> menu_splash_ready{};
    std::atomic<bool> observer_failed{};
    // Refreshed with the 500 ms world model; the spectate camera reads it every tick.
    bool spectate_ready{};
    std::uintptr_t spectate_client{};
    std::atomic<DWORD> engine_thread{};
    DWORD previous_state{~DWORD{0}}, requested_from{13};
    ULONGLONG next_model{}, request_started{};
    std::string last_request, last_start, last_lm_level, last_lm_start, last_level, load_result, last_sublevels;
    std::string multiplayer_map;
    // Game thread only. The native level registry changes only with a mod apply
    // or a level load, and the server's sublevels settle during loads: both are
    // read again then, and otherwise at a slower pace than the 500 ms model.
    std::optional<dingosdk::WorldCatalog> native_catalog;
    std::optional<dingosdk::SublevelCatalog> sublevel_catalog;
    std::string catalog_level;
    ULONGLONG next_catalog_read{}, next_sublevel_read{};
    std::atomic<bool> overlay_confirmed{false};
    std::string last_readiness, last_client_content, last_gameplay_settings_override_observation,
        last_skater_slot_override_observation,
        last_main_mission_override_observation,
        last_fixed_stop_entitlement_provider_observation,
        last_neighborhood_unlock_override_observation;
    std::uint64_t console_delivered_sequence{}; // Single, serialized overlay reader.
    dingosdk::ConsoleActivityFeed console_activity;
};
Runtime& runtime();
dingosdk::CustomLevelManifest read_custom_levels(const std::vector<std::filesystem::path>& mod_manifests);
dingosdk::runtime::RequestContext request_context(const Runtime& value);
// Game thread: hands the menu and console the current engine settings, if they
// changed since the last snapshot (runtime_state.cpp).
void publish_named_settings(Runtime& value);
struct ScheduledWorkScope {
    Runtime& value;
    bool active;
    ~ScheduledWorkScope() {
        if (active) { std::lock_guard lock(value.mutex); value.requests.finish_work(); }
    }
};

template<std::size_t N> bool matches(std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> actual{};
    return read(address, actual) && actual == expected;
}
void record(const std::string& json);
void console_line(std::string_view text);
void activity_line(dingosdk::ConsoleSource source, std::string_view text,
                   dingosdk::ConsoleSeverity severity = dingosdk::ConsoleSeverity::info);
bool equals(std::string a, std::string b);

// Offline feature model assembly (runtime_state.cpp).
void merge_skater_slot_observation(
    dingosdk::overlay::OfflineFeatureModel& model,
    const dingosdk::SkaterSlotOverrideObservation& slots);
void merge_main_mission_observation(
    dingosdk::overlay::OfflineFeatureModel& model,
    const dingosdk::MainMissionOverrideObservation& missions);
void merge_progression_provider_observations(
    dingosdk::overlay::OfflineFeatureModel& model,
    const dingosdk::FixedStopEntitlementProviderObservation& fixed_stops,
    const dingosdk::NeighborhoodUnlockOverrideObservation& neighborhoods,
    bool fixed_stop_route_ready);

// Native client reads (client_context.cpp).
struct ClientContent {
    bool sublevels_available{}, requested_sublevel_present{}, player_manager_present{}, players_available{};
    std::uint32_t sublevel_count{}, local_player_count{}, controllable_reference_count{};
};
ClientContent read_client_content(std::uintptr_t base, std::uintptr_t client, const std::string& requested);
bool native_context(std::uintptr_t base, std::uintptr_t client, DWORD game_type);

// Game update thread (client_tick.cpp).
void tick(std::uintptr_t client, std::uintptr_t update);

// Level requests (level_requests.cpp).
bool registered_request(const dingosdk::WorldCatalog& catalog, const Request& request);
void submit_local(std::uintptr_t base, std::uintptr_t client, const Request& request, std::uint8_t flag);
dingosdk::multiplayer::MapLoadResult load_multiplayer_map(std::string_view destination, bool submitted,
                                                        std::string& detail);
bool queue_load(void*, const char* asset, const char* start, const char* lm_level, const char* lm_start,
                char* result, std::size_t size);
bool queue_location_travel(const char* map);
bool queue_menu_bam();

// Overlay and native menu callbacks (overlay_callbacks.cpp).
void read_model(void*, dingosdk::overlay::Model& output);
void read_native_menu_model(void*, dingosdk::overlay::Model& output);
bool queue_debug(void*, const dingosdk::overlay::DebugRequest& request, char* result, std::size_t size);
bool queue_offline_feature(void*, const dingosdk::overlay::OfflineFeatureRequest& request,
                           char* result, std::size_t size);
bool queue_multiplayer_command(const char* action, const char* argument, const char* password,
                               char* result, std::size_t size);
bool queue_console_command(void*, const char* command, char* result, std::size_t size);

// Scheduled console commands (console_bridge.cpp).
void execute_console_command(const std::string& command);

// Main-menu expression hook (menu_entry_hook.cpp).
void observe_player_card_expression(std::uintptr_t graph, std::uintptr_t runtime_graph, void** inputs);
void start_menu_entry();
void start_frame_timing();
}
