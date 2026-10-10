#include "runtime_internal.h"
#include "Extension/Customization/developer_hoodie.h"
#include "Extension/Customization/developer_board.h"
#include "Extension/Assets/live_mods.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Settings/job_spin.h"
#include "Engine/Game/World/client_state.h"
#include "Extension/HallOfMeat/hall_of_meat.h"
#include "Extension/Skater/camera_observer.h"
#include "Extension/UI/NativeMenu/native_menu.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Boot/discord_presence.h"
#include "Engine/Game/World/world_names.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/Multiplayer/Hud/follow_camera.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/Multiplayer/Steam/steam_friend_join.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Progression/fixed_stop_entitlement_provider.h"
#include "Extension/Progression/mission_progression_override.h"
#include "Extension/Progression/neighborhood_unlock_override.h"
#include "Extension/Settings/gameplay_settings_override.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/ai_skaters.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/skater_slot_override.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/World/level_loading.h"
#include "Extension/World/loading_screen.h"
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

namespace dingosdk::runtime::detail {
namespace {
using HostedSelector = bool (*)(const char*);
const char* state_name(DWORD state) { return dingosdk::client_state_name(state); }
// What update_model learned about this tick's client, reused by the rest of the
// tick instead of reading the same native chains again.
struct TickState {
    bool valid{};
    DWORD state{}, game_type{};
    std::optional<bool> context;             // native_context
    std::optional<const char*> camera_issue; // camera_probe_unavailable_reason, nullptr when ready
};
void refresh_interactive_model(std::uintptr_t client, DWORD state, DWORD game_type) {
    auto& r = runtime();
    auto world = dingosdk::local_profile_world_layers();
    bool ready{};
    {
        std::lock_guard lock(r.mutex);
        ready = !r.observer_failed && !r.requests.loading() && r.model.can_queue_load &&
            state == r.previous_state && (state == 13 || state == 21) && world.ready;
    }
    const auto map = ready && native_context(r.base, client, game_type) ? world.map : dingosdk::WorldMap::none;
    // Commands already run on the verified client thread. Apply their settings
    // and publish the result now, without waiting for the 500 ms catalog scan.
    dingosdk::update_local_world_controls(map);
    dingosdk::update_local_graphics_controls();
    auto controls = dingosdk::local_profile_world_controls();
    auto graphics = dingosdk::local_profile_graphics_controls();
    auto parks = dingosdk::local_profile_parks();
    auto progression = dingosdk::local_profile_progression();
    auto player_card = dingosdk::local_profile_player_card();
    auto object_persistence = dingosdk::local_profile_object_persistence();
    auto editor = dingosdk::local_park_editor();
    auto bindings = dingosdk::local_profile_controller_bindings();
    const auto missions = dingosdk::local_profile_missions();
    std::vector<dingosdk::overlay::MissionRow> rows;
    for (const auto& row : missions.rows) rows.push_back({row.id, row.group, row.completed});
    std::lock_guard lock(r.mutex);
    r.model.world = std::move(world);
    r.model.world_controls = std::move(controls);
    r.model.graphics = std::move(graphics);
    r.model.parks = std::move(parks);
    r.model.progression = std::move(progression);
    r.model.player_card = std::move(player_card);
    r.model.object_persistence = std::move(object_persistence);
    r.model.editor = std::move(editor);
    r.model.bindings = std::move(bindings);
    r.model.missions_available = missions.available;
    r.model.mission_feedback = missions.feedback;
    r.model.missions = std::move(rows);
    ++r.model_revision;
}
// Engine settings that cost simulation time for nothing here, set once through the named
// setter (retried until the settings registry answers):
// - telemetry and the performance tracker only feed EA's backend, absent offline;
// - EA's error reporting (EadpErrorsData) only builds reports for EA's servers, which
//   ea_service_block keeps unreachable. Set here because the same values given on the game's
//   command line do not take;
// - the world-transform update, which grows with every remote player's entities, is split
//   over more jobs than Game.cfg's 2.
void apply_performance_settings() {
    static bool applied = false;
    static ULONGLONG next_attempt = 0;
    if (applied) return;
    const auto now = GetTickCount64();
    if (now < next_attempt) return;
    next_attempt = now + 500;
    try {
        const auto jobs = std::to_string(std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u));
        const std::array<std::pair<const char*, std::string>, 10> settings{{
            {"DingoTelemetry.Enable", "0"},
            {"DingoTelemetry.EnablePlayerTickEvents", "0"},
            {"DingoTelemetry.EnablePerformanceEvents", "0"},
            {"DingoTelemetry.EnableCPUBenchmark", "0"},
            {"EadpErrorsData.DisableAllEventsReporting", "1"},
            {"EadpErrorsData.EnableReportCrashes", "0"},
            {"EadpErrorsData.AutoSendEventFromPersistence", "0"},
            {"PerformanceTracker.Enabled", "0"},
            {"PerformanceTracker.JuiceLogPerformance", "0"},
            {"EcsWorldTransform.ParallelWorldTransformUpdateJobCount", jobs},
        }};
        bool pending = false;
        std::string results;
        for (const auto& [name, value] : settings) {
            const auto result = dingosdk::change_named_setting(name, value, false);
            pending = pending || result.starts_with("error: ");
            results += (results.empty() ? "" : " | ") + result;
        }
        if (!pending) {
            applied = true;
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
                "Performance settings: {}", results);
        }
    } catch (...) {}
}
// The mesh streaming pool holds every render mesh the world has loaded, in video memory.
// GraphicsPC.lua fixes it at 532960 KB (520 MB) on every PC quality level, sized for San Van's
// streamed cells; a big custom map's meshes do not fit, so its level either never finishes
// loading or waits out the transition's 180 s world-quality timeout
// (MeshStreamingLoadStateProvider never reports high) and spawns the skater invisible. The pools
// are re-created from this setting at every level start (FUN_14507a2d0), so raising it before a
// load is enough; the quality script sets it again whenever graphics settings are applied, so it
// is kept here. The engine turns it into bytes in 32 bits (PoolSize << 10, plus
// PoolHeadroomSize), so the combined allocation must remain below 4 GiB.
// How far it is raised depends on the card: 1.0.0 set 3.5 GiB on every card, the likely cause
// of older cards resetting the device while loading (DXGI_ERROR_DEVICE_RESET in
// gameRendBeginFrame on an RX 580, 2026-10-03). A quarter of the card's dedicated memory, from
// the game's own 520 MB up to 3.5 GiB (from 16 GB cards up); cards of 6 GB or less keep the
// game's value. Cards of up to 12 GB stop at 2 GiB: a quarter was 3 GiB of a 12 GB card, and
// with a big map and texture mods on top the game ran out of video memory (E_OUTOFMEMORY from
// CreateCommittedResource on an RTX 4070 SUPER, 2026-10-04).
namespace mesh_pool {
constexpr std::uint32_t stock_kb = 532960, most_kb = 3584u * 1024u, most_to_12gb_kb = 2048u * 1024u;
static_assert((std::uint64_t{most_kb} + 24576u) * 1024u <= 0xffffffffull);
struct Card { std::uint64_t memory{}; std::string name; };
// The hardware adapter with the most dedicated memory: the one the game renders on (a laptop's
// integrated GPU has little or none). The game's own dxgi.dll, so nothing new is loaded.
Card largest_card() {
    Card card;
    const auto module = GetModuleHandleW(L"dxgi.dll");
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory{};
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return card;
    IDXGIAdapter1* adapter{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            desc.DedicatedVideoMemory > card.memory) {
            card.memory = desc.DedicatedVideoMemory;
            card.name.clear();
            for (const auto* c = desc.Description; *c; ++c) card.name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        }
        adapter->Release();
    }
    factory->Release();
    return card;
}
// KiB for the pool on this card, or 0 to keep the game's own value.
std::uint32_t size_kb(std::uint64_t memory) {
    // Cards report a little under or over their nominal size.
    constexpr std::uint64_t six_gib = 6ull << 30, twelve_gib = 12ull << 30, slack = 256ull << 20;
    if (memory <= six_gib + slack) return 0;
    const auto most = memory <= twelve_gib + slack ? most_to_12gb_kb : most_kb;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(memory / 4 / 1024, stock_kb, most));
}
}
void apply_mesh_streaming_pool() {
    static const auto card = mesh_pool::largest_card();
    static const auto pool_kb = mesh_pool::size_kb(card.memory);
    static bool reported = false;
    if (!reported) {
        reported = true;
        dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
            "Mesh streaming pool: {} for {} ({:.1f} GB of video memory).",
            pool_kb ? std::format("{} MB", pool_kb / 1024) : std::string("the game's own 520 MB"),
            card.name.empty() ? std::string("an unknown card") : card.name, static_cast<double>(card.memory) / (1ull << 30));
    }
    if (!pool_kb) return;
    static ULONGLONG next_check = 0;
    static std::string last_result;
    const auto now = GetTickCount64();
    if (now < next_check) return;
    next_check = now + 2000;
    try {
        const auto result = dingosdk::change_named_setting("MeshStreaming.PoolSize", std::to_string(pool_kb), false);
        if (result.starts_with("error: ")) { next_check = now + 500; return; }
        if (result != last_result && !result.ends_with("(unchanged)"))
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
                "Mesh streaming pool raised for big custom maps (applies from the next level load): {}", result);
        last_result = result;
    } catch (...) {}
}
void apply_throwdown_modes() {
    // The throwdowner flow picks between the mode-select page and straight Jam
    // placement by reading these by name (GetBoolSetting). Retail receives them
    // from live config; S.K.A.T.E. ships off in the native constructor. Go
    // through the named setter so registered setting listeners are notified.
    static bool applied = false;
    static ULONGLONG next_attempt = 0;
    static std::string last_result;
    if (applied) return;
    const auto now = GetTickCount64();
    if (now < next_attempt) return;
    next_attempt = now + 500;
    try {
        bool pending = false;
        std::string results;
        // EnableAdvancedParameters adds the host's Customize button and the
        // parameter panel (players, timer, privacy, scored actions) to the
        // throwdown details page; it also ships off.
        for (const auto* name : {"DingoThrowdowns.EnableSpotBattle", "DingoThrowdowns.EnableSKATE",
                                 "DingoThrowdowns.EnableAdvancedParameters"}) {
            const auto result = dingosdk::change_named_setting(name, "1", false);
            pending = pending || result.starts_with("error: ");
            results += (results.empty() ? "" : " | ") + result;
        }
        {
            // Local play has nobody to wait for: with the single-player queue
            // enabled (native_throwdowns.cpp) the 120 s join countdown would only
            // delay the host. Revisit when throwdowns are relayed to other players.
            const auto result = dingosdk::change_named_setting("DingoThrowdowns.QueueStartTimer", "1", false);
            pending = pending || result.starts_with("error: ");
            results += " | " + result;
        }
        if (results != last_result) {
            last_result = results;
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::progression,
                "Throwdown modes: {}", results);
        }
        applied = !pending;
    } catch (...) {}
}
void update_model(std::uintptr_t client, TickState& frame) {
    auto& r = runtime();
    std::uintptr_t vtable{};
    DWORD state{}, game_type{};
    // Every client tick: peeked, not read (three ReadProcessMemory calls a tick, profiled 2026-10-02).
    if (!memory::peek(client, vtable) || vtable != r.base + engine::client_vtable || client > highest - 0x2a0 ||
        !memory::peek(client + 0xc4, state) || state > 26 || !memory::peek(client + 0xc0, game_type) || game_type > 3) return;
    DWORD no_thread{};
    r.engine_thread.compare_exchange_strong(no_thread, GetCurrentThreadId());
    if (r.engine_thread.load() != GetCurrentThreadId()) return;
    frame.valid = true;
    frame.state = state;
    frame.game_type = game_type;
    // Some two dozen native reads: one result serves the whole tick.
    const auto native_context_ready = [&] {
        if (!frame.context) frame.context = native_context(r.base, client, game_type);
        return *frame.context;
    };
    // Engine settings are only shown by the menu and console; refresh them
    // quickly (and hand them over) only while one of them is reading.
    const auto settings_now = GetTickCount64();
    const bool settings_wanted = settings_now < r.named_settings_wanted_until.load(std::memory_order_relaxed);
    dingosdk::refresh_named_settings(settings_wanted);
    apply_throwdown_modes();
    apply_performance_settings();
    dingosdk::job_spin::apply_default();
    apply_mesh_streaming_pool();
    dingosdk::multiplayer::apply_throwdown_strings(r.base);
    const bool named_context_ready = (state == 13 || state == 21) && native_context_ready();
    {
        std::lock_guard lock(r.mutex);
        r.named_context_ready = named_context_ready;
    }
    if (settings_wanted && settings_now >= r.next_named_settings_publish) {
        r.next_named_settings_publish = settings_now + 250;
        publish_named_settings(r);
    }
    // RESKATE_STARTUP_COMMANDS="load skate1map;noclip 1" (development): console commands
    // queued in order once the world is up and can accept them, so a build can be exercised
    // without anyone driving the overlay. A load waits for its destination to be listed.
    {
        static std::optional<std::vector<std::string>> startup_commands;
        if (!startup_commands) {
            startup_commands.emplace();
            std::wstring value(4096, L'\0');
            const auto length = GetEnvironmentVariableW(L"RESKATE_STARTUP_COMMANDS", value.data(), 4096);
            if (length && length < 4096) {
                std::string narrow;
                for (const auto character : value.substr(0, length))
                    narrow.push_back(character < 128 ? static_cast<char>(character) : '?');
                for (std::size_t start = 0; start <= narrow.size();) {
                    auto end = narrow.find(';', start);
                    if (end == std::string::npos) end = narrow.size();
                    auto piece = narrow.substr(start, end - start);
                    const auto first = piece.find_first_not_of(" \t");
                    const auto last = piece.find_last_not_of(" \t");
                    if (first != std::string::npos) startup_commands->push_back(piece.substr(first, last - first + 1));
                    start = end + 1;
                }
            }
        }
        if (!startup_commands->empty() && named_context_ready) {
            std::lock_guard lock(r.mutex);
            const auto& text = startup_commands->front();
            const auto lower = [](std::string value) {
                for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                return value;
            };
            // "wait <seconds>" pauses the list, e.g. to let a multiplayer join finish.
            static std::optional<std::chrono::steady_clock::time_point> wait_until;
            const bool wait = lower(text).starts_with("wait ");
            if (wait) {
                const auto now = std::chrono::steady_clock::now();
                if (!wait_until) wait_until = now + std::chrono::milliseconds(static_cast<int>(1000 * std::atof(text.c_str() + 5)));
                if (now >= *wait_until) {
                    wait_until.reset();
                    startup_commands->erase(startup_commands->begin());
                }
            }
            bool ready = !wait && r.model.can_queue_load && !r.requests.loading();
            if (ready && lower(text).starts_with("load ")) {
                const auto wanted = lower(text.substr(5, text.find(' ', 5) == std::string::npos ? std::string::npos : text.find(' ', 5) - 5));
                ready = std::ranges::any_of(r.model.levels, [&](const dingosdk::overlay::Level& level) {
                    return level.can_load && (lower(level.asset).find(wanted) != std::string::npos ||
                                              lower(level.display_name) == wanted);
                });
            }
            if (ready && r.requests.enqueue(ConsoleRequest{text}, request_context(r), GetCurrentThreadId())) {
                dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::level,
                    "Startup command queued: {}", text);
                startup_commands->erase(startup_commands->begin());
            }
        }
    }
    std::optional<ConsoleRequest> console_command;
    {
        std::lock_guard lock(r.mutex);
        console_command = r.requests.take<ConsoleRequest>(GetCurrentThreadId());
    }
    if (console_command) {
        ScheduledWorkScope work{r, true};
        const auto started = std::chrono::steady_clock::now();
        execute_console_command(console_command->text);
        const auto executed = std::chrono::steady_clock::now();
        refresh_interactive_model(client, state, game_type);
        const auto published = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
        record("{\"event\":\"menu_command_timing\",\"queue_ms\":" +
            std::to_string(milliseconds(started - console_command->queued_at)) + ",\"execute_ms\":" +
            std::to_string(milliseconds(executed - started)) + ",\"publish_ms\":" +
            std::to_string(milliseconds(published - executed)) + "}");
    }
    const auto now = GetTickCount64();
    bool has_request{};
    { std::lock_guard lock(r.mutex); has_request = r.requests.pending_load(); }
    // Camera phase validation is necessary before camera/motion operations,
    // not on every idle frame. Validate afresh after any action in this tick
    // that can have changed the native camera's identity (frame.camera_issue is
    // cleared then); until one runs, the first check serves the whole tick.
    const auto camera_phase_issue = [&]() -> const char* {
        if (!frame.camera_issue) {
            std::uintptr_t context{};
            frame.camera_issue = read(client + 8, context) ? dingosdk::camera_probe_unavailable_reason(context) :
                "Waiting for the local camera context.";
        }
        return *frame.camera_issue;
    };
    const auto camera_phase_ready = [&] { return camera_phase_issue() == nullptr; };
    std::optional<dingosdk::overlay::DebugRequest> debug_request;
    bool debug_busy{}, controller_busy{};
    std::uint32_t freecam_controller_combo{}, freecam_combo{}, tp_to_freecam_combo{}, noclip_combo{}, forward_velocity_combo{}, up_velocity_combo{}, offboard_up_velocity_combo{};
    // Yes and No in a dedicated server's vote: read only while one is running.
    std::uint32_t vote_yes_combo{}, vote_no_combo{};
    // The switches on buttons (action_binds): each runs its console command.
    std::array<std::uint32_t, dingosdk::action_binds.size()> action_combos{};
    const bool vote_open = dingosdk::multiplayer::server_vote_open();
    const unsigned poll_answers = dingosdk::multiplayer::server_poll_answers();
    bool freecam_controller = dingosdk::local_freecam_controller();
    {
        std::lock_guard lock(r.mutex);
        debug_busy = r.requests.loading();
        controller_busy = !r.requests.idle();
        freecam_controller_combo = r.model.bindings.available ? r.model.bindings.freecam_controller_combo : 0;
        freecam_combo = r.model.bindings.available ? r.model.bindings.freecam_combo : 0;
        tp_to_freecam_combo = r.model.bindings.available ? r.model.bindings.tp_to_freecam_combo : 0;
        noclip_combo = r.model.bindings.available ? r.model.bindings.noclip_combo : 0;
        forward_velocity_combo = r.model.bindings.available ? r.model.bindings.forward_velocity_combo : 0;
        up_velocity_combo = r.model.bindings.available ? r.model.bindings.up_velocity_combo : 0;
        offboard_up_velocity_combo = r.model.bindings.available ? r.model.bindings.offboard_up_velocity_combo : 0;
        if (r.model.bindings.available) action_combos = r.model.bindings.action_combos;
        if (vote_open && r.model.bindings.available) {
            vote_yes_combo = r.model.bindings.vote_yes_combo;
            vote_no_combo = r.model.bindings.vote_no_combo;
        }
        debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
    }
    // Capture belongs to the freecam state itself. Keep it active across brief
    // request/loading phases instead of opening a path back to player input.
    DingoSDKOverlaySetFreecamInputCapture(dingosdk::client_free_camera_active() && freecam_controller);
    dingosdk::ControllerInput controller;
    if (freecam_controller_combo || freecam_combo || tp_to_freecam_combo || noclip_combo || forward_velocity_combo || up_velocity_combo || offboard_up_velocity_combo ||
        vote_yes_combo || vote_no_combo || poll_answers || std::ranges::any_of(action_combos, [](auto combo) { return combo != 0; }))
        DingoSDKOverlayReadControllerInput(&controller);
    for (std::size_t i = 0; i < action_combos.size(); ++i)
        if (r.action_bind_latches[i].update(action_combos[i], controller, r.observer_failed)) {
            std::array<char, 256> result{};
            const std::string command(dingosdk::action_binds[i].command);
            if (queue_console_command(nullptr, command.c_str(), result.data(), result.size()))
                record(("{\"event\":\"controller_binding_triggered\",\"action\":\"" + std::string(dingosdk::action_binds[i].key) + "\"}").c_str());
        }
    // (The input reads as nothing while the menu, the console or the chat box is open, so typing
    // a bound key answers no vote.)
    if (r.vote_yes_bind_latch.update(vote_yes_combo, controller, !vote_open) && dingosdk::multiplayer::queue_command("vote", "yes", ""))
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"vote_yes\"}");
    if (r.vote_no_bind_latch.update(vote_no_combo, controller, !vote_open) && dingosdk::multiplayer::queue_command("vote", "no", ""))
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"vote_no\"}");
    // A poll's answers are on the number keys, 1 for the first: only while one is running, so
    // the keys are the game's own the rest of the time.
    for (unsigned answer = 0; answer < r.poll_answer_latches.size(); ++answer) {
        const std::uint32_t key = answer < poll_answers ? dingosdk::keyboard_binding_tag | ('1' + answer) : 0U;
        const char number[2]{static_cast<char>('1' + answer), 0};
        if (r.poll_answer_latches[answer].update(key, controller, !poll_answers) && dingosdk::multiplayer::queue_command("vote", number, ""))
            record("{\"event\":\"controller_binding_triggered\",\"action\":\"poll_answer\"}");
    }
    
    if (r.freecam_controller_bind_latch.update(freecam_controller_combo, controller, r.observer_failed || controller_busy || !r.debug_model.free_camera)) {
        if (dingosdk::set_local_freecam_controller(!freecam_controller))
            freecam_controller = !freecam_controller;
        DingoSDKOverlaySetFreecamInputCapture(dingosdk::client_free_camera_active() && freecam_controller);
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"freecam_controller\"}");
    }
    
    if (r.tp_to_freecam_bind_latch.update(tp_to_freecam_combo, controller, r.observer_failed || controller_busy || !r.debug_model.free_camera || !r.debug_model.camera_position_valid || debug_request.has_value())) {
        if (dingosdk::teleport_local_skater(r.debug_model.camera_position)) {
            std::lock_guard lock(r.mutex);
            const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::set_free_camera, false};
            if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId()))
                debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
        }
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"tp_to_freecam\"}");
    }
    
    const bool freecam_bind_blocked = r.observer_failed || controller_busy || debug_request.has_value() ||
        (state != 13 && state != 21) || !r.debug_model.camera_available ||
        (freecam_combo && !camera_phase_ready());
    if (r.freecam_bind_latch.update(freecam_combo, controller, freecam_bind_blocked)) {
        {
            std::lock_guard lock(r.mutex);
            const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::set_free_camera, !r.debug_model.free_camera};
            if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId()))
                debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
        }
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"freecam\"}");
    }

    const bool bind_blocked = r.observer_failed || controller_busy || debug_request.has_value() ||
        (state != 13 && state != 21) || !r.debug_model.noclip_available ||
        (noclip_combo && !camera_phase_ready());
    const bool noclip_triggered = r.noclip_bind_latch.update(noclip_combo, controller, bind_blocked);
    if (noclip_triggered) {
        {
            std::lock_guard lock(r.mutex);
            const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::set_noclip, !r.debug_model.noclip};
            if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId()))
                debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
        }
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"noclip\"}");
    }
    const bool forward_velocity_blocked = r.observer_failed || controller_busy || debug_request.has_value() ||
        (r.debug_model.offboard_up_velocity_available && forward_velocity_combo == offboard_up_velocity_combo) ||
        (r.debug_model.free_camera && freecam_controller) ||
        (state != 13 && state != 21) || !r.debug_model.forward_velocity_available ||
        (forward_velocity_combo && forward_velocity_combo == noclip_combo);
    if (r.forward_velocity_bind_latch.update(forward_velocity_combo, controller, forward_velocity_blocked)) {
        {
            std::lock_guard lock(r.mutex);
            const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::add_forward_velocity};
            if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId()))
                debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
        }
        record("{\"event\":\"controller_binding_triggered\",\"action\":\"forward_velocity\"}");
    }
    const bool up_velocity_blocked = r.observer_failed || controller_busy || debug_request.has_value() ||
        (r.debug_model.offboard_up_velocity_available && up_velocity_combo == offboard_up_velocity_combo) ||
        (r.debug_model.free_camera && freecam_controller) ||
        (state != 13 && state != 21) || !r.debug_model.up_velocity_available ||
        (up_velocity_combo && (up_velocity_combo == noclip_combo || up_velocity_combo == forward_velocity_combo));
    if (r.up_velocity_bind_latch.update(up_velocity_combo, controller, up_velocity_blocked)) {
        std::lock_guard lock(r.mutex);
        const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::add_up_velocity};
        if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId())) {
            debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
            record("{\"event\":\"controller_binding_triggered\",\"action\":\"up_velocity\"}");
        }
    }

    const bool offboard_up_velocity_blocked = r.observer_failed || controller_busy || debug_request.has_value() ||
        (r.debug_model.free_camera && freecam_controller) ||
        (state != 13 && state != 21) || !r.debug_model.offboard_up_velocity_available ||
        (offboard_up_velocity_combo && offboard_up_velocity_combo == noclip_combo);
    if (r.offboard_up_velocity_bind_latch.update(offboard_up_velocity_combo, controller, offboard_up_velocity_blocked)) {
        std::lock_guard lock(r.mutex);
        const dingosdk::overlay::DebugRequest request{dingosdk::overlay::DebugAction::add_offboard_up_velocity};
        if (r.requests.enqueue(request, request_context(r), GetCurrentThreadId())) {
            debug_request = r.requests.take<dingosdk::overlay::DebugRequest>(GetCurrentThreadId());
            record("{\"event\":\"controller_binding_triggered\",\"action\":\"offboard_up_velocity\"}");
        }
    }
    if (debug_request || r.debug_model.free_camera || r.debug_model.first_person || r.debug_model.noclip ||
        now >= r.next_debug) {
        ScheduledWorkScope work{r, debug_request.has_value()};
        r.next_debug = now + 100;
        const bool debug_ready = !r.observer_failed && !debug_busy && (state == 13 || state == 21) && native_context_ready();
        dingosdk::overlay::FlightInput flight_input;
        DingoSDKOverlayReadFlightInput(&flight_input, (r.debug_model.free_camera || r.debug_model.noclip) && debug_ready,
            r.debug_model.noclip || (r.debug_model.free_camera && freecam_controller));
        const auto phase_issue = camera_phase_issue();
        auto debug = dingosdk::on_client_debug_tick(r.base, client, debug_ready,
            phase_issue == nullptr,
            debug_request ? &*debug_request : nullptr, &flight_input);
        // Input ownership must follow the live camera even with every overlay closed.
        DingoSDKOverlaySetFreecamInputCapture(dingosdk::client_free_camera_active() && freecam_controller);
        // A debug action, or a mode the tick ended by itself (host policy, lost
        // camera), can have replaced the native camera: validate it again.
        if (debug_request || debug.free_camera != r.debug_model.free_camera ||
            debug.first_person != r.debug_model.first_person || debug.noclip != r.debug_model.noclip ||
            debug.park_editor != r.debug_model.park_editor)
            frame.camera_issue.reset();
        if (debug_ready && phase_issue && !debug.camera_available) {
            debug.camera_unavailable = phase_issue;
            debug.noclip_unavailable = phase_issue;
        }
        // Ignore brief loading/ownership transitions. Report a persistent reason
        // once, then report recovery; the menu always shows the current reason.
        const std::string issue = debug_ready && debug.skater_position_valid && !debug.noclip_available
            ? debug.noclip_unavailable : std::string{};
        if (issue != r.flight_unavailable_issue) {
            r.flight_unavailable_issue = issue;
            r.flight_unavailable_since = now;
        }
        if (!issue.empty() && now - r.flight_unavailable_since >= 2000 && issue != r.flight_unavailable_logged) {
            dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::skater,
                "{} unavailable: {}", debug.camera_available ? "Noclip" : "Freecam and Noclip", issue);
            r.flight_unavailable_logged = issue;
        } else if (debug.noclip_available && !r.flight_unavailable_logged.empty()) {
            dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::skater,
                "Freecam and Noclip are available again.");
            r.flight_unavailable_logged.clear();
        }
        if (debug.noclip_velocity_updates && !r.debug_model.noclip_velocity_updates)
            record("{\"event\":\"noclip_velocity_applied\",\"count\":" + std::to_string(debug.noclip_velocity_updates) + "}");
        if (debug.noclip_motion_updates && !r.debug_model.noclip_motion_updates)
            record("{\"event\":\"noclip_offboard_motion_applied\",\"count\":" + std::to_string(debug.noclip_motion_updates) + "}");
        if (debug.forward_velocity_updates != r.debug_model.forward_velocity_updates)
            record("{\"event\":\"forward_velocity_applied\",\"count\":" + std::to_string(debug.forward_velocity_updates) + "}");
        if (debug.up_velocity_updates != r.debug_model.up_velocity_updates)
            record("{\"event\":\"up_velocity_applied\",\"count\":" + std::to_string(debug.up_velocity_updates) + "}");
        if (debug.offboard_up_velocity_updates != r.debug_model.offboard_up_velocity_updates)
            record("{\"event\":\"offboard_up_velocity_applied\",\"count\":" + std::to_string(debug.offboard_up_velocity_updates) + "}");
        if (r.debug_model.noclip && !debug.noclip && !debug_request) {
            std::ostringstream stopped;
            stopped << "{\"event\":\"noclip_stopped\",\"reason\":" << std::quoted(debug.status) << '}';
            record(stopped.str());
        }
        if (debug_request) record("{\"event\":\"interactive_debug_result\",\"action\":" +
            std::to_string(static_cast<int>(debug_request->action)) + ",\"free_camera\":" +
            (debug.free_camera ? "true" : "false") + ",\"noclip\":" + (debug.noclip ? "true" : "false") + ",\"game_ui_hidden\":" +
            (debug.game_ui_hidden ? "true" : "false") + ",\"no_bail\":" + (debug.no_bail ? "true" : "false") +
            ",\"no_bail_active\":" + (debug.no_bail_active ? "true" : "false") + ",\"speed\":" + std::to_string(debug.camera_speed) +
            ",\"velocity_updates\":" + std::to_string(debug.noclip_velocity_updates) + ",\"motion_updates\":" + std::to_string(debug.noclip_motion_updates) + "}");
        if (debug_request) {
            std::ostringstream message;
            message << "Noclip " << (debug.noclip ? "on" : "off") << " | free camera " << (debug.free_camera ? "on" : "off")
                << " | No Bail " << (debug.no_bail_active ? "active" : debug.no_bail ? "waiting" : "off")
                << (debug.no_bail_active && !debug.no_bail ? " (noclip)" : "")
                << " | speed " << debug.camera_speed << " | game UI "
                << (debug.game_ui_hidden ? "hidden" : "visible");
            if (!debug.status.empty()) message << " | " << debug.status;
            activity_line(dingosdk::ConsoleSource::runtime, message.str());
        }
        std::lock_guard lock(r.mutex);
        r.debug_model = std::move(debug);
        ++r.debug_revision;
    }
    std::optional<dingosdk::overlay::OfflineFeatureRequest> offline_request;
    {
        std::lock_guard lock(r.mutex);
        offline_request = r.requests.take<dingosdk::overlay::OfflineFeatureRequest>(GetCurrentThreadId());
    }
    if (offline_request || now >= r.next_offline) {
        ScheduledWorkScope work{r, offline_request.has_value()};
        DINGO_PROFILE_ZONE("tick/update_model/features (500 ms)");
        r.next_offline = now + 500;
        auto offline = dingosdk::update_gameplay_settings_override(
            offline_request ? &*offline_request : nullptr);
        if (offline_request) {
            using dingosdk::overlay::OfflineFeatureGroup;
            if (offline_request->group == OfflineFeatureGroup::board_wear)
                dingosdk::profile_runtime::set_local_preference("BoardWear", offline_request->enabled);
            else if (offline_request->group == OfflineFeatureGroup::restore_all)
                dingosdk::profile_runtime::set_local_preference("BoardWear", false);
            dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
                "Offline feature %d -> %d applied: %s (board wear available %d, effective %d).",
                static_cast<int>(offline_request->group), offline_request->enabled, offline.model.status.c_str(),
                offline.model.board_wear.available, offline.model.board_wear.effective);
        }
        auto slot_action = dingosdk::SkaterSlotOverrideAction::tick;
        if (offline_request &&
            offline_request->group == dingosdk::overlay::OfflineFeatureGroup::restore_all)
            slot_action = dingosdk::SkaterSlotOverrideAction::restore;
        const auto profile_access = dingosdk::local_profile_access();
        {
            DINGO_PROFILE_ZONE("tick/update_model/features (500 ms)/local profile");
            dingosdk::update_local_customization();
        }
        // Saved access owns these prerequisites. This runs on the recorded
        // game-update thread and reacquires/validates each native settings object.
        const auto enable_saved_feature = [&](const char* name, bool requested, const char* reapplied_log = nullptr) {
            if (!requested) return;
            const auto variable = std::find_if(offline.model.variables.begin(), offline.model.variables.end(),
                [&](const auto& field) { return field.name == name; });
            if (variable != offline.model.variables.end() && variable->available && !variable->value) {
                offline = dingosdk::update_gameplay_engine_variable({name, dingosdk::EngineVariableAction::set, true});
                if (reapplied_log)
                    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::runtime,
                        "%s", reapplied_log);
            }
        };
        enable_saved_feature("FastTravelPointsEnabled", profile_access.bus_stops);
        enable_saved_feature("EnableNeighborhoodRank", profile_access.neighborhoods || profile_access.neighborhood_ranks);
        enable_saved_feature("EnableCASArtistSandbox", profile_access.cosmetics);
        enable_saved_feature("EnableMyStuffMenu", profile_access.cosmetics);
        enable_saved_feature("BoardWearEnabled",
            dingosdk::profile_runtime::local_preference("BoardWear").value_or(false),
            "Re-applied saved board wear preference (BoardWearEnabled).");
        if (profile_access.preset_slots) slot_action = dingosdk::SkaterSlotOverrideAction::enable;
        const auto slots = dingosdk::update_skater_slot_override(slot_action, dingosdk::local_customization_selected_preset(),
            dingosdk::local_customization_outfits_loadable());
        if (slots.manager_available && slots.ui_ready)
            dingosdk::observe_local_customization_selection(slots.selected_slot);
        const bool missions_requested = offline.model.activities.available &&
            offline.model.activities.effective &&
            offline.model.activities.override_active;
        if (missions_requested)
            dingosdk::arm_main_mission_override(r.base, true);
        else
            dingosdk::restore_main_mission_override();
        const auto missions = dingosdk::main_mission_override_observation();
        const bool fixed_stop_requested = r.fixed_stop_entitlement_route_ready &&
            (profile_access.bus_stops || (offline.model.fast_travel.available &&
            offline.model.fast_travel.effective &&
            offline.model.fast_travel.override_active));
        dingosdk::set_fixed_stop_entitlement_provider_enabled(fixed_stop_requested);
        const bool neighborhood_requested = profile_access.neighborhoods || (offline.model.progression.available &&
            offline.model.progression.effective &&
            offline.model.progression.override_active);
        (void)dingosdk::set_neighborhood_unlock_override_enabled(neighborhood_requested);
        const auto fixed_stops = dingosdk::fixed_stop_entitlement_provider_observation();
        const auto neighborhoods = dingosdk::neighborhood_unlock_override_observation();
        merge_skater_slot_observation(offline.model, slots);
        merge_main_mission_observation(offline.model, missions);
        merge_progression_provider_observations(offline.model, fixed_stops, neighborhoods,
            r.fixed_stop_entitlement_route_ready);
        if (offline.json != r.last_gameplay_settings_override_observation) {
            record(offline.json);
            r.last_gameplay_settings_override_observation = offline.json;
        }
        if (slots.json != r.last_skater_slot_override_observation) {
            record(slots.json);
            r.last_skater_slot_override_observation = slots.json;
        }
        if (missions.json != r.last_main_mission_override_observation) {
            record(missions.json);
            r.last_main_mission_override_observation = missions.json;
        }
        if (fixed_stops.json != r.last_fixed_stop_entitlement_provider_observation) {
            record(fixed_stops.json);
            r.last_fixed_stop_entitlement_provider_observation = fixed_stops.json;
        }
        if (neighborhoods.json != r.last_neighborhood_unlock_override_observation) {
            record(neighborhoods.json);
            r.last_neighborhood_unlock_override_observation = neighborhoods.json;
        }
        std::lock_guard lock(r.mutex);
        r.offline_model = std::move(offline.model);
        ++r.offline_revision;
        // Skater slots, teleports and customization ran: check the camera again.
        frame.camera_issue.reset();
    }
    if (r.debug_model.park_editor && !r.observer_failed && !debug_busy && (state == 13 || state == 21) &&
        native_context_ready()) {
        {
            DINGO_PROFILE_ZONE("tick/update_model/park editor");
            dingosdk::tick_local_park_editor();
        }
        frame.camera_issue.reset();
        auto editor = dingosdk::local_park_editor();
        std::lock_guard lock(r.mutex);
        r.model.editor = std::move(editor);
        ++r.model_revision;
    } else {
        // Other players' objects still to be created: queue the next as soon as the
        // last one is in, not at the 500 ms customization pace.
        DINGO_PROFILE_ZONE("tick/update_model/network objects");
        dingosdk::tick_network_objects();
    }
    if (r.observer_failed) return; // Keep the bounded restore/telemetry path available after catalog failure.
    dingosdk::hall_of_meat::on_client_tick();
    if (!has_request && now < r.next_model && state == r.previous_state) return;
    r.next_model = now + 500;
    DINGO_PROFILE_ZONE("tick/update_model/world model (500 ms)");
    auto description = dingosdk::read_world_description(GetCurrentProcess(), client + 0x198);
    r.multiplayer_map = description.level.empty() ? std::string{} : description.level + "|" + description.lm_level;
    const auto& current_level = description.lm_level.empty() ? description.level : description.lm_level;
    dingosdk::live_mods::set_current_level(current_level);
    dingosdk::hall_of_meat::set_level(current_level);
    // A live mod apply hands over the destinations its mods declare. A map
    // being played that is no longer among them (its mod disabled or deleted)
    // cannot stay loaded: the player goes to San Van.
    bool left_map{}, manifests_applied{};
    if (auto manifests = dingosdk::live_mods::take_level_manifests()) {
        manifests_applied = true;
        auto next = read_custom_levels(*manifests);
        const auto listed = [&](const dingosdk::CustomLevelManifest& set) {
            return std::ranges::any_of(set.levels, [&](const auto& level) { return equals(level.asset, description.lm_level); });
        };
        left_map = !description.lm_level.empty() && listed(r.custom_levels) && !listed(next);
        r.custom_levels = std::move(next);
        dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::level,
            "Custom-level destinations after the live mod apply: {}.", r.custom_levels.levels.size());
    }
    // Handed out with the manifests: once the merge is in, load the level being
    // played again ("Apply and reload level"), or San Van when that map is gone.
    const bool reload = dingosdk::live_mods::take_reload();
    if (left_map || reload) {
        constexpr std::string_view san_van = "levels/game/BAM_LevelRoot/BAM_LevelRoot";
        const std::string target = left_map ? std::string(san_van) : current_level;
        std::lock_guard lock(r.mutex);
        if (target.empty() || !r.requests.enqueue(ConsoleRequest{"load " + target}, request_context(r), GetCurrentThreadId()))
            dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::level,
                "Mods applied, but the level could not be {}; load a level to use them.", left_map ? "left" : "reloaded");
        else if (left_map)
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::level,
                "{} was removed by the mod apply; loading San Van.", description.lm_level);
        else
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::level,
                "Reloading {} to use the applied mods.", target);
    }
    // The native level registry changes only with a mod apply or a level load,
    // and the server's sublevels settle during loads: read both again then, and
    // whenever this client is not in play; otherwise every 10 s and 2 s.
    bool requests_loading{};
    { std::lock_guard lock(r.mutex); requests_loading = r.requests.loading(); }
    const bool world_changed = manifests_applied || requests_loading || (state != 13 && state != 21) ||
        state != r.previous_state || description.level != r.last_level || description.lm_level != r.catalog_level;
    r.catalog_level = description.lm_level;
    if (!r.native_catalog || !r.native_catalog->available || world_changed || now >= r.next_catalog_read) {
        r.native_catalog = dingosdk::read_world_catalog(GetCurrentProcess(), r.base);
        r.next_catalog_read = now + 10000;
    }
    const bool sublevels_read = !r.sublevel_catalog || !r.sublevel_catalog->available || world_changed ||
        now >= r.next_sublevel_read;
    if (sublevels_read) {
        r.sublevel_catalog = dingosdk::read_server_sublevels(GetCurrentProcess(), r.base);
        r.next_sublevel_read = now + 2000;
    }
    auto catalog = *r.native_catalog;
    const auto native_level_count = catalog.levels.size();
    catalog = dingosdk::merge_custom_levels(std::move(catalog), r.custom_levels);
    const auto& sublevels = *r.sublevel_catalog;
    const auto client_content = read_client_content(r.base, client, description.lm_level);
    const bool context_ready = native_context_ready();
    const bool manifest_custom_active = (state == 13 || state == 21) &&
        context_ready && description.available &&
        std::any_of(catalog.levels.begin(), catalog.levels.end(), [&](const auto& level) {
            return level.manifest_only && equals(level.asset, description.lm_level);
        });
    // Schema 1 Studio maps are authored exclusively from BAM's bounded
    // service/environment closure. This hint permits that reduced controller
    // graph to identify its family without weakening native-map detection.
    dingosdk::set_local_world_layer_map_hint(
        manifest_custom_active ? dingosdk::WorldMap::bam : dingosdk::WorldMap::none);
    dingosdk::ConsoleActivity activity;
    activity.state = state;
    activity.native_loading_logging = r.native_loading_logging;
    activity.state_name = state_name(state);
    activity.level = description.level;
    activity.sublevel = description.lm_level;
    for (const auto& level : sublevels.levels)
        activity.sublevel_active |= sublevels.available && sublevels.active && level.state == 5 &&
            equals(level.asset, description.lm_level);
    activity.client_sublevel_present = client_content.sublevels_available &&
        client_content.requested_sublevel_present;
    activity.players_available = client_content.players_available;
    activity.players = client_content.local_player_count;
    activity.controllables = client_content.controllable_reference_count;
    {
        std::lock_guard lock(r.mutex);
        if (r.debug_model.skater_position_valid) {
            activity.skater = r.debug_model.skater_identity;
            activity.position = r.debug_model.skater_position;
        }
    }
    r.console_activity.update(activity, &activity_line);
    const auto content_json = "{\"event\":\"client_content\",\"sublevels_available\":" +
        std::string(client_content.sublevels_available ? "true" : "false") +
        ",\"sublevel_count\":" + std::to_string(client_content.sublevel_count) +
        ",\"requested_sublevel_present\":" + (client_content.requested_sublevel_present ? "true" : "false") +
        ",\"player_manager_present\":" + (client_content.player_manager_present ? "true" : "false") +
        ",\"players_available\":" + (client_content.players_available ? "true" : "false") +
        ",\"local_player_count\":" + std::to_string(client_content.local_player_count) +
        ",\"controllable_reference_count\":" + std::to_string(client_content.controllable_reference_count) + "}";
    if (content_json != r.last_client_content) { record(content_json); r.last_client_content = content_json; }
    if (sublevels_read) {
        const auto sublevel_json = dingosdk::sublevel_catalog_json(sublevels);
        if (sublevel_json != r.last_sublevels) {
            dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Context::server,
                dingosdk::logging::Channel::level, "Server sublevel catalog: {} entries; available={}, root active={}.",
                sublevels.levels.size(), sublevels.available, sublevels.active);
            dingosdk::logging::event(dingosdk::logging::Context::server, dingosdk::logging::Channel::level,
                "{\"event\":\"server_sublevels\",\"catalog\":" + sublevel_json + "}");
            r.last_sublevels = sublevel_json;
        }
    }
    if (state != r.previous_state || description.level != r.last_level) {
        record("{\"event\":\"engine_state\",\"state\":" + std::to_string(state) +
               ",\"description\":" + dingosdk::world_description_json(description) + "}");
        r.previous_state = state;
        r.last_level = description.level;
    }
    if (!r.catalog_logged && catalog.available && native_level_count && !catalog.levels.empty()) {
        record("{\"event\":\"level_catalog\",\"catalog\":" + dingosdk::world_catalog_json(catalog) + "}");
        r.catalog_logged = true;
        const auto manifest_count = static_cast<std::size_t>(std::count_if(
            catalog.levels.begin(), catalog.levels.end(), [](const auto& level) { return level.manifest_only; }));
        activity_line(dingosdk::ConsoleSource::level,
            "Discovered " + std::to_string(catalog.levels.size()) + " levels (" +
            std::to_string(native_level_count) + " native, " + std::to_string(manifest_count) +
            " manifest). Ready for manual selection.");
    }
    dingosdk::overlay::Model model;
    const auto missions_model = dingosdk::local_profile_missions();
    model.progression = dingosdk::local_profile_progression();
    model.player_card = dingosdk::local_profile_player_card();
    model.hall_of_meat = {dingosdk::hall_of_meat::available(), dingosdk::hall_of_meat::enabled()};
    model.bindings = dingosdk::local_profile_controller_bindings();
    model.parks = dingosdk::local_profile_parks();
    model.world = dingosdk::local_profile_world_layers();
    model.missions_available = missions_model.available;
    model.mission_feedback = missions_model.feedback;
    for (const auto& row : missions_model.rows) model.missions.push_back({row.id, row.group, row.completed});
    model.state = state_name(state);
    model.detail = description.level.empty() ? "Waiting for a level description" : description.level;
    if (!description.lm_level.empty()) {
        bool active = false;
        for (const auto& level : sublevels.levels)
            active = active || (sublevels.available && sublevels.active && level.state == 5 && equals(level.asset, description.lm_level));
        model.detail += "\nSublevel: " + description.lm_level + (active ? " (active on local server)" : " (not yet confirmed active)");
        model.detail += "\nClient sublevel: " + std::string(!client_content.sublevels_available ? "unavailable" :
            (client_content.requested_sublevel_present ? "present" : "not present"));
    }
    model.detail += client_content.players_available ? "\nLocal players: " + std::to_string(client_content.local_player_count) +
        " | Controllable references: " + std::to_string(client_content.controllable_reference_count) : "\nLocal players: unavailable";
    for (const auto& level : catalog.levels) {
        dingosdk::overlay::Level choice;
        choice.asset = level.asset;
        choice.display_name = level.display_name;
        choice.manifest_start_point = level.manifest_start_point;
        choice.native_registered = !level.manifest_only;
        choice.custom = level.custom;
        for (const auto& point : level.start_points) choice.start_points.push_back(point.name);
        choice.can_load = catalog.available && context_ready;
        choice.load_block_reason = "Waiting for this process's native local world context.";
        model.levels.push_back(std::move(choice));
    }
    const bool local = (state == 13 || state == 21) && context_ready && catalog.available && !catalog.levels.empty();
    model.can_queue_load = local;
    model.load_block_reason = local ? "" : "Waiting for an active local level and initialized level manager.";
    if (!catalog.available) model.load_block_reason = catalog.issue;
    const auto readiness = "{\"event\":\"load_readiness\",\"can_load\":" + std::string(local ? "true" : "false") +
               ",\"state\":" + std::to_string(state) +
               ",\"context_ready\":" + (context_ready ? "true" : "false") +
               ",\"game_type\":" + std::to_string(game_type) +
               ",\"catalog_available\":" + (catalog.available ? "true" : "false") +
               ",\"level_count\":" + std::to_string(catalog.levels.size()) + "}";
    if (readiness != r.last_readiness) {
        record(readiness);
        r.last_readiness = readiness;
    }
    bool load_busy{};
    { std::lock_guard lock(r.mutex); load_busy = r.requests.loading(); }
    dingosdk::update_local_world_controls(!r.observer_failed && !load_busy && local && context_ready &&
        (state == 13 || state == 21) && model.world.ready ? model.world.map : dingosdk::WorldMap::none);
    model.world_controls = dingosdk::local_profile_world_controls();
    dingosdk::update_local_graphics_controls();
    model.graphics = dingosdk::local_profile_graphics_controls();
    model.object_persistence = dingosdk::local_profile_object_persistence();
    // The park list is a disk walk of the Mods folder: only while the menu or the editor shows it.
    dingosdk::set_park_mod_list_visible(r.debug_model.park_editor ||
        GetTickCount64() < r.named_settings_wanted_until.load(std::memory_order_relaxed));
    model.editor = dingosdk::local_park_editor();
    // Spectating only needs one controllable local player; it works on every
    // map, including custom mod levels.
    r.spectate_ready = local && !load_busy && client_content.players_available &&
        client_content.local_player_count == 1 && client_content.controllable_reference_count == 1;
    r.spectate_client = client;
    std::optional<Request> request;
    {
        std::lock_guard lock(r.mutex);
        if (r.requests.inflight()) {
            r.transition_seen = r.transition_seen || state != r.requested_from;
            const bool manifest_sublevel = !r.last_lm_level.empty() &&
                std::any_of(catalog.levels.begin(), catalog.levels.end(), [&](const auto& level) {
                    return level.manifest_only && equals(level.asset, r.last_lm_level);
                });
            bool sublevel_active = r.last_lm_level.empty();
            for (const auto& level : sublevels.levels)
                sublevel_active = sublevel_active || (sublevels.available && sublevels.active && level.state == 5 &&
                    equals(level.asset, r.last_lm_level));
            const bool client_sublevel_present = r.last_lm_level.empty() ||
                (client_content.sublevels_available && client_content.requested_sublevel_present);
            // Native destinations are corroborated by both stock sublevel
            // registries. A Studio manifest destination may not be published by
            // one or both supplemental registries, so the engine's settled state
            // plus its exact active WorldDescription is authoritative.
            const bool manifest_description_active = manifest_sublevel && description.available &&
                equals(description.level, r.last_request) &&
                equals(description.lm_level, r.last_lm_level);
            const bool content_confirmed = manifest_sublevel ? manifest_description_active :
                (sublevel_active && client_sublevel_present);
            const bool settled = r.transition_seen && (state == 13 || state == 21) && content_confirmed;
            const bool timed_out = now - r.request_started >= 90000 && (state == 13 || state == 21) && local;
            const bool cancelled = state == 2 || state == 24 || state == 25;
            if (settled || timed_out || cancelled) {
                const bool destination_matched = settled && equals(description.level, r.last_request) &&
                    (r.last_lm_level.empty() || equals(description.lm_level, r.last_lm_level));
                const bool start_points_matched =
                    (r.last_start.empty() || equals(description.start_point, r.last_start)) &&
                    (r.last_lm_start.empty() || equals(description.lm_start_point, r.last_lm_start));
                // A start point is a placement request, not part of a detached
                // custom level's loaded identity. Preserve the comparison in
                // diagnostics, but do not report a loaded custom world as failed
                // if the engine normalizes or substitutes its default spawn.
                const bool matched = destination_matched &&
                    (manifest_sublevel || start_points_matched);
                r.load_result = matched ? (manifest_sublevel ?
                    "Requested custom destination is active; player control is not yet verified." :
                    "Requested root loaded and sublevel content is present; player control is not yet verified.") :
                    "The requested level/start point did not finish loading as expected.";
                record("{\"event\":\"native_level_result\",\"matched\":" + std::string(matched ? "true" : "false") +
                       ",\"timed_out\":" + (timed_out ? "true" : "false") +
                       ",\"manifest_sublevel\":" + (manifest_sublevel ? "true" : "false") +
                       ",\"manifest_description_active\":" + (manifest_description_active ? "true" : "false") +
                       ",\"start_points_matched\":" + (start_points_matched ? "true" : "false") +
                       ",\"server_sublevel_active\":" + (sublevel_active ? "true" : "false") +
                       ",\"client_sublevel_present\":" + (client_sublevel_present ? "true" : "false") +
                       ",\"description\":" + dingosdk::world_description_json(description) + "}");
                activity_line(dingosdk::ConsoleSource::level,
                    matched ? "Level content loaded: " + r.last_request +
                        (r.last_lm_level.empty() ? "" : " + " + r.last_lm_level)
                        : (timed_out ? "Level load timed out: " : "Level load did not complete: ") + r.last_request,
                    matched ? dingosdk::ConsoleSeverity::success : dingosdk::ConsoleSeverity::warning);
                r.requests.finish_load();
                dingosdk::loading_screen::cancel();
            }
        }
        if (!r.load_result.empty()) model.detail += "\n" + r.load_result;
        if (r.requests.inflight()) {
            model.can_queue_load = false;
            model.load_block_reason = "Waiting for the requested level to finish loading.";
        }
        request = r.requests.take_load();
        r.menu_splash_ready = local && description.available &&
            equals(description.level, "levels/Game/DingoLevel_Splash/DingoLevel_Splash");
        if (!r.menu_splash_ready || (!r.requests.loading() && !request)) r.menu_load_queued = false;
        r.model = std::move(model);
        ++r.model_revision;
    }
    // Start San Van as soon as the splash world's local loader is ready. Use
    // the normal request scheduler, preserving user/join requests and all
    // transition cleanup. This is a once-per-process startup action.
    if (!r.startup_load_queued) {
        if (request || (local && description.available && !r.menu_splash_ready))
            r.startup_load_queued = true;
        else if (queue_menu_bam()) r.startup_load_queued = true;
    }
    if (!request) return;
    const dingosdk::LevelInfo* selected_level = nullptr;
    for (const auto& level : catalog.levels)
        if (!level.manifest_only && equals(level.asset, request->asset)) selected_level = &level;
    if (!local || !registered_request(catalog, *request) || !selected_level) {
        activity_line(dingosdk::ConsoleSource::level,
            "Load rejected: local context or selected level changed.", dingosdk::ConsoleSeverity::warning);
        record("{\"event\":\"native_level_rejected\",\"reason\":\"context_or_selection_changed\"}");
        std::lock_guard lock(r.mutex);
        r.requests.finish_load();
        r.model.load_block_reason = "Load rejected: the local context or registered selection changed.";
        ++r.model_revision;
        return;
    }
    // Validated afresh for the restore, which may itself change the camera.
    frame.camera_issue.reset();
    const bool restored = dingosdk::restore_client_debug(r.base, client, camera_phase_ready());
    DingoSDKOverlaySetFreecamInputCapture(false);
    frame.camera_issue.reset();
    if (!restored) {
        std::lock_guard lock(r.mutex);
        r.requests.finish_load();
        r.load_result = "Load cancelled: restore the current debug camera/settings first, then retry.";
        activity_line(dingosdk::ConsoleSource::level, r.load_result, dingosdk::ConsoleSeverity::warning);
        return;
    }
    if (!dingosdk::multiplayer::prepare_native_menu_level_load(r.base)) {
        std::lock_guard lock(r.mutex);
        r.requests.finish_load();
        r.load_result = "Load cancelled: menu cleanup could not finish. Close the pause menu and retry.";
        activity_line(dingosdk::ConsoleSource::level, r.load_result, dingosdk::ConsoleSeverity::warning);
        return;
    }
    {
        std::lock_guard lock(r.mutex);
        r.requests.submitted_load();
        r.transition_seen = false;
        r.requested_from = state;
        r.request_started = now;
        r.last_request = request->asset;
        r.last_start = request->start;
        r.last_lm_level = request->lm_level;
        r.last_lm_start = request->lm_start;
        r.load_result.clear();
        r.model.can_queue_load = false;
        r.model.load_block_reason = "Level request submitted; waiting for engine loading.";
        ++r.model_revision;
    }
    dingosdk::WorldDescription selected;
    selected.available = true; selected.level = request->asset; selected.start_point = request->start;
    selected.lm_level = request->lm_level; selected.lm_start_point = request->lm_start;
    selected.flags[0] = selected_level->level_flag68;
    const bool hosted = reinterpret_cast<HostedSelector>(r.base + rt::hosted_level_selector)(request->asset.c_str());
    activity_line(dingosdk::ConsoleSource::level, "Loading " + request->asset +
        (request->lm_level.empty() ? "" : " + " + request->lm_level) +
        " (" + (hosted ? "local hosted server" : "single player") + ").");
    record("{\"event\":\"native_level_request\",\"predicted_route\":\"" + std::string(hosted ? "HostedLocal" : "SinglePlayer") +
           "\",\"selection\":" + dingosdk::world_description_json(selected) + "}");
    dingosdk::multiplayer::host_map_change(request->asset + "|" + request->lm_level);
    submit_local(r.base, client, *request, selected_level->level_flag68);
}
}
void tick(std::uintptr_t client, std::uintptr_t update) {
    const auto incoming_error = GetLastError();
    // The profiler's client update timing: the native update, and ReSkate's own work around it.
    const auto tick_start = dingosdk::profiler::now_ns();
    auto& r = runtime();
    dingosdk::poll_level_loading(client);
    dingosdk::camera_tick_enter(client);
    struct CameraTickScope {
        ~CameraTickScope() {
            const auto error = GetLastError();
            dingosdk::camera_tick_leave();
            SetLastError(error);
        }
    } camera_tick_scope;
    SetLastError(incoming_error);
    const auto native_start = dingosdk::profiler::now_ns();
    r.original_tick(client, update);
    const auto native_time = dingosdk::profiler::now_ns() - native_start;
    struct RecordUpdate {
        std::uint64_t start, native;
        ~RecordUpdate() { dingosdk::profiler::record_client_update(dingosdk::profiler::now_ns() - start - native); }
    } record_update{tick_start, native_time};
    struct PreserveTickError {
        DWORD value{GetLastError()};
        ~PreserveTickError() { SetLastError(value); }
    } preserve_tick_error;
    dingosdk::camera_tick_native_return();
    dingosdk::poll_level_loading(client);
    try {
        TickState tick_state;
        {
            DINGO_PROFILE_ZONE("tick/update_model");
            update_model(client, tick_state);
        }
        if (r.engine_thread.load() != GetCurrentThreadId()) return;
        DWORD state{},game_type{};
        bool loading{};
        { std::lock_guard lock(r.mutex); loading=r.requests.loading(); }
        // update_model's context check, when it made one for this same state and mode.
        const auto context_ready = [&] {
            return tick_state.valid && tick_state.context && tick_state.state == state && tick_state.game_type == game_type
                ? *tick_state.context : native_context(r.base, client, game_type);
        };
        const bool multiplayer_ready=!r.observer_failed && !loading &&
            memory::peek(client+0xc4,state) && (state==13 || state==21) && memory::peek(client+0xc0,game_type) &&
            context_ready();
        {
            DINGO_PROFILE_ZONE("tick/multiplayer");
            dingosdk::multiplayer::tick(r.base,client,multiplayer_ready,r.multiplayer_map,load_multiplayer_map);
            if (auto notice = dingosdk::multiplayer::take_leave_notice(); !notice.empty())
                dingosdk::overlay::notify(dingosdk::overlay::NoticeLevel::warning, "Map not installed", std::move(notice));
        }
        dingosdk::multiplayer::refresh_identity_lists();
        // The player's Discord status (discord_presence.h): where they skate and with whom,
        // looked at every couple of seconds.
        if (static std::uint64_t next_presence{}; dingosdk::discord_presence::available() && GetTickCount64() >= next_presence) {
            next_presence = GetTickCount64() + 2000;
            const auto mp = dingosdk::multiplayer::model();
            const bool session = mp.active && !mp.echo && (mp.hosting || mp.connected);
            // A destination is "root|level": the level names the map. Maps ReSkate adds are "reskate <name>".
            const std::string_view destination = session && !mp.map.empty() ? std::string_view(mp.map) : std::string_view(r.multiplayer_map);
            auto map = dingosdk::world_level_name(destination.substr(destination.find_last_of('|') + 1));
            if (map.size() > 8 && (map.starts_with("reskate ") || map.starts_with("ReSkate "))) map.erase(0, 8);
            dingosdk::discord_presence::Presence presence;
            presence.details = map.empty() ? std::string("Skating") : "On " + map;
            if (!session) {
                presence.state = "Solo skating";
            } else {
                // A session behind a password keeps its name to itself.
                presence.state = mp.password_required ? std::string(mp.dedicated ? "Private server" : "Private lobby")
                                 : mp.lobby_name.empty() ? std::string(mp.dedicated ? "On a server" : "In a lobby")
                                 : (mp.dedicated ? "Server: " : "Lobby: ") + mp.lobby_name;
                presence.party_size = mp.players;
                presence.party_most = std::max(mp.capacity, mp.players);
                presence.party = std::to_string(mp.host_id);
                // Anyone may walk in: no password, and a server or a lobby that is publicly listed
                // (one shared by code stays with those who were given the code).
                if (!mp.password_required && (mp.dedicated || (mp.public_host && mp.lobby_listed) || mp.public_lobby))
                    presence.join = mp.join_code;
            }
            dingosdk::discord_presence::update(std::move(presence));
        }
        // A session the player chose to join from Discord, once the game can join one.
        if (static std::string discord_join; dingosdk::discord_presence::available()) {
            if (auto asked = dingosdk::discord_presence::take_join(); !asked.empty()) discord_join = std::move(asked);
            if (!discord_join.empty() && multiplayer_ready && dingosdk::multiplayer::queue_command("join", discord_join, ""))
                discord_join.clear();
        }
        dingosdk::tick_local_developer_hoodie(r.base, client, multiplayer_ready);
        dingosdk::tick_local_developer_board(r.base, client, multiplayer_ready);
        // The session spawns and places skaters and can teleport: check the camera again.
        tick_state.camera_issue.reset();
        // The spectate camera follows a moving skater, so it runs every client tick, after the
        // session placed the remote skaters for this frame (and also out of play, to give up).
        std::uintptr_t camera_context{};
        const bool camera_phase = tick_state.camera_issue ? *tick_state.camera_issue == nullptr :
            memory::peek(client + 8, camera_context) && !dingosdk::camera_probe_unavailable_reason(camera_context);
        {
            DINGO_PROFILE_ZONE("tick/native party");
            dingosdk::multiplayer::tick_native_party_actions(r.base, client,
                r.spectate_ready && r.spectate_client == client && (state == 13 || state == 21), camera_phase);
        }
        if (state == 13 || state == 21) {
            DINGO_PROFILE_ZONE("tick/camera view");
            // The camera ReSkate's nametags start from, and the spectate camera's lesson in how
            // the game frames the local skater.
            if (dingosdk::publish_local_camera_view(r.base, client))
                if (const auto view = dingosdk::latest_game_view())
                    dingosdk::multiplayer::observe_gameplay_camera(r.base, *view);
        }
        // Same verified client scope in which remote skater actors are created.
        {
            DINGO_PROFILE_ZONE("tick/AI skaters");
            dingosdk::ai_skaters::tick(r.base,client,multiplayer_ready);
        }
        {
            DINGO_PROFILE_ZONE("tick/trainer");
            // A custom map is a sublevel of the root level: that is the map the player means.
            dingosdk::trainer::tick(r.base, client, multiplayer_ready, r.catalog_level.empty() ? r.last_level : r.catalog_level);
        }
        {
            DINGO_PROFILE_ZONE("tick/Steam friend join");
            dingosdk::multiplayer::tick_steam_friend_join();
        }
        // Model creation requires cleanup on every native unload, including
        // transitions that bypass the ReSkate request scheduler.
        if (r.native_loading_logging) {
            DINGO_PROFILE_ZONE("tick/native menu");
            dingosdk::multiplayer::tick_native_menu(r.base, loading);
        }
    }
    catch (const std::exception&) {
        if (!r.observer_failed.exchange(true)) activity_line(dingosdk::ConsoleSource::runtime,
            "Debug controller stopped after an observation error. See the runtime log.", dingosdk::ConsoleSeverity::error);
        std::lock_guard lock(r.mutex);
        r.model.can_queue_load = false;
        r.model.load_block_reason = "Debug controller stopped after a local observation error.";
        ++r.model_revision;
        r.requests.fault();
    }
}
}
