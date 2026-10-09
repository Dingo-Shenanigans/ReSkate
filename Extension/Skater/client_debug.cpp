#include "client_source_spawn.h"
#include "client_source_spawn_internal.h"
#include "no_bail.h"
#include "offboard_flight.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "free_flight.h"
#include "Engine/Game/Skater/velocity_boost.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <cmath>
#include <optional>
#include <utility>

namespace dingosdk::client_source::detail {
double source_flight_now() {
    // GetTickCount64 advances by roughly 15.6 ms even at high frame rates.
    // Keep those coarse ticks for deadlines, never for integrating motion.
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 0.0;
    }();
    LARGE_INTEGER counter{};
    source_require(frequency > 0 && QueryPerformanceCounter(&counter) && counter.QuadPart >= 0,
        "The flight timer is unavailable.");
    return static_cast<double>(counter.QuadPart) / frequency;
}
namespace {
constexpr std::array<float, 7> debug_speeds{5.0f, 0.6f, 3.0f, 15.0f, 60.0f, 300.0f, 1500.0f};
struct DebugUi { std::uintptr_t object{}; std::uint8_t draw{}; };
// The UI settings object last verified writable, rechecked once a second: its
// VirtualQuery otherwise ran on every debug tick. Callers hold SourceState::busy.
struct WritableUi { std::uintptr_t object{}; ULONGLONG until{}; };
bool ui_writable(std::uintptr_t object) {
    static WritableUi cache;
    const auto now = GetTickCount64();
    if (cache.object == object && now < cache.until) return true;
    if (!source_writable(object + 0x4e, 1)) return false;
    cache = {object, now + 1000};
    return true;
}
DebugUi debug_ui(std::uintptr_t base) {
    SourceReader reader;
    DebugUi result{reader.pointer(base, spawn::ui_settings)};
    source_require(source_object(result.object) && reader.pointer(result.object) == base + spawn::ui_settings_vtable &&
        reader.pointer(result.object, 8) == base + spawn::ui_settings_type, "Game UI settings are unavailable.");
    result.draw = reader.value<std::uint8_t>(result.object, 0x4e);
    source_require(result.draw <= 1 && ui_writable(result.object), "Game UI draw flag is invalid.");
    reader.verify();
    return result;
}
std::array<float, 3> debug_position(SourceReader& reader, std::uintptr_t address) {
    // The fourth SIMD lane is not a coordinate and can contain a native sentinel.
    std::array<float, 4> row{};
    source_require(reader.raw(address, row.data(), sizeof(row)), "Position is unavailable.");
    for (std::size_t i = 0; i < 3; ++i)
        source_require(std::isfinite(row[i]) && std::abs(row[i]) <= 1000000.0f, "Position exceeds diagnostic bounds.");
    return {row[0], row[1], row[2]};
}
}
std::array<float, 16> debug_skater(std::uintptr_t base, std::uintptr_t client, overlay::DebugModel& model) {
    SourceReader reader;
    source_require(reader.pointer(client) == base + addr::engine::client_vtable, "Client changed.");
    const auto context = reader.pointer(client, 8);
    const auto offset = reader.value<std::uint32_t>(base, addr::engine::context_player_manager_offset);
    source_require(offset <= 0x1000000, "Player manager offset changed.");
    const auto manager = reader.pointer(context, offset);
    source_require(reader.pointer(manager) == base + addr::engine::local_player_manager_vtable, "Player manager changed.");
    const auto begin = reader.pointer(manager, 0x4c8), end = reader.pointer(manager, 0x4d0);
    source_require(reader.count(begin, end, 8) == 1, "No unique local skater.");
    const auto player = reader.pointer(begin);
    source_require(reader.pointer(player) == base + addr::engine::local_player_vtable && reader.pointer(player, 0x78) == context &&
        reader.value<std::uint8_t>(player, 0x45) == 1 && !reader.value<std::uint8_t>(player, 0x44), "Local player changed.");
    const auto entity = reader.pointer(player, 0xb8), handle = reader.pointer(player, 0xb0);
    source_require(reader.pointer(entity) == base + addr::engine::skater_entity_vtable && reader.pointer(entity, 0x20) == context &&
        reader.pointer(entity, 0xf8) == player && reader.pointer(handle) == entity + 8,
        "No verified player-bound skater.");
    const auto collection = reader.pointer(entity, 0x70);
    source_require(reader.pointer(collection) == entity, "Skater transform owner changed.");
    const auto count = reader.value<std::uint8_t>(collection, 8);
    const auto first = reader.value<std::uint8_t>(collection, 9), extra = reader.value<std::uint8_t>(collection, 10);
    source_require(count <= 128 && first <= 128 && extra <= 32, "Skater transform layout changed.");
    const auto offset_matrix = std::uintptr_t{0x10} + (std::uintptr_t{first} + 2 * std::uintptr_t{extra}) * 0x20;
    const auto position = debug_position(reader, collection + offset_matrix + 0x30);
    std::array<float, 16> matrix{};
    source_require(reader.raw(collection + offset_matrix, matrix.data(), sizeof(matrix)) && valid_flight_transform(matrix),
        "Skater transform is invalid.");
    reader.verify();
    model.skater_position = position;
    model.skater_identity = entity;
    model.skater_position_valid = true;
    return matrix;
}
namespace {
bool debug_has_lease(const InteractiveDebug& debug) {
    return debug.no_bail || debug.noclip || debug.camera_owned || debug.camera_ambiguous || debug.ui_owned;
}
void debug_restore_ui(SourceTrial& trial) {
    auto& debug = trial.debug;
    if (!debug.ui_owned) return;
    const auto ui = debug_ui(trial.base);
    source_require(ui.object == debug.ui_object, "UI settings were replaced; restoration deferred.");
    if (ui.draw == debug.ui_applied) debug_write(ui.object + 0x4e, debug.ui_original);
    debug.ui_owned = false;
}
void debug_restore_camera(SourceTrial& trial, std::uintptr_t client, bool phase, DWORD error) {
    auto& debug = trial.debug;
    debug_stop_noclip(debug);
    source_require(!debug.camera_ambiguous, "Native camera call had an uncertain result; restart before changing camera mode.");
    if (!debug.camera_owned) { debug.first_person = false; debug.first_person_saved_fov = 0; return; }
    source_require(phase, "Waiting for the native camera update phase.");
    auto camera = source_camera_snapshot(trial, client);
    if (debug.camera_owned) {
        source_require(camera.identity == debug.camera_identity, "Camera was replaced; restoration deferred.");
        source_require(camera.mode <= 1, "Another native camera mode is active; it was left unchanged.");
        if (camera.mode == 1) {
            source_require(camera.active == camera.identity.camera, "Free camera selection changed.");
            first_person_disarm();
            if (debug.first_person) first_person_restore_fov(debug);
            free_camera_restore_fov(debug);
            debug.camera_ambiguous = true; // Cleared only after a native return; no retry on an exception.
            SetLastError(error);
            trial.camera_mode(camera.identity.controller, 0);
            debug.camera_ambiguous = false;
            camera = source_camera_snapshot(trial, client);
            source_require(camera.identity == debug.camera_identity && !camera.mode &&
                camera.active != camera.identity.camera && !camera.input_active, "Camera restoration could not be verified.");
        }
        source_require(camera.active != camera.identity.camera && !camera.input_active,
            "Mode zero still has active free-camera state; restoration is unverified.");
        debug.camera_owned = false;
        debug.flight_ready = false;
        debug.first_person = false;
    }
}
// `taken` receives the camera snapshot this step read, for the tick's model to reuse:
// nothing after it here changes the camera's identity or mode (only its pose and FOV).
void debug_flight_tick(SourceTrial& trial, std::uintptr_t client, bool ready, bool phase,
    const overlay::FlightInput* input, DWORD error, std::optional<SourceCameraSnapshot>& taken) {
    auto& debug = trial.debug;
    const auto now = source_flight_now();
    const auto elapsed = std::clamp(now - debug.flight_time, 0.0, .05);
    debug.flight_time = now;
    if (!debug.first_person || !debug.camera_owned || debug.camera_ambiguous || !ready || !phase) first_person_disarm();
    if ((!debug.camera_owned && !debug.noclip) || debug.camera_ambiguous || !ready || !phase) {
        debug.noclip_velocity.valid = false;
        clear_no_bail_flight();
        return;
    }
    const overlay::FlightInput idle{};
    const auto seconds = static_cast<float>(elapsed);
    const auto camera = source_camera_snapshot(trial, client);
    taken = camera;
    if (debug.noclip) {
        source_require(camera.identity == debug.camera_identity && camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "Player camera changed; flight stopped.");
        const auto bodies = debug_noclip_bodies(trial.base, client, debug.noclip_entity);
        const auto view = debug_view_matrix(trial, camera);
        debug.noclip_velocity = {client, debug.noclip_entity, bodies.core, GetTickCount64() + 250,
            flight_velocity(view, input ? *input : idle, debug.flight_speed), true};
        return;
    }
    if (!debug.flight_ready) return;
    source_require(camera.identity == debug.camera_identity && camera.mode == 1 && camera.active == camera.identity.camera,
        debug.first_person ? "First person lost the camera; view paused." : "Freecam ownership changed; movement paused.");
    auto next = debug.flight_matrix;
    if (debug.first_person) {
        // Respawns, loads and bails can briefly leave no readable head. Hold the
        // last pose instead of dropping the view, and say why once.
        std::uintptr_t component{};
        first_person::Vec3 origin{};
        auto& arm = first_person_arm();
        try {
            component = first_person_component(trial.base, client);
            arm.watched_base.store(trial.base, std::memory_order_release);
            arm.watched.store(component, std::memory_order_release);
            // Newest capture of this skater, if one is recent; otherwise read now.
            std::optional<FirstPersonArm::Snapshot> latest;
            {
                std::lock_guard lock(arm.snapshot_mutex);
                // A capture stays usable while animation pauses: the pose in
                // memory then still has the head hidden and cannot be re-read.
                if (arm.captures && now - arm.latest.time < 2.0 && component == arm.component) {
                    latest = arm.latest;
                }
            }
            if (latest) {
                // The tick runs before this frame's animation, so the newest
                // capture is last frame's head. The render handoff replaces it
                // with this frame's; this only covers frames without one.
                for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u}) next[i] = latest->head[i];
                origin = latest->origin;
            } else origin = first_person_head_matrix(trial.base, component, next);
        } catch (const SourceGuard& guard) {
            first_person_disarm();
            debug.status = guard.message;
            debug.first_person_waiting = true;
            return;
        }
        if (arm.component != component || arm.camera != camera.identity.camera) arm.spring.reset();
        arm.component = component;
        arm.camera = camera.identity.camera;
        arm.transform = trial.camera_transform;
        arm.fov = debug.first_person_fov;
        arm.settings = debug.first_person_settings;
        arm.spring.begin(now);
        next = arm.spring.sample(next, arm.settings, origin);
        source_require(valid_flight_transform(next), "First-person spring pose is invalid.");
        arm.matrix = next;
        arm.thread.store(GetCurrentThreadId(), std::memory_order_release);
        if (debug.first_person_waiting) {
            debug.first_person_waiting = false;
            debug.status = debug.first_person_settings.stabilize
                ? "First person on. True first person keeps the view level and steady."
                : "First person on. The camera follows the skater's head.";
        }
    } else {
        next = step_free_flight(debug.flight_matrix, input ? *input : idle, seconds, debug.flight_speed);
    }
    // Runs after the native client update on the same engine thread. Publishing
    // our owned pose each tick also prevents native helper input from drifting it.
    debug.flight_ready = false; // An exceptional setter must not be retried automatically.
    SetLastError(error);
    trial.camera_transform(camera.identity.camera, &next);
    debug.flight_matrix = next;
    debug.flight_ready = true;
    // Written every tick: the camera may refresh its FOV from its own settings.
    if (debug.first_person && debug.first_person_fov > 0)
        (void)first_person_write_fov(camera.identity.camera, debug.first_person_fov);
    else if (!debug.first_person && debug.free_camera_fov > 0) {
        if (!debug.free_camera_saved_fov) {
            float current{};
            debug.free_camera_saved_fov = first_person_read(camera.identity.camera + camera_fov_offset, &current, 4) &&
                std::isfinite(current) && current > 1.0f && current < 175.0f ? current : 0.0f;
        }
        (void)first_person_write_fov(camera.identity.camera, debug.free_camera_fov);
    }
}
// ---- saved choices ("ReSkate.*" in the local profile)
namespace saved {
constexpr const char* no_bail = "NoBail";
constexpr const char* flight_speed = "FlightSpeed";
constexpr const char* forward_velocity = "ForwardVelocity";
constexpr const char* up_velocity = "UpVelocity";
constexpr const char* offboard_up_velocity = "OffboardUpVelocity";
constexpr const char* first_person_fov = "FirstPerson.Fov";
constexpr const char* free_camera_fov = "FreeCamera.Fov";
constexpr const char* spring = "FirstPerson.Spring";
constexpr std::array<const char*, 3> offset{"FirstPerson.OffsetX", "FirstPerson.OffsetY", "FirstPerson.OffsetZ"};
constexpr std::array<const char*, 3> rotation{"FirstPerson.Pitch", "FirstPerson.Yaw", "FirstPerson.Roll"};
constexpr std::array<const char*, 4> strength{"FirstPerson.Up", "FirstPerson.Down", "FirstPerson.Left", "FirstPerson.Right"};
constexpr const char* stabilize = "FirstPerson.Stabilize";
constexpr const char* follow_flips = "FirstPerson.FollowFlips";
constexpr const char* board_only = "FirstPerson.ThirdPersonOnFoot";
constexpr std::array<const char*, 4> steady{"FirstPerson.Smoothing", "FirstPerson.HeadPitch", "FirstPerson.HeadRoll", "FirstPerson.Bob"};
constexpr ULONGLONG delay_ms = 750;

std::optional<float> number(const char* key) {
    const auto value = profile_runtime::local_value(key);
    if (!value || !value->is_number()) return {};
    const auto result = static_cast<float>(value->get<double>());
    return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}
} // namespace saved

void mark_debug_changed(InteractiveDebug& debug) noexcept {
    debug.save_pending = true;
    debug.save_due = GetTickCount64() + saved::delay_ms;
}

void save_debug(InteractiveDebug& debug) noexcept {
    debug.save_pending = false;
    try {
        const auto& arm = debug.first_person_settings;
        std::vector<std::pair<std::string, Json>> values{
            {saved::no_bail, debug.no_bail || debug.no_bail_restore},
            {saved::flight_speed, static_cast<double>(debug.flight_speed)},
            {saved::forward_velocity, static_cast<double>(debug.forward_velocity_speed)},
            {saved::up_velocity, static_cast<double>(debug.up_velocity_speed)},
            {saved::offboard_up_velocity, static_cast<double>(debug.offboard_up_velocity_speed)},
            {saved::first_person_fov, static_cast<double>(debug.first_person_fov)},
            {saved::free_camera_fov, static_cast<double>(debug.free_camera_fov)},
            {saved::spring, arm.enabled}};
        for (std::size_t i = 0; i < 3; ++i) {
            values.emplace_back(saved::offset[i], static_cast<double>(arm.offset[i]));
            values.emplace_back(saved::rotation[i], static_cast<double>(arm.rotation[i]));
        }
        const std::array<float, 4> strengths{arm.up, arm.down, arm.left, arm.right};
        for (std::size_t i = 0; i < 4; ++i) values.emplace_back(saved::strength[i], static_cast<double>(strengths[i]));
        values.emplace_back(saved::stabilize, arm.stabilize);
        values.emplace_back(saved::follow_flips, arm.follow_flips);
        values.emplace_back(saved::board_only, arm.board_only);
        const std::array<float, 4> steady{arm.smoothing, arm.head_pitch, arm.head_roll, arm.bob};
        for (std::size_t i = 0; i < 4; ++i) values.emplace_back(saved::steady[i], static_cast<double>(steady[i]));
        profile_runtime::set_local_values(values);
    } catch (...) { /* Saving is best effort; the choices already apply. */ }
}

// Applies saved choices over the current values; anything missing or out of
// range keeps its default. No Bail is re-applied by the tick once a skater exists.
void load_saved_debug(InteractiveDebug& debug) noexcept {
    try {
        if (const auto value = profile_runtime::local_preference(saved::no_bail); value && *value && !debug.no_bail) {
            debug.no_bail_restore = true;
            debug.no_bail_restore_after = GetTickCount64() + 2000;
        }
        if (const auto speed = saved::number(saved::flight_speed);
            speed && std::find(debug_speeds.begin(), debug_speeds.end(), *speed) != debug_speeds.end())
            debug.flight_speed = *speed;
        if (const auto speed = saved::number(saved::forward_velocity); speed && *speed >= 1.0f && *speed <= 300.0f)
            debug.forward_velocity_speed = *speed;
        if (const auto speed = saved::number(saved::up_velocity); speed && *speed >= 1.0f && *speed <= 25.0f)
            debug.up_velocity_speed = *speed;
        if (const auto speed = saved::number(saved::offboard_up_velocity); speed && *speed >= 1.0f && *speed <= 25.0f)
            debug.offboard_up_velocity_speed = *speed;
        if (const auto fov = saved::number(saved::first_person_fov);
            fov && *fov >= first_person_fov_min && *fov <= first_person_fov_max) {
            debug.first_person_fov = *fov;
            first_person_arm().fov = *fov;
        }
        if (const auto fov = saved::number(saved::free_camera_fov);
            fov && (*fov == 0.0f || (*fov >= first_person_fov_min && *fov <= first_person_fov_max)))
            debug.free_camera_fov = *fov;
        auto arm = debug.first_person_settings;
        if (const auto enabled = profile_runtime::local_preference(saved::spring)) arm.enabled = *enabled;
        for (std::size_t i = 0; i < 3; ++i) {
            if (const auto value = saved::number(saved::offset[i])) arm.offset[i] = *value;
            if (const auto value = saved::number(saved::rotation[i])) arm.rotation[i] = *value;
        }
        const std::array<float*, 4> strengths{&arm.up, &arm.down, &arm.left, &arm.right};
        for (std::size_t i = 0; i < 4; ++i)
            if (const auto value = saved::number(saved::strength[i])) *strengths[i] = *value;
        if (const auto enabled = profile_runtime::local_preference(saved::stabilize)) arm.stabilize = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::follow_flips)) arm.follow_flips = *enabled;
        if (const auto enabled = profile_runtime::local_preference(saved::board_only)) arm.board_only = *enabled;
        const std::array<float*, 4> steady{&arm.smoothing, &arm.head_pitch, &arm.head_roll, &arm.bob};
        for (std::size_t i = 0; i < 4; ++i)
            if (const auto value = saved::number(saved::steady[i])) *steady[i] = *value;
        if (first_person::valid(arm)) {
            debug.first_person_settings = arm;
            first_person_arm().settings = arm;
        }
    } catch (...) { /* Unreadable choices leave the defaults in place. */ }
}

void debug_action(SourceTrial& trial, std::uintptr_t client, bool can_control, bool phase,
                  const overlay::DebugRequest& request, DWORD error) {
    auto& debug = trial.debug;
    if (request.action == overlay::DebugAction::set_park_editor) {
        source_require(!request.enabled || lobby_object_placement_allowed(),
            "The host has disabled object placement for you in this session.");
        if (request.enabled == debug.park_editor) return;
        source_require(can_control && phase, "Wait for the local camera before changing editor mode.");
        debug.editor_transition = true;
        struct EndTransition { bool& flag; ~EndTransition(){flag=false;} } transition{debug.editor_transition};
        const auto apply = [&](overlay::DebugAction action, bool enabled) {
            debug_action(trial, client, can_control, phase, {action, enabled}, error);
        };
        if (request.enabled) {
            const auto camera = source_camera_snapshot(trial, client);
            debug.editor_previous_camera = camera.mode == 1;
            debug.editor_previous_ui = debug_ui(trial.base).draw == 0;
            debug.editor_previous_noclip = debug.noclip;
            debug.editor_previous_first_person = debug.first_person;
            // Retain the restoration state even if activation fails halfway.
            debug.park_editor = true;
            try {
                apply(overlay::DebugAction::set_free_camera, true);
                apply(overlay::DebugAction::set_game_ui_hidden, true);
            } catch (...) {
                try {
                    apply(overlay::DebugAction::set_game_ui_hidden, debug.editor_previous_ui);
                    if (!debug.editor_previous_camera) apply(overlay::DebugAction::set_free_camera, false);
                    if (debug.editor_previous_noclip && session_noclip_allowed()) apply(overlay::DebugAction::set_noclip, true);
                    if (debug.editor_previous_first_person) apply(overlay::DebugAction::set_first_person, true);
                    debug.park_editor = false;
                } catch (...) {}
                throw;
            }
            debug.status = "Park editor enabled. Hold RMB over the viewport to fly.";
        } else {
            apply(overlay::DebugAction::set_game_ui_hidden, debug.editor_previous_ui);
            if (!debug.editor_previous_camera) apply(overlay::DebugAction::set_free_camera, false);
            if (debug.editor_previous_noclip && session_noclip_allowed()) apply(overlay::DebugAction::set_noclip, true);
            if (debug.editor_previous_first_person) apply(overlay::DebugAction::set_first_person, true);
            debug.park_editor = false;
            debug.status = "Park editor closed. Previous camera and UI settings restored.";
        }
        return;
    }
    source_require(!debug.park_editor || debug.editor_transition || request.action == overlay::DebugAction::restore_debug ||
        request.action == overlay::DebugAction::set_camera_speed || request.action == overlay::DebugAction::set_forward_velocity_speed ||
        request.action == overlay::DebugAction::set_up_velocity_speed ||
        request.action == overlay::DebugAction::set_offboard_up_velocity_speed ||
        request.action == overlay::DebugAction::set_first_person_fov ||
        request.action == overlay::DebugAction::set_free_camera_fov ||
        (request.action >= overlay::DebugAction::set_first_person_spring && request.action <= overlay::DebugAction::reset_first_person_arm) ||
        request.action == overlay::DebugAction::set_no_bail,
        "Close Park Editor before changing camera or HUD modes.");
    if (request.action >= overlay::DebugAction::set_first_person_spring &&
        request.action <= overlay::DebugAction::reset_first_person_arm) {
        auto settings = debug.first_person_settings;
        using Action = overlay::DebugAction;
        switch (request.action) {
        case Action::set_first_person_spring: settings.enabled = request.enabled; break;
        case Action::set_first_person_offset_x: settings.offset[0] = request.value; break;
        case Action::set_first_person_offset_y: settings.offset[1] = request.value; break;
        case Action::set_first_person_offset_z: settings.offset[2] = request.value; break;
        case Action::set_first_person_pitch: settings.rotation[0] = request.value; break;
        case Action::set_first_person_yaw: settings.rotation[1] = request.value; break;
        case Action::set_first_person_roll: settings.rotation[2] = request.value; break;
        case Action::set_first_person_spring_up: settings.up = request.value; break;
        case Action::set_first_person_spring_down: settings.down = request.value; break;
        case Action::set_first_person_spring_left: settings.left = request.value; break;
        case Action::set_first_person_spring_right: settings.right = request.value; break;
        case Action::set_first_person_stabilize: settings.stabilize = request.enabled; break;
        case Action::set_first_person_follow_flips: settings.follow_flips = request.enabled; break;
        case Action::set_first_person_smoothing: settings.smoothing = request.value; break;
        case Action::set_first_person_head_pitch: settings.head_pitch = request.value; break;
        case Action::set_first_person_head_roll: settings.head_roll = request.value; break;
        case Action::set_first_person_bob: settings.bob = request.value; break;
        case Action::set_first_person_board_only: settings.board_only = request.enabled; break;
        case Action::reset_first_person_arm: settings = {}; break;
        default: break;
        }
        source_require(first_person::valid(settings), "First-person arm setting is outside its supported range.");
        auto& arm = first_person_arm();
        if (settings.enabled != debug.first_person_settings.enabled ||
            settings.stabilize != debug.first_person_settings.stabilize || request.action == Action::reset_first_person_arm)
            arm.spring.reset();
        debug.first_person_settings = settings;
        arm.settings = settings;
        mark_debug_changed(debug);
        debug.status = request.action == Action::set_first_person_stabilize
            ? (settings.stabilize ? "True first person on: level horizon, steady view." : "True first person off: the view follows the raw head.")
            : request.action == Action::set_first_person_board_only
            ? (settings.board_only ? "Third person while walking; first person on the board." : "First person while walking too.")
            : "First-person settings updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_free_camera_fov) {
        source_require(request.value == 0.0f || (std::isfinite(request.value) && request.value >= first_person_fov_min &&
            request.value <= first_person_fov_max), "Freecam FOV must be between 40 and 120 degrees.");
        debug.free_camera_fov = request.value;
        if (request.value == 0.0f) free_camera_restore_fov(debug);
        mark_debug_changed(debug);
        debug.status = request.value == 0.0f ? "Freecam uses the game's own FOV." : "Freecam FOV updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_camera_speed) {
        const auto choice = std::find(debug_speeds.begin(), debug_speeds.end(), request.value);
        source_require(choice != debug_speeds.end(), "Choose one of the flight speed presets.");
        debug.flight_speed = *choice;
        mark_debug_changed(debug);
        debug.status = "Flight speed updated. Mouse sensitivity is unchanged.";
        return;
    }
    if (request.action == overlay::DebugAction::set_forward_velocity_speed ||
        request.action == overlay::DebugAction::set_up_velocity_speed ||
        request.action == overlay::DebugAction::set_offboard_up_velocity_speed) {
        if (request.action == overlay::DebugAction::set_offboard_up_velocity_speed) {
            source_require(std::isfinite(request.value) && request.value >= 1.0f && request.value <= 25.0f,
                "Off-board Up Boost speed must be between 1 and 25.");
            debug.offboard_up_velocity_speed = request.value;
            debug.status = "Off-board Up Boost speed updated.";
        } else {
            const bool up = request.action == overlay::DebugAction::set_up_velocity_speed;
            source_require(std::isfinite(request.value) && request.value >= 1.0f && request.value <= (up ? 25.0f : 300.0f),
                up ? "Up Boost speed must be between 1 and 25." : "Forward Boost speed must be between 1 and 300.");
            (up ? debug.up_velocity_speed : debug.forward_velocity_speed) = request.value;
            debug.status = up ? "Up Boost speed updated." : "Forward Boost speed updated.";
        }
        mark_debug_changed(debug);
        return;
    }
    if (request.action == overlay::DebugAction::restore_debug) {
        if (debug.save_pending) save_debug(debug);
        debug.first_person_settings = {};
        first_person_arm().settings = {};
        first_person_disarm();
        debug.park_editor = false;
        debug.flight_speed = 15.0f;
        debug.forward_velocity_speed = 20.0f;
        debug.forward_velocity.valid = false;
        debug.up_velocity_speed = 20.0f;
        debug.offboard_up_velocity_speed = 20.0f;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
        debug.no_bail = false;
        debug_stop_noclip(debug);
        clear_no_bail();
        // Level loads drop temporary modes, never the player's saved choices.
        load_saved_debug(debug);
        // Restore UI first even when the camera's native context is unavailable.
        std::string issue;
        try { debug_restore_ui(trial); }
        catch (const SourceGuard& guard) { issue = guard.message; }
        catch (...) { issue = "UI restoration failed."; }
        try {
            debug_restore_camera(trial, client, phase, error);
        } catch (const SourceGuard& guard) { if (!issue.empty()) issue += " "; issue += guard.message; }
        catch (...) { if (!issue.empty()) issue += " "; issue += "Camera restoration failed."; }
        if (!issue.empty()) { debug.status = issue; return; }
        debug.status = "Debug changes restored.";
        return;
    }
    if (request.action == overlay::DebugAction::set_noclip && !request.enabled) {
        debug_stop_noclip(debug);
        debug.status = debug.no_bail ? "Noclip disabled. Manual No Bail remains enabled." : "Noclip disabled. Normal skating resumed.";
        return;
    }
    if (request.action == overlay::DebugAction::set_no_bail && !request.enabled) {
        debug.no_bail = false;
        debug.no_bail_restore = false;
        mark_debug_changed(debug);
        debug.status = debug.noclip ? "No Bail manual toggle off; noclip still protects against wipeouts." : "No Bail disabled.";
        return;
    }
    source_require(can_control, "Wait for an active local level before changing debug settings.");
    if (request.action == overlay::DebugAction::add_forward_velocity || request.action == overlay::DebugAction::add_up_velocity || request.action == overlay::DebugAction::add_offboard_up_velocity) {
        source_require(session_boosts_allowed(), "The host has turned off boosts in this session.");
        const bool offboard_boost = request.action == overlay::DebugAction::add_offboard_up_velocity;
        const bool up = request.action == overlay::DebugAction::add_up_velocity || offboard_boost;
        source_require(!debug.noclip, "Disable Noclip before using velocity boosts.");
        source_require(source_state().velocity_guard_active.load(std::memory_order_acquire),
            "Velocity boosts are unavailable for this game build.");
        overlay::DebugModel skater;
        const auto transform = debug_skater(trial.base, client, skater);
        const auto bodies = debug_noclip_bodies(trial.base, client, skater.skater_identity);
        if (offboard_boost) {
            source_require(bodies.offboard || bodies.wipeout, "Off-board Up Boost requires walking, falling or gliding.");
        } else {
            source_require(!bodies.offboard, "Velocity boosts require the skater to be on the board.");
        }
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(skater.skater_identity, 0x7e0) == 0,
            "Wait for the current teleport before using velocity boosts.");
        const auto board_velocity = up ? std::array<float, 3>{} :
            reader.value<std::array<float, 3>>(bodies.parts[0], 0x70);
        reader.verify();
        const float speed = offboard_boost ? debug.offboard_up_velocity_speed : (up ? debug.up_velocity_speed : debug.forward_velocity_speed);
        const auto velocity = velocity_boost_delta(transform, speed,
            up ? VelocityBoostDirection::up : VelocityBoostDirection::forward, board_velocity);
        source_require(velocity.has_value(), "Velocity boost direction or speed is invalid.");
        auto& boost = offboard_boost ? debug.offboard_up_velocity : (up ? debug.up_velocity : debug.forward_velocity);
        source_require(offboard_boost || !boost.valid, "Velocity boost is already queued.");
        boost = {client, skater.skater_identity, bodies.core, GetTickCount64() + 1000, *velocity, true};
        if (offboard_boost) source_state().offboard_boost_core.store(bodies.core, std::memory_order_release);
        if (offboard_boost) {
            // Give the player the impulse on this client tick. The motion hook
            // keeps its upward speed briefly if native off-board drive rewrites it.
            const float current_up = reader.value<float>(bodies.parts[9], 0x74);
            const float target_up = std::max(speed, current_up + speed);
            source_require(std::isfinite(target_up) && target_up <= 100000,
                "Off-board boost produced an invalid upward speed.");
            std::array<std::array<float, 3>, 32> velocities{};
            std::array<std::uint32_t, 32> flags{};
            for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
                velocities[i] = reader.value<std::array<float, 3>>(bodies.parts[i], 0x70);
                flags[i] = reader.value<std::uint32_t>(bodies.parts[i], 0x60);
                velocities[i][1] = std::max(velocities[i][1], target_up);
            }
            reader.verify();
            if (bodies.offboard)
                source_require(ensure_offboard_up_velocity(trial.base, bodies.core, bodies.context,
                    bodies.rig_wrapper, target_up), "Off-board boost motion ownership changed.");
            for (std::size_t i = 9; i < bodies.parts.size(); ++i) {
                debug_write(bodies.parts[i] + 0x70, velocities[i]);
                debug_write(bodies.parts[i] + 0x60, flags[i] | 8u);
            }
            boost.velocity[1] = target_up;
            boost.applied = true;
            boost.expires = GetTickCount64() + 180;
            ++debug.offboard_up_velocity_updates;
            debug.status = "Off-board up velocity added.";
        } else debug.status = up ? "Up Boost queued." : "Forward Boost queued.";
        return;
    }
    if (request.action == overlay::DebugAction::set_no_bail) {
        source_require(session_no_bail_allowed(), "The host has turned off No Bail in this session.");
        overlay::DebugModel skater;
        (void)debug_skater(trial.base, client, skater);
        source_require(update_no_bail(client, skater.skater_identity, true, debug.noclip && debug.noclip_velocity.valid,
            debug.noclip_velocity.expires), "No Bail is unavailable for the current skater.");
        debug.no_bail = true;
        mark_debug_changed(debug);
        debug.status = "No Bail enabled. Prevents new wipeouts; recover first if already bailed.";
        return;
    }
    if (request.action == overlay::DebugAction::set_game_ui_hidden) {
        const auto ui = debug_ui(trial.base);
        const auto desired = static_cast<std::uint8_t>(request.enabled ? 0 : 1);
        if (debug.ui_owned) source_require(ui.object == debug.ui_object, "UI settings owner changed.");
        if (ui.draw != desired) {
            if (!debug.ui_owned) { debug.ui_original = ui.draw; debug.ui_object = ui.object; }
            debug.ui_owned = true; debug.ui_applied = desired;
            debug_write(ui.object + 0x4e, desired);
        }
        if (debug.ui_owned && desired == debug.ui_original) debug.ui_owned = false;
        debug.status = request.enabled ? "Game UI hidden. " + launcher::key_name(launcher::overlay_keys().menu) +
            " still opens this overlay." : "Game UI visible.";
        return;
    }
    source_require(phase, "Waiting for the native camera update phase.");
    source_require(!debug.camera_ambiguous, "Native camera result is uncertain; restart before changing camera mode.");
    if (request.action == overlay::DebugAction::set_noclip) {
        if (debug.noclip) return;
        source_require(session_noclip_allowed(), "The host has turned off noclip in this session.");
        debug.forward_velocity.valid = false;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
        source_require(no_bail_available(), "Noclip requires the native No Bail hooks.");
        source_require(source_state().velocity_guard_active.load(std::memory_order_acquire),
            "Velocity flight is unavailable for this game build.");
        overlay::DebugModel skater;
        (void)debug_skater(trial.base, client, skater);
        (void)debug_noclip_bodies(trial.base, client, skater.skater_identity);
        SourceReader reader;
        source_require(reader.value<std::uint8_t>(skater.skater_identity, 0x7e0) == 0,
            "Wait for the current teleport before enabling flight.");
        reader.verify();
        if (debug.camera_owned) debug_restore_camera(trial, client, phase, error);
        const auto camera = source_camera_snapshot(trial, client);
        source_require(camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "Noclip needs the normal player camera.");
        (void)debug_view_matrix(trial, camera);
        debug.camera_identity = camera.identity;
        debug.noclip_entity = skater.skater_identity;
        debug.flight_time = source_flight_now();
        debug.noclip_velocity_updates = debug.noclip_motion_updates = 0;
        debug.noclip_altitude_valid = false;
        debug.noclip = true;
        debug.status = "Flight enabled with No Bail. Left stick moves; RT / LT raise / lower; L3 boosts.";
        return;
    }
    auto camera = source_camera_snapshot(trial, client);
    if (request.action == overlay::DebugAction::set_first_person_fov) {
        source_require(std::isfinite(request.value) && request.value >= first_person_fov_min &&
            request.value <= first_person_fov_max, "First-person FOV must be between 40 and 120 degrees.");
        debug.first_person_fov = request.value;
        first_person_arm().fov = request.value;
        mark_debug_changed(debug);
        if (debug.first_person) (void)first_person_write_fov(camera.identity.camera, request.value);
        debug.status = "First-person FOV updated.";
        return;
    }
    if (request.action == overlay::DebugAction::set_first_person) {
        if (!request.enabled) {
            if (!debug.first_person) return;
            debug_restore_camera(trial, client, phase, error); // Also ends first person.
            debug.status = "First person off. Original camera restored.";
            return;
        }
        if (debug.first_person) return;
        // Resolve the head against the live camera matrix before taking the
        // camera, so an unavailable skeleton never leaves an orphaned mode 1.
        auto pose = debug_view_matrix(trial, camera);
        const auto origin = first_person_head_matrix(trial.base, first_person_component(trial.base, client), pose);
        {
            // Optional: without the animation hook the view trails by one step.
            std::string hook_detail;
            if (multiplayer::install_entity_hooks(trial.base, hook_detail)) {
                multiplayer::set_animation_evaluated_listener(&first_person_on_animation);
                multiplayer::set_render_pose_listener(&first_person_on_render);
            } else
                dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::skater,
                    "First person will trail the head by one animation step: {}", hook_detail);
        }
        // Take mode 1 through the verified Freecam path, then drive it from the head.
        if (!debug.camera_owned)
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_free_camera, true}, error);
        camera = source_camera_snapshot(trial, client);
        source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
            camera.active == camera.identity.camera, "First person could not take the camera.");
        debug.first_person = true;
        debug.first_person_waiting = false;
        {
            float saved{};
            debug.first_person_saved_fov = first_person_read(camera.identity.camera + camera_fov_offset, &saved, 4) &&
                std::isfinite(saved) && saved > 1.0f && saved < 175.0f ? saved : 0.0f;
        }
        if (debug.first_person_fov > 0) (void)first_person_write_fov(camera.identity.camera, debug.first_person_fov);
        auto& arm = first_person_arm();
        arm.settings = debug.first_person_settings;
        arm.spring.reset();
        arm.spring.begin(source_flight_now());
        pose = arm.spring.sample(pose, arm.settings, origin);
        source_require(valid_flight_transform(pose), "First-person arm pose is invalid.");
        debug.flight_ready = false;
        SetLastError(error);
        trial.camera_transform(camera.identity.camera, &pose);
        debug.flight_matrix = pose;
        debug.flight_time = source_flight_now();
        debug.flight_ready = true;
        debug.status = "First person on. The camera follows the skater's head.";
        return;
    }
    if (request.action == overlay::DebugAction::set_free_camera) {
        debug_stop_noclip(debug);
        if (!request.enabled) {
            source_require(!camera.mode || debug.camera_owned, "This free camera is owned by another experiment.");
            debug_restore_camera(trial, client, phase, error);
            debug.status = "Original camera restored.";
            return;
        }
        if (debug.camera_owned) {
            source_require(camera.identity == debug.camera_identity && camera.mode == 1,
                "The owned free camera changed.");
            if (debug.first_person) {
                // Keep the owned camera and fly on from the last head pose.
                first_person_disarm();
                first_person_restore_fov(debug);
                debug.first_person = false;
                debug.status = "Freecam enabled from the first-person view.";
            }
            return;
        }
        source_require(camera.mode == 0 && camera.view_enabled && !camera.input_active,
            "An active local view with no other camera override is required.");
        const auto pose = debug_view_matrix(trial, camera);
        debug.camera_identity = camera.identity;
        debug.camera_owned = true; debug.camera_ambiguous = true;
        SetLastError(error);
        trial.camera_mode(camera.identity.controller, 1);
        debug.camera_ambiguous = false;
        camera = source_camera_snapshot(trial, client);
        source_require(camera.identity == debug.camera_identity && camera.mode == 1 &&
            camera.active == camera.identity.camera && camera.input_active,
            "Freecam activation could not be verified; disable Freecam to restore the view.");
        SetLastError(error);
        trial.camera_transform(camera.identity.camera, &pose);
        debug.flight_matrix = pose;
        debug.flight_time = source_flight_now();
        debug.flight_ready = true;
        debug.status = "Freecam enabled. WASD / Q E move; hold RMB to look; Shift boosts.";
        return;
    }
    source_require(false, "Unknown debug request.");
}
// "Third person on foot": runs only while first person is on or this paused
// it, so players who never use first person are untouched. Switches through
// the same actions as the menu toggle, after the state has held briefly.
void first_person_board_tick(SourceTrial& trial, std::uintptr_t client, bool can_control, bool phase, DWORD error) {
    auto& debug = trial.debug;
    // Choosing Freecam or the Park Editor while handed back ends the hand-back.
    if (debug.first_person_paused && (debug.park_editor || (debug.camera_owned && !debug.first_person)))
        debug.first_person_paused = false;
    // With the option off, a hand-back still in progress resumes below.
    if (!debug.first_person_paused && !(debug.first_person && debug.first_person_settings.board_only)) return;
    if (!can_control || !phase || debug.park_editor || debug.noclip || debug.editor_transition) return;
    const auto now = GetTickCount64();
    const auto on_foot = debug.first_person_settings.board_only ? first_person_on_foot(trial.base, client) : std::optional<bool>(false);
    if (!on_foot) return;
    if (*on_foot != debug.first_person_last_on_foot) {
        debug.first_person_last_on_foot = *on_foot;
        debug.first_person_foot_since = now;
    }
    const auto held = now - debug.first_person_foot_since;
    if (now < debug.first_person_retry_after) return;
    try {
        if (debug.first_person && *on_foot && held >= 250) {
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_first_person, false}, error);
            debug.first_person_paused = true;
            debug.status = "On foot: third person until you are back on the board.";
        } else if (debug.first_person_paused && !*on_foot && held >= 120) {
            debug_action(trial, client, can_control, phase, {overlay::DebugAction::set_first_person, true}, error);
            debug.first_person_paused = !debug.first_person;
        }
    } catch (const SourceGuard& guard) {
        debug.status = guard.message;
        debug.first_person_retry_after = now + 500;
    } catch (...) {
        debug.first_person_retry_after = now + 500;
    }
}
}
}

namespace dingosdk {
using namespace client_source::detail;

overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client, bool can_control,
    bool camera_phase_observed, const overlay::DebugRequest* request, const overlay::FlightInput* flight_input) {
    SourceLastError error;
    overlay::DebugModel model;
    auto& state = source_state();
    model.free_camera = state.free_camera_active.load(std::memory_order_acquire);
    if (!state.initialized.load(std::memory_order_acquire) || state.busy.test_and_set(std::memory_order_acquire)) return model;
    SourceBusyScope scope{state.busy};
    auto& trial = state.trial;
    auto& debug = trial.debug;
    if (base != trial.base || (debug.thread && debug.thread != GetCurrentThreadId())) return model;
    debug.thread = GetCurrentThreadId();
    if (!debug.saved_loaded) {
        debug.saved_loaded = true;
        load_saved_debug(debug);
    }
    if (debug.save_pending && GetTickCount64() >= debug.save_due) save_debug(debug);
    if (debug.no_bail_restore && can_control && !debug.park_editor && GetTickCount64() >= debug.no_bail_restore_after) {
        debug.no_bail_restore = false;
        debug.no_bail = true; // the tick's update_no_bail applies it to the current skater
    }
    if (!can_control) {
        debug.forward_velocity.valid = false;
        debug.up_velocity.valid = false;
        debug.offboard_up_velocity.valid = false;
    }
    if (debug.park_editor && !lobby_object_placement_allowed() && can_control && camera_phase_observed) {
        try {
            debug_action(trial, client, can_control, camera_phase_observed,
                {overlay::DebugAction::set_park_editor, false}, error.value);
            debug.status = "The host disabled object placement. Previous camera and UI settings restored.";
        } catch (const SourceGuard& guard) { debug.status = guard.message; }
        catch (...) { debug.status = "Restoring camera and UI after the host disabled object placement."; }
    }
    // The host took noclip away (joining, or changed mid-session): land now. A disallowed No
    // Bail is only held back below, so the player's own saved choice comes back after the session.
    if (debug.noclip && !session_noclip_allowed()) {
        debug_stop_noclip(debug);
        debug.status = "The host turned off noclip in this session.";
    }
    const bool no_bail_allowed = session_no_bail_allowed();
    if (request) {
        // The player's own First person choice replaces any on-foot hand-back.
        if (request->action == overlay::DebugAction::set_first_person || request->action == overlay::DebugAction::restore_debug)
            debug.first_person_paused = false;
        try { debug_action(trial, client, can_control, camera_phase_observed, *request, error.value); }
        catch (const SourceGuard& guard) { debug.status = guard.message; }
        catch (...) { debug.status = "Debug action failed; inspect the current state before retrying."; }
    }
    first_person_board_tick(trial, client, can_control, camera_phase_observed, error.value);
    // Camera ownership survives a busy tick or a failed presentation snapshot.
    // Input capture must follow the lease, not those transient model failures.
    state.free_camera_active.store(debug.camera_owned && !debug.first_person, std::memory_order_release);
    model.free_camera = debug.camera_owned && !debug.first_person;
    std::optional<SourceCameraSnapshot> flight_camera;
    try { debug_flight_tick(trial, client, can_control, camera_phase_observed, flight_input, error.value, flight_camera); }
    catch (const SourceGuard& guard) { debug_stop_noclip(debug); debug.status = guard.message; }
    catch (...) { debug_stop_noclip(debug); debug.status = "Flight stopped after a native error. Disable Freecam if its view is still active."; }
    model.available = can_control || debug_has_lease(debug);
    model.park_editor = debug.park_editor;
    model.settings_owned = debug_has_lease(debug);
    model.status = debug.status;
    model.first_person_arm = debug.first_person_settings;
    try {
        const auto ui = debug_ui(base);
        model.ui_available = can_control;
        model.game_ui_hidden = !ui.draw;
    } catch (...) {}
    try {
        const auto camera = flight_camera ? *flight_camera : source_camera_snapshot(trial, client);
        const bool owned_view = camera.mode == 1 && camera.active == camera.identity.camera;
        model.free_camera = owned_view && !debug.first_person;
        // Handed back on foot still reads as on, so the toggle can switch it off.
        model.first_person = (owned_view && debug.first_person) || debug.first_person_paused;
        model.first_person_fov = debug.first_person_fov;
        model.free_camera_fov = debug.free_camera_fov;
        model.camera_available = can_control && camera_phase_observed && !debug.camera_ambiguous;
        model.camera_unavailable = model.camera_available ? "" : !can_control ? "Waiting for active local controls." :
            !camera_phase_observed ? "Waiting for a verified local camera update." :
            "Camera mode could not be verified; restart to use flight.";
        SourceReader reader;
        model.camera_speed = debug.flight_speed;
        const auto vtable = reader.pointer(camera.active);
        if (vtable == base + spawn::free_camera_vtable || vtable == base + addr::engine::camera_vtable) {
            model.camera_position = debug_position(reader, camera.active + 0x80);
            model.camera_position_valid = true;
            if (debug.park_editor) {
                model.camera_transform = debug_view_matrix(trial, camera);
                // Camera's degree FOV at +ac, initialized by spawn::camera_fov_init.
                model.camera_fov = reader.value<float>(camera.active, 0xac);
                model.camera_transform_valid = std::isfinite(model.camera_fov) && model.camera_fov > 1 && model.camera_fov < 175;
            }
        }
        reader.verify();
    } catch (const SourceGuard& guard) {
        model.camera_position_valid = false;
        model.camera_transform_valid = false;
        if (!model.camera_available) model.camera_unavailable = guard.message;
    } catch (...) {
        model.camera_position_valid = false;
        model.camera_transform_valid = false;
        if (!model.camera_available) model.camera_unavailable = "Local camera could not be read.";
    }
    model.noclip_unavailable = model.camera_unavailable;
    model.forward_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Forward Boost." :
        debug.noclip ? "Disable Noclip before using Forward Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Forward Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    model.up_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Up Boost." :
        debug.noclip ? "Disable Noclip before using Up Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Up Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    model.offboard_up_velocity_unavailable = !can_control ? "Waiting for active local controls." :
        debug.park_editor ? "Close Park Editor before using Off-board Up Boost." :
        debug.noclip ? "Disable Noclip before using Off-board Up Boost." :
        !state.velocity_guard_active.load(std::memory_order_acquire) ? "Off-board Up Boost is unavailable for this game build." :
        "Local skater physics could not be read.";
    try {
        (void)debug_skater(base, client, model);
        const auto bodies = debug_noclip_bodies(base, client, model.skater_identity);
        if (debug.forward_velocity.valid && (GetTickCount64() >= debug.forward_velocity.expires ||
            debug.forward_velocity.entity != model.skater_identity || debug.forward_velocity.core != bodies.core)) {
            debug.forward_velocity.valid = false;
            debug.status = "Forward Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip && !debug.forward_velocity.valid &&
            state.velocity_guard_active.load(std::memory_order_acquire) && !bodies.offboard) {
            model.forward_velocity_available = true;
            model.forward_velocity_unavailable.clear();
        } else if (bodies.offboard) {
            model.forward_velocity_unavailable = "Forward Boost requires the skater to be on the board.";
        } else if (debug.forward_velocity.valid) {
            model.forward_velocity_unavailable = "Forward Boost is being applied.";
        }
        if (debug.up_velocity.valid && (GetTickCount64() >= debug.up_velocity.expires ||
            debug.up_velocity.entity != model.skater_identity || debug.up_velocity.core != bodies.core)) {
            debug.up_velocity.valid = false;
            debug.status = "Up Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip && !debug.up_velocity.valid &&
            state.velocity_guard_active.load(std::memory_order_acquire) && !bodies.offboard) {
            model.up_velocity_available = true;
            model.up_velocity_unavailable.clear();
        } else if (bodies.offboard) {
            model.up_velocity_unavailable = "Up Boost requires the skater to be on the board.";
        } else if (debug.up_velocity.valid) {
            model.up_velocity_unavailable = "Up Boost is being applied.";
        }
        if (debug.offboard_up_velocity.valid && (GetTickCount64() >= debug.offboard_up_velocity.expires ||
            debug.offboard_up_velocity.entity != model.skater_identity || debug.offboard_up_velocity.core != bodies.core ||
            (!debug.offboard_up_velocity.applied && !bodies.offboard && !bodies.wipeout))) {
            const bool applied = debug.offboard_up_velocity.applied;
            debug.offboard_up_velocity.valid = false;
            if (!applied) debug.status = "Off-board Up Boost cancelled because skater physics changed.";
        }
        if (can_control && !debug.park_editor && !debug.noclip &&
            state.velocity_guard_active.load(std::memory_order_acquire) && (bodies.offboard || bodies.wipeout)) {
            model.offboard_up_velocity_available = true;
            model.offboard_up_velocity_unavailable.clear();
        } else if (!bodies.offboard && !bodies.wipeout) {
            model.offboard_up_velocity_unavailable = "Off-board Up Boost requires walking, falling or gliding.";
        } else if (debug.offboard_up_velocity.valid) {
            model.offboard_up_velocity_unavailable = "Off-board Up Boost is being applied.";
        }
        model.no_bail_available = can_control && update_no_bail(client, model.skater_identity, debug.no_bail && no_bail_allowed,
            debug.noclip && debug.noclip_velocity.valid, debug.noclip_velocity.expires);
        if (model.camera_available) {
            if (!model.no_bail_available) model.noclip_unavailable = "Waiting for local No Bail protection.";
            else if (!state.velocity_guard_active.load(std::memory_order_acquire))
                model.noclip_unavailable = "Noclip motion hooks are unavailable for this launch.";
            else {
                // `bodies` above already verified the skater's physics this tick.
                model.noclip_available = true;
                model.noclip_unavailable.clear();
            }
        }
    } catch (const SourceGuard& guard) {
        if (model.camera_available) model.noclip_unavailable = guard.message;
        model.forward_velocity_unavailable = guard.message;
        model.up_velocity_unavailable = guard.message;
        model.offboard_up_velocity_unavailable = guard.message;
    } catch (...) {
        if (model.camera_available) model.noclip_unavailable = "Local skater physics could not be read.";
        model.forward_velocity_unavailable = "Local skater physics could not be read.";
        model.up_velocity_unavailable = "Local skater physics could not be read.";
        model.offboard_up_velocity_unavailable = model.up_velocity_unavailable;
    }
    if (!session_boosts_allowed()) {
        model.forward_velocity_available = model.up_velocity_available = model.offboard_up_velocity_available = false;
        model.forward_velocity_unavailable = model.up_velocity_unavailable = model.offboard_up_velocity_unavailable = "The host has turned off boosts in this session.";
    }
    if (!model.no_bail_available) {
        clear_no_bail();
        if (debug.noclip) {
            debug_stop_noclip(debug);
            debug.status = "Flight stopped: local wipeout protection is unavailable.";
        }
    }
    if (!session_noclip_allowed()) {
        model.noclip_available = false;
        model.noclip_unavailable = "The host has turned off noclip in this session.";
    }
    // Noclip's own flight protection stays; only the manual toggle is the host's to refuse.
    if (!no_bail_allowed) model.no_bail_available = false;
    model.no_bail = debug.no_bail && no_bail_allowed;
    model.no_bail_active = model.no_bail_available && ((debug.no_bail && no_bail_allowed) ||
        (debug.noclip && debug.noclip_velocity.valid && GetTickCount64() < debug.noclip_velocity.expires));
    model.settings_owned = debug_has_lease(debug);
    model.status = debug.status;
    model.noclip = debug.noclip;
    model.noclip_velocity_updates = debug.noclip_velocity_updates;
    model.noclip_motion_updates = debug.noclip_motion_updates;
    model.forward_velocity_speed = debug.forward_velocity_speed;
    model.forward_velocity_updates = debug.forward_velocity_updates;
    model.up_velocity_speed = debug.up_velocity_speed;
    model.up_velocity_updates = debug.up_velocity_updates;
    model.offboard_up_velocity_speed = debug.offboard_up_velocity_speed;
    model.offboard_up_velocity_updates = debug.offboard_up_velocity_updates;
    state.offboard_boost_core.store(debug.offboard_up_velocity.valid ? debug.offboard_up_velocity.core : 0,
        std::memory_order_release);
    return model;
}
bool client_free_camera_active() noexcept {
    return source_state().free_camera_active.load(std::memory_order_acquire);
}
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed) {
    const overlay::DebugRequest request{overlay::DebugAction::restore_debug};
    const auto model = on_client_debug_tick(base, client, false, camera_phase_observed, &request);
    return model.status == "Debug changes restored." && !model.settings_owned;
}
}
