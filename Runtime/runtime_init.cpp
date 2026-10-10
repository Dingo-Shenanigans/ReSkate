#include "runtime_internal.h"
#include "bootstrap.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Settings/engine_tweaks.h"
#include "Extension/UI/ui_pointer_skip.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/profiler_labels.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Boot/offline_boot.h"
#include "Extension/HallOfMeat/hall_of_meat.h"
#include "Extension/Skater/camera_observer.h"
#include "Extension/Progression/entitlement_request_hook.h"
#include "Extension/Skater/skater_observer.h"
#include "Extension/UI/NativeMenu/native_menu.h"
#include "Extension/UI/NativeMenu/ui_sound.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Throwdowns/throwdown_debug_text.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Profile/local_profile.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Progression/fixed_stop_entitlement_provider.h"
#include "Extension/Progression/mission_progression_override.h"
#include "Extension/Progression/neighborhood_unlock_override.h"
#include "Extension/Progression/progression_service_guard.h"
#include "Extension/Rendering/graphics_labels.h"
#include "Extension/Settings/gameplay_settings_override.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/skater_slot_override.h"
#include "Extension/UI/Startup/startup_window.h"
#include "Extension/World/level_loading.h"
#include "Extension/World/loading_screen.h"
#include "Extension/Customization/preset_lookup_guard.h"
#include "Extension/Rendering/replay_export.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Vfs/world_layer_scan.h"
#include "Engine/Game/World/world_layer_catalog.h"
#include <array>
#include <filesystem>

namespace dingosdk::runtime::detail {
namespace {
bool exact_environment_one(const wchar_t* name) noexcept {
    const auto incoming_error = GetLastError();
    std::array<wchar_t, 2> value{};
    const auto length = GetEnvironmentVariableW(name, value.data(),
        static_cast<DWORD>(value.size()));
    SetLastError(incoming_error);
    return length == 1 && value[0] == L'1';
}
bool validate_image(std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != dingosdk::supported_build::game_image_size) return false;
    if (!matches(base + rt::client_tick, rt::client_tick_prefix)) return false;
    for (const auto& entry : rt::image_checks) if (!matches(base + entry.rva, entry.bytes)) return false;
    return true;
}
}
}

extern "C" __declspec(dllexport) BOOL WINAPI DingoSDKDebugInitialize() {
    try {
        using namespace dingosdk::runtime::detail;
        auto& r = runtime();
        if (r.initialized) return TRUE;
        r.base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto logging_options = dingosdk::logging::options_from_environment();
        const bool files_ready = dingosdk::logging::initialize(logging_options);
        dingosdk::logging::startup_banner();
        if (!files_ready) dingosdk::logging::write(dingosdk::logging::Level::warning,
            dingosdk::logging::Channel::runtime, "File logging is unavailable; console and debugger output remain active.");
        if (!validate_image(r.base)) {
            dingosdk::logging::write(dingosdk::logging::Level::critical, dingosdk::logging::Channel::runtime, "Game image validation failed.");
            return FALSE;
        }
        if (const auto& content = dingosdk::content_cache::catalogs(); content.available) {
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::assets,
                "Game content cache: {} items, {} challenges, {} entitlements.",
                content.items.size(), content.challenges.size(), content.entitlements.size());
        } else {
            dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::assets,
                "Game content cache is not installed: item names, challenges and default entitlements are unavailable. "
                "Start ReSkate from ReSkateLauncher to install it.");
        }
        // World layers come from the installed level data. The launcher builds
        // the per-build cache; without it this scans the level TOCs once here.
        try {
            std::wstring executable(32768, L'\0');
            executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
            const auto started = GetTickCount64();
            auto layers = dingosdk::world_layer_scan::load_or_scan(
                std::filesystem::path(executable).parent_path(), dingosdk::world_layer_scan::cache_file());
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::world,
                "World layers: {} layers across {} maps from the level data ({} ms).",
                layers.layers.size(), layers.anchors.size(), GetTickCount64() - started);
            dingosdk::install_world_layer_catalog(std::move(layers));
        } catch (const std::exception& error) {
            dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::world,
                "World layers are unavailable: {}", error.what());
        }
        // The game shows nothing for several seconds yet; the startup window
        // covers that, and goes by itself once the game's window appears. A
        // startup that fails takes it down on the way out.
        dingosdk::startup::open();
        struct StartupWindow {
            bool handed_over{};
            ~StartupWindow() { if (!handed_over) dingosdk::startup::close(); }
            void hand_over() {
                handed_over = true;
                dingosdk::startup::status("Starting Skate\xE2\x80\xA6");
            }
        } startup_window;
        dingosdk::initialize_client_source_spawn(r.base);
        if (!dingosdk::runtime::initialize_bootstrap(r.base)) return FALSE;

        // Skate imports its graphics factory from the sibling Streamline
        // interposer. Load that ordinary dependency while the game is still
        // suspended so the overlay can hook the provider before first use.
        HMODULE self{};
#pragma warning(push)
#pragma warning(disable: 4191)
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DingoSDKDebugInitialize), &self)) {
            dingosdk::logging::log(dingosdk::logging::Level::error, dingosdk::logging::Channel::runtime,
                "Cannot locate the runtime module (Windows error {}).", GetLastError());
            return FALSE;
        }
#pragma warning(pop)
        wchar_t self_path[32768]{};
        const auto self_length = GetModuleFileNameW(self, self_path, 32768);
        if (!self_length || self_length >= 32768) {
            dingosdk::logging::log(dingosdk::logging::Level::error, dingosdk::logging::Channel::runtime,
                "Cannot resolve the runtime path (Windows error {}).", GetLastError());
            return FALSE;
        }
        const auto interposer_path = std::filesystem::path(
            std::wstring(self_path, self_length)).parent_path() / L"sl.interposer.dll";
        // Every mod folder may declare destinations, highest priority first,
        // and the Patch folder keeps working for a single Studio-built map.
        const auto& mod_catalog = dingosdk::mods::catalog();
        const auto patch_level_path = std::filesystem::path(
            std::wstring(self_path, self_length)).parent_path() / L"Patch" / L"reskate-levels.json";
        r.patch_level_manifests = {patch_level_path};
        if (!mod_catalog.data_root.empty()) {
            const auto data_level_path = mod_catalog.data_root / L"Patch" / L"reskate-levels.json";
            std::error_code level_path_error;
            if (!std::filesystem::equivalent(patch_level_path, data_level_path, level_path_error) ||
                level_path_error) r.patch_level_manifests.push_back(data_level_path);
        }
        r.custom_levels = read_custom_levels(dingosdk::mods::level_manifest_paths(mod_catalog));
        if (r.custom_levels.present)
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::level,
                "Custom-level destinations after merging: {}.", r.custom_levels.levels.size());
        auto interposer = GetModuleHandleW(L"sl.interposer.dll");
        if (!interposer) interposer = LoadLibraryExW(interposer_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!interposer) {
            dingosdk::logging::log(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics,
                "Cannot load the game's sl.interposer.dll (Windows error {}).", GetLastError());
            return FALSE;
        }
        wchar_t interposer_loaded_path[32768]{};
        const auto interposer_length = GetModuleFileNameW(
            interposer, interposer_loaded_path, 32768);
        std::error_code interposer_error;
        if (!interposer_length || interposer_length >= 32768 ||
            !std::filesystem::equivalent(interposer_path,
                std::filesystem::path(std::wstring(interposer_loaded_path, interposer_length)),
                interposer_error) || interposer_error) {
            dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics,
                "Loaded graphics provider does not match the game's sibling sl.interposer.dll.");
            return FALSE;
        }
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
            "Game graphics provider loaded: sl.interposer.dll.");
        const dingosdk::overlay::CallbacksV3 callbacks{
            nullptr, read_model, queue_load, queue_debug, queue_offline_feature,
            queue_console_command};
        auto native_callbacks = callbacks;
        native_callbacks.read_model = read_native_menu_model;
        dingosdk::multiplayer::set_native_menu_callbacks(native_callbacks);
        dingosdk::overlay::set_multiplayer_queue(queue_multiplayer_command);
        dingosdk::overlay::set_chat_feed(dingosdk::multiplayer::chat);
        dingosdk::overlay::set_hub_page_feed(dingosdk::multiplayer::native_menu_page);
        dingosdk::overlay::set_hub_callbacks(native_callbacks);
        dingosdk::overlay::set_ui_sound(dingosdk::multiplayer::queue_ui_sound);
        dingosdk::overlay::set_game_text_feed(dingosdk::multiplayer::skate_debug_text);
        dingosdk::overlay::set_skate_hud_feed(dingosdk::multiplayer::skate_hud);
        dingosdk::overlay::set_nametag_feed(dingosdk::multiplayer::custom_nametags);
        // Engine functions the profiler's stack sampler names in its reports.
        static constexpr dingosdk::profiler::Label engine_labels[]{
            {dingosdk::addr::profiler_labels::client_update, "Client game update (ReSkate tick hook)"},
            {dingosdk::addr::profiler_labels::client_frame_job.rva, "Client frame job"},
            {dingosdk::addr::profiler_labels::expression_vm, "Expression VM"},
            {dingosdk::addr::profiler_labels::expression_vm_profiled, "Expression VM (profiled)"},
            {dingosdk::addr::profiler_labels::menu_expression, "Menu expression"},
            {dingosdk::addr::profiler_labels::job_run_until_done, "Job system: run jobs until done"},
            {dingosdk::addr::profiler_labels::job_execute, "Job system: execute job"},
            {dingosdk::addr::profiler_labels::job_idle_wait, "Job system: idle wait"},
            {dingosdk::addr::profiler_labels::simulation_loop_thread, "GameSimulationLoop thread"},
            {dingosdk::addr::profiler_labels::job_worker_loop, "Job system: worker loop"},
            {dingosdk::addr::profiler_labels::job_pop, "Job system: look for work (spins when idle)"},
            {dingosdk::addr::profiler_labels::job_queue_pop, "Job system: queue pop"},
            {dingosdk::addr::profiler_labels::expression_shader_model_build, "Job: expressionShaderModelBuild"},
            {dingosdk::addr::profiler_labels::render_bundle_job, "Job: renderBundleJob"},
            {dingosdk::addr::profiler_labels::mesh_cull, "Job: meshCull"},
            {dingosdk::addr::profiler_labels::render_dispatch_end, "Job: renderDispatchEnd"},
            {dingosdk::addr::profiler_labels::input_dispatch, "Input dispatch"},
            {dingosdk::addr::profiler_labels::ui_pointer_update, "UI pointer update (mouse hit-test)"},
            {dingosdk::addr::profiler_labels::ui_view_pointer, "UI view pointer hit-test"}};
        dingosdk::profiler::set_engine_labels(engine_labels);
        dingosdk::overlay::set_park_surface_queue(dingosdk::queue_local_park_surface);
        dingosdk::overlay::set_park_preview_queue(dingosdk::queue_local_park_preview);
        dingosdk::overlay::set_park_selection_queue(dingosdk::queue_local_park_selection);
        dingosdk::overlay::set_park_paste_queue(dingosdk::queue_local_park_paste);
        if (!DingoSDKOverlayStartV3(&callbacks)) {
            dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Overlay startup failed.");
            return FALSE;
        }
        // Install the offline boot hooks before the passive observers and other
        // native hooks, while Skate's primary thread remains suspended. The
        // launcher always sets the global-offline variable; install the dependent
        // guard first in forwarding-only mode. Launches without it do not
        // install this hook.
        const bool global_offline_requested = exact_environment_one(
            L"RESKATE_DEBUG_TEST_GLOBAL_OFFLINE");
        const bool progression_guard_prepared = global_offline_requested &&
            dingosdk::prepare_progression_service_guard(r.base);
        record("{\"event\":\"progression_service_guard_prepared\",\"requested\":" +
            std::string(global_offline_requested ? "true" : "false") +
            ",\"prepared\":" +
            std::string(progression_guard_prepared ? "true" : "false") + "}");
        const bool global_offline_hook = global_offline_requested &&
            progression_guard_prepared && dingosdk::start_offline_boot(r.base);
        record("{\"event\":\"global_offline_experiment_initialized\",\"active\":" +
            std::string(global_offline_hook ? "true" : "false") +
            ",\"requested\":" +
            std::string(global_offline_requested ? "true" : "false") +
            ",\"progression_guard_prepared\":" +
            std::string(progression_guard_prepared ? "true" : "false") + "}");
        const auto global_offline = dingosdk::offline_boot_observation();
        record(global_offline.json);
        const bool progression_guard_armed = global_offline_hook &&
            dingosdk::arm_progression_service_guard(r.base);
        record("{\"event\":\"progression_service_guard_initialized\",\"prepared\":" +
            std::string(progression_guard_prepared ? "true" : "false") +
            ",\"armed\":" + std::string(progression_guard_armed ? "true" : "false") +
            ",\"global_offline_prerequisite\":" +
            std::string(global_offline_hook ? "true" : "false") + "}");
        const auto progression_guard = dingosdk::progression_service_guard_observation();
        record(progression_guard.json);
        if (global_offline_requested && (!global_offline_hook || !progression_guard_armed)) {
            record("{\"event\":\"launcher_readiness\",\"ready\":false,"
                "\"reason\":\"authored_offline_hooks\"}");
            return FALSE;
        }
        const bool graphics_labels = dingosdk::initialize_graphics_labels(r.base,
            global_offline_hook && progression_guard_armed);
        record("{\"event\":\"graphics_labels_initialized\",\"active\":" +
            std::string(graphics_labels ? "true" : "false") + "}");
        if (global_offline_hook && progression_guard_armed &&
            !dingosdk::initialize_local_profile(r.base, true, dingosdk::profile::default_path())) {
            record("{\"event\":\"launcher_readiness\",\"ready\":false,\"reason\":\"local_profile\"}");
            return FALSE;
        }
        if (global_offline_hook && progression_guard_armed) {
            dingosdk::set_local_location_travel_queue(&queue_location_travel);
            start_menu_entry();
        }
        start_frame_timing();
        (void)dingosdk::ui_pointer::install(r.base);
        (void)dingosdk::engine_tweaks::install(r.base);
        const bool fixed_stop_provider = (global_offline_hook && progression_guard_armed) &&
            dingosdk::prepare_fixed_stop_entitlement_provider(r.base);
        record("{\"event\":\"fixed_stop_entitlement_provider_initialized\",\"prepared\":" +
            std::string(fixed_stop_provider ? "true" : "false") +
            ",\"requested\":" +
            std::string(global_offline_requested ? "true" : "false") + "}");
        const auto fixed_stop_provider_observation =
            dingosdk::fixed_stop_entitlement_provider_observation();
        record(fixed_stop_provider_observation.json);
        r.last_fixed_stop_entitlement_provider_observation =
            fixed_stop_provider_observation.json;
        const bool entitlement_trace = (global_offline_hook && progression_guard_armed) &&
            dingosdk::prepare_entitlement_request_trace(r.base);
        record("{\"event\":\"entitlement_request_trace_initialized\",\"active\":" +
            std::string(entitlement_trace ? "true" : "false") +
            ",\"requested\":" +
            std::string(global_offline_requested ? "true" : "false") + "}");
        const auto entitlement_trace_observation =
            dingosdk::entitlement_request_trace_observation();
        record(entitlement_trace_observation.json);
        r.fixed_stop_entitlement_route_ready = fixed_stop_provider && entitlement_trace;
        dingosdk::set_fixed_stop_entitlement_ownership(dingosdk::local_profile_owns_entitlement);
        dingosdk::set_fixed_stop_entitlement_provider_enabled(
            r.fixed_stop_entitlement_route_ready && dingosdk::local_profile_access().bus_stops);
        record("{\"event\":\"fixed_stop_entitlement_route_initialized\",\"ready\":" +
            std::string(r.fixed_stop_entitlement_route_ready ? "true" : "false") +
            ",\"provider_prepared\":" +
            std::string(fixed_stop_provider ? "true" : "false") +
            ",\"trace_prepared\":" +
            std::string(entitlement_trace ? "true" : "false") + "}");
        const bool neighborhood_unlock_provider =
            (global_offline_hook && progression_guard_armed) &&
            dingosdk::prepare_neighborhood_unlock_override(r.base);
        if (neighborhood_unlock_provider)
            dingosdk::set_expression_binder_observer(&observe_player_card_expression);
        if (neighborhood_unlock_provider)
            (void)dingosdk::set_neighborhood_unlock_override_enabled(dingosdk::local_profile_access().neighborhoods);
        record("{\"event\":\"neighborhood_unlock_provider_initialized\",\"prepared\":" +
            std::string(neighborhood_unlock_provider ? "true" : "false") +
            ",\"requested\":" +
            std::string(global_offline_requested ? "true" : "false") + "}");
        const auto neighborhood_unlock_provider_observation =
            dingosdk::neighborhood_unlock_override_observation();
        record(neighborhood_unlock_provider_observation.json);
        r.last_neighborhood_unlock_override_observation =
            neighborhood_unlock_provider_observation.json;
        const bool named_settings = dingosdk::initialize_named_settings(r.base);
        record("{\"event\":\"named_settings_initialized\",\"ready\":" + std::string(named_settings ? "true" : "false") + "}");
        const bool gameplay_settings = dingosdk::initialize_gameplay_settings_override(
            r.base, global_offline_hook && progression_guard_armed);
        const bool skater_slots = dingosdk::initialize_skater_slot_override(
            r.base, global_offline_hook && progression_guard_armed);
        const bool main_missions = (global_offline_hook && progression_guard_armed) &&
            dingosdk::prepare_main_mission_override(r.base);
        const auto gameplay_settings_observation =
            dingosdk::gameplay_settings_override_observation();
        const auto skater_slot_observation =
            dingosdk::skater_slot_override_observation();
        const auto main_mission_observation =
            dingosdk::main_mission_override_observation();
        record(gameplay_settings_observation.json);
        record(skater_slot_observation.json);
        record(main_mission_observation.json);
        r.last_gameplay_settings_override_observation =
            gameplay_settings_observation.json;
        r.last_skater_slot_override_observation = skater_slot_observation.json;
        r.last_main_mission_override_observation = main_mission_observation.json;
        {
            std::lock_guard lock(r.mutex);
            r.offline_model = gameplay_settings_observation.model;
            merge_skater_slot_observation(r.offline_model, skater_slot_observation);
            merge_main_mission_observation(r.offline_model, main_mission_observation);
            merge_progression_provider_observations(r.offline_model,
                fixed_stop_provider_observation,
                neighborhood_unlock_provider_observation,
                r.fixed_stop_entitlement_route_ready);
            ++r.offline_revision;
        }
        record("{\"event\":\"gameplay_settings_override_initialized\",\"active\":" +
            std::string(gameplay_settings ? "true" : "false") +
            ",\"authored_offline_prerequisite\":" +
            std::string(global_offline_hook && progression_guard_armed ? "true" : "false") + "}");
        record("{\"event\":\"skater_slot_override_initialized\",\"active\":" +
            std::string(skater_slots ? "true" : "false") +
            ",\"authored_offline_prerequisite\":" +
            std::string(global_offline_hook && progression_guard_armed ? "true" : "false") + "}");
        record("{\"event\":\"main_mission_override_initialized\",\"prepared\":" +
            std::string(main_missions ? "true" : "false") +
            ",\"authored_offline_prerequisite\":" +
            std::string(global_offline_hook && progression_guard_armed ? "true" : "false") + "}");
        const bool skater_hooks = dingosdk::start_skater_observer(r.base);
        record("{\"event\":\"skater_hooks_initialized\",\"active\":" + std::string(skater_hooks ? "true" : "false") + "}");
        const bool camera_hook = dingosdk::start_camera_observer(r.base);
        record("{\"event\":\"camera_observer_initialized\",\"active\":" + std::string(camera_hook ? "true" : "false") + "}");
        (void)dingosdk::start_no_bail(r.base);
        const bool meat = dingosdk::hall_of_meat::start(r.base);
        record("{\"event\":\"hall_of_meat_initialized\",\"active\":" + std::string(meat ? "true" : "false") + "}");
        const bool noclip_velocity = dingosdk::start_client_noclip_velocity(r.base);
        record("{\"event\":\"noclip_velocity_initialized\",\"active\":" + std::string(noclip_velocity ? "true" : "false") + "}");
        const bool loading_screens = dingosdk::loading_screen::start(r.base);
        record("{\"event\":\"map_loading_screens_initialized\",\"active\":" + std::string(loading_screens ? "true" : "false") + "}");
        const bool preset_guard = dingosdk::preset_lookup_guard::start(r.base);
        record("{\"event\":\"preset_lookup_guard_initialized\",\"active\":" + std::string(preset_guard ? "true" : "false") + "}");
        const bool replay_export = dingosdk::replay_export::start(r.base);
        record("{\"event\":\"replay_export_initialized\",\"active\":" + std::string(replay_export ? "true" : "false") + "}");
        void* original{};
        auto* address = reinterpret_cast<void*>(r.base + rt::client_tick);
        auto hook_status = dingosdk::hook_prepare(address, reinterpret_cast<void*>(&tick), &original);
        if (hook_status == dingosdk::HookOk) {
            r.original_tick = reinterpret_cast<Tick>(original);
            hook_status = dingosdk::hook_enable(address);
            if (hook_status != dingosdk::HookOk) dingosdk::hook_remove(address);
        }
        if (hook_status != dingosdk::HookOk) {
            // The renderer is already installed. Keep it available for diagnostics
            // and report successful optional loading without claiming native control.
            {
                std::lock_guard lock(r.mutex);
                r.model.state = "Native controller unavailable";
                r.model.load_block_reason = "The engine update hook could not be initialized.";
                r.offline_model.available = false;
                r.offline_model.status =
                    "Gameplay settings require the native game update hook.";
                ++r.model_revision;
                ++r.offline_revision;
                r.initialized = !global_offline_requested;
            }
            record("{\"event\":\"native_hook_failed\",\"status\":" + std::to_string(hook_status) + "}");
            if (global_offline_requested) {
                record("{\"event\":\"launcher_readiness\",\"ready\":false,"
                    "\"reason\":\"native_tick_hook\"}");
                return FALSE;
            }
            startup_window.hand_over();
            return TRUE;
        }
        {
            std::lock_guard lock(r.mutex);
            r.native_tick_ready = true;
        }
        r.native_loading_logging = dingosdk::start_level_loading_logging(r.base,
            [](std::uintptr_t base, unsigned next) noexcept {
                dingosdk::local_profile_before_level_transition(next);
                dingosdk::multiplayer::native_menu_before_level_transition(base, next);
                dingosdk::multiplayer::throwdown_lab_before_level_transition(next);
            });
        if (!r.native_loading_logging) dingosdk::logging::write(dingosdk::logging::Level::warning,
            dingosdk::logging::Channel::ui, "Native ReSkate menu disabled: shutdown cleanup hook is unavailable.");
        r.initialized = true;
        if (global_offline_requested)
            record("{\"event\":\"launcher_readiness\",\"ready\":true}");
        record("{\"event\":\"debug_initialized\",\"overlay\":true,\"native_load\":\"guarded_local_only\"}");
        activity_line(dingosdk::ConsoleSource::runtime, "Offline runtime initialized. Waiting for the game.");
        startup_window.hand_over();
        return TRUE;
    } catch (const std::exception& error) {
        dingosdk::logging::log(dingosdk::logging::Level::critical, dingosdk::logging::Channel::runtime,
            "Runtime initialization failed: {}", error.what());
        return FALSE;
    } catch (...) {
        dingosdk::logging::write(dingosdk::logging::Level::critical, dingosdk::logging::Channel::runtime,
            "Runtime initialization failed with an unknown exception.");
        return FALSE;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
