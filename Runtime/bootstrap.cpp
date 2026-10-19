#include "bootstrap.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/Boot/startup_interventions.h"
#include "Extension/Console/commands.h"
#include "Extension/Boot/steam_restart_guard.h"
#include "Extension/Boot/user_data_redirect.h"
#include "Extension/Boot/ea_app_block.h"
#include "Extension/Boot/ea_service_block.h"
#include "Extension/Boot/descriptor_heap_size.h"
#include "Extension/Boot/game_fatal_dump.h"
#include "Extension/Boot/eadp_errors_arena.h"
#include "Extension/Assets/native_patch_support.h"
#include "Extension/Assets/mod_layers.h"
#include "Extension/Assets/loose_files.h"
#include "Extension/Scripting/custom_script_loader.h"
#include "Extension/Scripting/lua_native_block.h"
#include "Extension/Assets/morph_memory_pool.h"
#include "Extension/Assets/native_render_resource_pool.h"
#include "Extension/Rendering/display_startup.h"
#include "Extension/World/native_route_lookahead.h"
#include "Extension/World/native_entity_pages.h"
#include "Extension/World/world_pool_usage.h"
#include "Extension/World/physics_world_size.h"
#include "Extension/World/spatial_cull_ids.h"
#include <string>

namespace dingosdk::runtime {
namespace {
bool stage(logging::Channel channel, const char* name, bool ready, const std::string& error) {
    logging::log(ready ? logging::Level::info : logging::Level::error, channel,
        "{}: {}{}", name, ready ? "ready" : "failed", error.empty() ? "" : " - " + error);
    return ready;
}
}
bool initialize_bootstrap(std::uintptr_t base) {
    if (!base) return false;
    using logging::Channel;
    std::string error;
    bool ready = start_startup_interventions(base, error);
    if (!stage(Channel::runtime, "Startup interventions", ready, error)) return false;
    const auto hooks = hook_initialize();
    if (hooks != HookOk && hooks != HookAlreadyInitialized) {
        logging::log(logging::Level::error, Channel::hooks, "Hook service failed: {} ({})", hook_status_string(hooks), static_cast<long>(hooks));
        return false;
    }
    logging::write(logging::Level::info, Channel::hooks, "Microsoft Detours hook service ready.");
    // The memory ledger, when RESKATE_MEMORY_TRACE asks for it: here, while the game is still
    // suspended and before the pools below are enlarged, so it sees who makes all of them.
    console::start_memory_trace_from_environment();
    // (Not needed for the game to run: a mismatch leaves crash reports at what the game writes.)
    error.clear(); ready = start_game_fatal_dump(base, error);
    (void)stage(Channel::runtime, "Crash dumps from the game's own fatal handlers", ready, error);
    error.clear(); ready = start_native_patch_support(base, error);
    if (!stage(Channel::assets, "Native InitFS/package support", ready, error)) return false;
    error.clear(); ready = start_mod_layers(base, error);
    if (!stage(Channel::assets, "Mod layout layers", ready, error)) return false;
    // Before anything that lets a script of the player's own into the game's Lua.
    error.clear(); ready = start_lua_native_block(base, error);
    if (!stage(Channel::assets, "Lua kept to Lua (no DLLs, programs, file writes or bytecode)", ready, error)) return false;
    error.clear(); ready = start_loose_files(base, error);
    if (!stage(Channel::assets, "Loose Lua/config files", ready, error)) return false;
    error.clear(); ready = start_custom_script_loader(base, error);
    if (!stage(Channel::assets, "Custom Lua scripts", ready, error)) return false;
    error.clear(); ready = start_morph_memory_pool(base, error);
    if (!stage(Channel::assets, "Morph memory pools", ready, error)) return false;
    error.clear(); ready = start_native_render_resource_pool(base, error);
    if (!stage(Channel::assets, "Native render-resource pools", ready, error)) return false;
    error.clear(); ready = start_native_entity_pages(base, error);
    if (!stage(Channel::world, "Native entity pages (524288 entity/update slots, 1048576 render slots, 16384 spatial blocks/bucket, 128 MiB arena minimum)", ready, error)) return false;
    error.clear(); ready = start_spatial_cull_ids(base, error);
    if (!stage(Channel::world, "Spatial cull IDs (262144 a queue)", ready, error)) return false;
    // (Diagnostics only: how full those pools get. A mismatch leaves the game as it is.)
    error.clear(); ready = start_world_pool_usage(base, error);
    (void)stage(Channel::world, "World pool usage watch", ready, error);
    error.clear(); ready = start_physics_world_size(base, error);
    if (!stage(Channel::world, "Physics world pools (65000 static bodies)", ready, error)) return false;
    error.clear(); ready = start_native_route_lookahead(base, error);
    if (!stage(Channel::world, "NPC route cycle and endpoint guards", ready, error)) return false;
    // The level unload guard (unload_guard.h) is not installed: stepping over an entry with no
    // asset stopped the crash, but that entry had been registered when the level loaded, and
    // left registered it kept the level from ever finishing its unload (a map change that never
    // ends, which is worse than the crash). It goes back in once it can release what it skips.
    if (!stage(Channel::graphics, "Display startup settings", start_display_settings(base), {})) return false;
    // (Not needed for the game to run: a mismatch leaves the heaps at the game's size, and only
    // the densest levels outgrow them.)
    error.clear(); ready = start_descriptor_heap_size(base, error);
    (void)stage(Channel::graphics, "Descriptor heaps: 1,000,000, with 524,288 a frame", ready, error);
    error.clear(); ready = start_user_data_redirect(error);
    if (!stage(Channel::runtime, "Separate game user data", ready, error)) return false;
    error.clear(); ready = start_ea_app_block(error);
    if (!stage(Channel::runtime, "EA app launch block", ready, error)) return false;
    error.clear(); ready = start_ea_service_block(error);
    if (!stage(Channel::runtime, "EA online services block", ready, error)) return false;
    // (Not needed for the game to run: a mismatch leaves the arena at the game's size.)
    error.clear(); ready = start_eadp_errors_arena(base, error);
    (void)stage(Channel::runtime, "EA error-report arena capped at 32 MiB", ready, error);
    error.clear(); ready = start_steam_restart_guard(error);
    return stage(Channel::runtime, launcher::offline_mode() ? "Offline Steam" : "Steam restart guard", ready, error);
}
}
