#include "bootstrap.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/Boot/startup_interventions.h"
#include "Extension/Boot/steam_restart_guard.h"
#include "Extension/Boot/user_data_redirect.h"
#include "Extension/Boot/ea_app_block.h"
#include "Extension/Boot/ea_service_block.h"
#include "Extension/Assets/native_patch_support.h"
#include "Extension/Assets/mod_layers.h"
#include "Extension/Assets/loose_files.h"
#include "Extension/Scripting/custom_script_loader.h"
#include "Extension/Assets/morph_memory_pool.h"
#include "Extension/Assets/native_render_resource_pool.h"
#include "Extension/Rendering/display_startup.h"
#include "Extension/World/native_route_lookahead.h"
#include "Extension/World/native_entity_pages.h"
#include "Extension/World/physics_world_size.h"
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
    error.clear(); ready = start_native_patch_support(base, error);
    if (!stage(Channel::assets, "Native InitFS/package support", ready, error)) return false;
    error.clear(); ready = start_mod_layers(base, error);
    if (!stage(Channel::assets, "Mod layout layers", ready, error)) return false;
    error.clear(); ready = start_loose_files(base, error);
    if (!stage(Channel::assets, "Loose Lua/config files", ready, error)) return false;
    error.clear(); ready = start_custom_script_loader(base, error);
    if (!stage(Channel::assets, "Custom Lua scripts", ready, error)) return false;
    error.clear(); ready = start_morph_memory_pool(base, error);
    if (!stage(Channel::assets, "Morph memory pools", ready, error)) return false;
    error.clear(); ready = start_native_render_resource_pool(base, error);
    if (!stage(Channel::assets, "Native render-resource pools", ready, error)) return false;
    error.clear(); ready = start_native_entity_pages(base, error);
    if (!stage(Channel::world, "Native entity pages (1048576 entity/update/render slots, 16384 spatial blocks/bucket, 128 MiB arena minimum)", ready, error)) return false;
    error.clear(); ready = start_physics_world_size(base, error);
    if (!stage(Channel::world, "Physics world pools (65000 static bodies)", ready, error)) return false;
    error.clear(); ready = start_native_route_lookahead(base, error);
    if (!stage(Channel::world, "NPC route cycle and endpoint guards", ready, error)) return false;
    // The level unload guard (unload_guard.h) is not installed: stepping over an entry with no
    // asset stopped the crash, but that entry had been registered when the level loaded, and
    // left registered it kept the level from ever finishing its unload (a map change that never
    // ends, which is worse than the crash). It goes back in once it can release what it skips.
    if (!stage(Channel::graphics, "Display startup settings", start_display_settings(base), {})) return false;
    error.clear(); ready = start_user_data_redirect(error);
    if (!stage(Channel::runtime, "Separate game user data", ready, error)) return false;
    error.clear(); ready = start_ea_app_block(error);
    if (!stage(Channel::runtime, "EA app launch block", ready, error)) return false;
    error.clear(); ready = start_ea_service_block(error);
    if (!stage(Channel::runtime, "EA online services block", ready, error)) return false;
    error.clear(); ready = start_steam_restart_guard(error);
    return stage(Channel::runtime, launcher::offline_mode() ? "Offline Steam" : "Steam restart guard", ready, error);
}
}
