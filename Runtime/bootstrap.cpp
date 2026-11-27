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
#include "Extension/Rendering/frame_generation_retain.h"
#include "Extension/Boot/game_fatal_dump.h"
#include "Extension/Boot/eadp_errors_arena.h"
#include "Extension/Assets/native_patch_support.h"
#include "Extension/Assets/mod_layers.h"
#include "Extension/Assets/loose_files.h"
#include "Extension/Scripting/lua_native_block.h"
#include "Extension/Assets/morph_memory_pool.h"
#include "Extension/Assets/native_render_resource_pool.h"
#include "Extension/Rendering/display_startup.h"
#include "Extension/World/native_route_lookahead.h"
#include "Extension/World/native_entity_pages.h"
#include "Extension/World/world_pool_usage.h"
#include "Extension/World/physics_world_size.h"
#include "Extension/World/spatial_cull_ids.h"
#include <algorithm>
#include <format>
#include <string>

namespace dingosdk::runtime {
bool initialize_bootstrap(std::uintptr_t base, std::string& failure) {
    if (!base) {
        failure = "Bootstrap started without the game image.";
        logging::write(logging::Level::error, logging::Channel::runtime, failure);
        return false;
    }
    using logging::Channel;
    const auto stage = [&failure](Channel channel, const char* name, bool ready, const std::string& error) {
        const auto line = std::format("{}: {}{}", name, ready ? "ready" : "failed", error.empty() ? "" : " - " + error);
        logging::write(ready ? logging::Level::info : logging::Level::error, channel, line);
        if (!ready) failure = line;
        return ready;
    };
    std::string error;
    bool ready = start_startup_interventions(base, error);
    if (!stage(Channel::runtime, "Startup interventions", ready, error)) return false;
    const auto hooks = hook_initialize();
    if (hooks != HookOk && hooks != HookAlreadyInitialized) {
        failure = std::format("Hook service failed: {} ({})", hook_status_string(hooks), static_cast<long>(hooks));
        logging::write(logging::Level::error, Channel::hooks, failure);
        return false;
    }
    logging::write(logging::Level::info, Channel::hooks, "Microsoft Detours hook service ready.");
    // The service waits for a thread that is ending and starts its transaction over without a
    // word (hooks.cpp, exiting_wait_ms). Said here, so a log shows when an install met one and
    // what the wait cost. A transaction that failed all the same is a warning: its caller says
    // which hook it was.
    hook_observe_retries([](const HookRetries& met, HookStatus result) {
        const auto cost = std::format("started over {} time(s), waited {:.3f} ms in all, {:.3f} ms at the longest",
            met.tries, static_cast<double>(met.waited_us) / 1000.0, static_cast<double>(met.longest_wait_us) / 1000.0);
        if (result == HookOk)
            logging::write(logging::Level::info, logging::Channel::hooks, std::format(
                "A hook transaction waited for a thread that was ending (last: thread {}): {}.", met.thread_id, cost));
        else
            logging::write(logging::Level::warning, logging::Channel::hooks, std::format(
                "A hook transaction failed after it met a thread that refused suspension (last: thread {}): {}; {}.",
                met.thread_id, cost, hook_status_string(result)));
    });
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
    if (!stage(Channel::graphics, "Display startup settings", start_display_settings(base), {})) return false;
    // (Not needed for the game to run: a mismatch leaves the heaps at the game's size, and only
    // the densest levels outgrow them.)
    error.clear(); ready = start_descriptor_heap_size(base, error);
    (void)stage(Channel::graphics, "Descriptor heaps: 1,000,000, with 524,288 a frame", ready, error);
    // (Not needed for the game to run: a mismatch leaves frame generation remaking its resources at
    // each of the game's one-frame offs, the stalls it had before.)
    error.clear(); ready = start_frame_generation_retain(base, error);
    (void)stage(Channel::graphics, "Frame generation keeps its resources through one-frame offs", ready, error);
    {
        const auto single_frame = std::min(frame_descriptor_cap, (heap_descriptor_total - persistent_descriptors) / frames_in_flight);
        const auto line = single_heap_layout
            ? std::format("Descriptor heaps: one heap of {}, with {} a frame", heap_descriptor_total, single_frame)
            : std::format("Descriptor heaps: {} each, with {} a frame", persistent_descriptors + split_frame_descriptor_cap,
                  split_frame_descriptor_cap);
        (void)stage(Channel::graphics, line.c_str(), ready, error);
    }
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
