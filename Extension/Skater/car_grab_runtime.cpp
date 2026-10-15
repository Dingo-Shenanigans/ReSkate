#include "car_grab_runtime.h"
#include "traffic_vehicle_provider.h"
#include "car_grab_reach.h"
#include "Extension/UI/Overlay/car_grab_hud.h"
#include "client_source_spawn_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Runtime/runtime_internal.h"
#include "car_grab/controller.h"
#include "car_grab/input_gate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <span>

extern "C" void DingoSDKOverlayReadCarGrabInput(dingosdk::overlay::FlightInput* output);

namespace dingosdk {
namespace {
using namespace client_source::detail;
constexpr ULONGLONG lease_ms = 100;
car_grab::Config native_grip_config() noexcept {
    car_grab::Config result;
    // A collision contact authorizes hand placement, not traffic identity.
    // A verified fresh traffic pose can establish a stopped car's path axis.
    result.require_grip = false;
    result.min_vehicle_speed = 0.f;
    return result;
}
struct ClientLease {
    std::uintptr_t base{}, client{}, context{};
    DWORD client_state{}, game_type{};
    ULONGLONG published{}, expires{};
    bool ready{};
    ControllerInput input;
    overlay::FlightInput analog;
    bool camera_valid{};
    std::array<float, 16> camera{};
    float camera_fov{};
};
bool latest_traffic(const ClientLease& lease, car_grab_native::TrafficSnapshot& next) {
    // Client readiness/input are a separate authorization lease. Physics uses
    // the park's latest copied values, not another client-tick traffic cache.
    next = car_grab_native::snapshot();
    return next.scope.base == lease.base && next.scope.client == lease.client &&
        next.scope.client_context == lease.context;
}
struct Bridge {
    std::mutex mutex;
    ClientLease lease;
    car_grab::Controller controller{native_grip_config()}; // SourceState::busy owns this.
    car_grab::GrabInputGate input_gate;
    car_grab::GrabContinuity continuity;
    car_grab_native::TrafficSnapshot held_traffic;
    std::uint64_t held_vehicle{};
    bool attached{};
    car_grab_native::ReachTarget reach;
    car_grab_native::ReachStatus pose_status = car_grab_native::ReachStatus::idle;
    car_grab_hud::Phase phase = car_grab_hud::Phase::hidden;
    car_grab_hud::Cause cause = car_grab_hud::Cause::none;
    ULONGLONG logged{};
    ULONGLONG diagnostic_logged{};
    ULONGLONG physics_logged{};
    ULONGLONG suspension_logged{};
    std::uint64_t physics_signature{};
};
enum class PhysicsGate : unsigned {
    client_lease, debug_controls, multiplayer, local_context, local_core,
    input, ground, traffic_scope, traffic_current, frame, controller,
    velocity, selected_current, surface_current, write, ready
};
enum class PhysicsOutcome : unsigned { ready, rejected, unavailable, source_guard, unexpected };
struct PhysicsDiagnostic {
    PhysicsGate gate = PhysicsGate::client_lease;
    PhysicsOutcome outcome = PhysicsOutcome::ready;
    bool input_sampled{}, raw_grab{}, effective_grab{}, keyboard{}, focused{};
    bool brake{}, jump{}, ground_valid{}, target_valid{}, result_valid{}, traffic_scoped{};
    std::uint32_t device{}, published_device{}, ground{};
    double lease_age_ms = -1, lease_remaining_ms = -1, physics_seconds{};
    double result_distance = -1;
    bool target_motion{};
    car_grab::Vec3 target_forward{}, target_velocity{};
    car_grab::Vec3 skater_position{}, skater_velocity{};
    bool skater_motion{};
    double target_age_ms{}, rear_projection{};
    std::uint64_t traffic_epoch{};
    car_grab::TargetInfo target;
    car_grab::Result result;
    car_grab_native::CurrentReport current;
};
Bridge& bridge() { static auto* value = new Bridge; return *value; }
bool key_held(const ControllerInput& input, unsigned key) noexcept {
    return key < 256 && (input.keys[key / 64] & (std::uint64_t{1} << (key % 64))) != 0;
}
car_grab::Input controls(const ControllerInput& input) noexcept {
    car_grab::Input result;
    result.grab = key_held(input, 'G') || (input.available && (input.buttons & 0x0200u) != 0); // RB / R1
    result.brake = key_held(input, 'S'); // Native brake requests are checked separately.
    result.jump = key_held(input, VK_SPACE); // A pushes; native departure from ground100 releases a controller ollie.
    result.steer = static_cast<float>(key_held(input, 'D')) - static_cast<float>(key_held(input, 'A'));
    return result;
}
double now_seconds() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 0.0;
    }();
    LARGE_INTEGER value{};
    return frequency > 0 && QueryPerformanceCounter(&value) && value.QuadPart >= 0
        ? static_cast<double>(value.QuadPart) / frequency : 0.0;
}
car_grab_hud::Cause reason_for(car_grab::DetachReason reason) noexcept {
    using R = car_grab::DetachReason;
    using C = car_grab_hud::Cause;
    switch (reason) {
    case R::released: return C::let_go;
    case R::too_far: return C::out_of_reach;
    case R::vehicle_lost: return C::car_disappeared;
    case R::stale_vehicle: case R::frame_gap: return C::vehicle_stale;
    case R::ineligible: return C::not_on_board;
    case R::none: return C::none;
    default: return C::reset;
    }
}
void feedback(Bridge& state, car_grab_hud::Feedback value) noexcept {
    const auto now = GetTickCount64();
    // Log transitions, with a bound for noisy input/lifecycle failures. Never
    // log once per physics frame; the HUD receives every fresh value instead.
    if ((value.phase != state.phase || value.cause != state.cause) &&
        (now - state.logged >= 500 || value.phase == car_grab_hud::Phase::attached)) {
        logging::printf(logging::Level::info, logging::Channel::skater,
            "Car Grab state=%u cause=%u speed=%.2f m/s distance=%.2f m.",
            static_cast<unsigned>(value.phase), static_cast<unsigned>(value.cause), value.speed_mps, value.distance);
        state.logged = now;
    }
    state.phase = value.phase; state.cause = value.cause;
    car_grab_hud::publish(value);
}
const char* target_status(car_grab::TargetStatus status) noexcept {
    using S = car_grab::TargetStatus;
    switch (status) {
    case S::no_vehicle: return "no_vehicle";
    case S::missing_surface: return "missing_surface";
    case S::too_slow: return "too_slow";
    case S::stale_or_invalid: return "stale_or_invalid";
    case S::out_of_range: return "out_of_range";
    case S::ready: return "ready";
    default: return "none";
    }
}
const char* physics_gate(PhysicsGate gate) noexcept {
    switch (gate) {
    case PhysicsGate::client_lease: return "client_lease";
    case PhysicsGate::debug_controls: return "debug_controls";
    case PhysicsGate::multiplayer: return "multiplayer";
    case PhysicsGate::local_context: return "local_context";
    case PhysicsGate::local_core: return "local_core";
    case PhysicsGate::input: return "input";
    case PhysicsGate::ground: return "ground";
    case PhysicsGate::traffic_scope: return "traffic_scope";
    case PhysicsGate::traffic_current: return "traffic_current";
    case PhysicsGate::frame: return "frame";
    case PhysicsGate::controller: return "controller";
    case PhysicsGate::velocity: return "velocity";
    case PhysicsGate::selected_current: return "selected_current";
    case PhysicsGate::surface_current: return "surface_current";
    case PhysicsGate::write: return "write";
    case PhysicsGate::ready: return "ready";
    default: return "unexpected";
    }
}
const char* current_failure(car_grab_native::CurrentFailure failure) noexcept {
    using F = car_grab_native::CurrentFailure;
    switch (failure) {
    case F::none: return "none";
    case F::initial_lease: return "initial_lease";
    case F::scope_unavailable: return "scope_unavailable";
    case F::scope_changed: return "scope_changed";
    case F::root_unavailable: return "root_unavailable";
    case F::selected_unavailable: return "selected_unavailable";
    case F::incarnation_changed: return "incarnation_changed";
    case F::entity_changed: return "entity_changed";
    case F::traffic_unavailable: return "traffic_unavailable";
    case F::traffic_missing: return "traffic_missing";
    case F::removal: return "removal";
    case F::read_changed: return "read_changed";
    case F::final_lease: return "final_lease";
    default: return "unexpected";
    }
}
bool removal_waitable(const car_grab_native::CurrentReport& report) noexcept {
    using namespace car_grab_native;
    return report.failure == CurrentFailure::initial_lease && report.initial_age_valid &&
        report.initial_age >= 0 && report.initial_epoch_current && report.initial_publication_current &&
        !report.initial_removal_free &&
        (report.initial_status == TrafficStatus::ready || report.initial_status == TrafficStatus::warming);
}
double log_value(double value, bool valid = true) noexcept {
    // Bounds apply only to text output. No clamp reaches control or safety math.
    return valid && std::isfinite(value) ? std::clamp(value, -60000., 60000.) : -1.;
}
void physics_diagnostic(Bridge& state, const PhysicsDiagnostic& d) noexcept {
    const auto now = GetTickCount64();
    const auto signature = static_cast<std::uint64_t>(d.gate) |
        (static_cast<std::uint64_t>(d.outcome) << 5) |
        (static_cast<std::uint64_t>(d.current.failure) << 8) |
        (static_cast<std::uint64_t>(d.result.reason) << 13) |
        (static_cast<std::uint64_t>(d.target.status) << 17) |
        (static_cast<std::uint64_t>(d.raw_grab) << 21) |
        (static_cast<std::uint64_t>(d.effective_grab) << 22) |
        (static_cast<std::uint64_t>(d.result.attached) << 23) |
        (static_cast<std::uint64_t>(d.result.suspended) << 24);
    const bool changed = signature != state.physics_signature;
    if (now - state.physics_logged < 250 || (!changed && now - state.physics_logged < 1000)) return;
    // Idle only needs a transition record. Held/rejected states have a 1 s
    // heartbeat; transitions share the same maximum four records per second.
    if (!changed && !d.raw_grab && d.outcome == PhysicsOutcome::ready && !d.result.attached) return;
    state.physics_logged = now; state.physics_signature = signature;
    logging::printf(logging::Level::info, logging::Channel::skater,
        "Car Grab physics gate=%s outcome=%u input_sampled=%u raw=%u effective=%u held_blocked=%u "
        "keyboard=%u focus=%u device=%u published_device=%u brake=%u jump=%u "
        "client_age_ms_capped=%.2f client_remaining_ms_capped=%.2f traffic_scope=%u epoch=%llu "
        "native=%s traffic_initial_ms_capped=%.2f traffic_final_ms_capped=%.2f "
        "lease_failure=%u identity_verified=%u motion_warming=%u "
        "ground_valid=%u ground=%u seconds_capped=%.4f controller_elapsed_ms_capped=%.2f "
        "target_valid=%u target=%016llx target_status=%s target_distance_capped=%.2f "
        "target_motion=%u forward_capped=(%.3f,%.3f,%.3f) velocity_capped=(%.2f,%.2f,%.2f) "
        "target_age_ms_capped=%.2f rear_projection_capped=%.2f "
        "skater_motion=%u skater_position_capped=(%.2f,%.2f,%.2f) skater_velocity_capped=(%.2f,%.2f,%.2f) "
        "result_valid=%u result=%016llx result_distance_capped=%.2f attached=%u mode=%u reason=%u suspended=%u write_requested=%u.",
        physics_gate(d.gate), static_cast<unsigned>(d.outcome), d.input_sampled ? 1u : 0u,
        d.raw_grab ? 1u : 0u, d.effective_grab ? 1u : 0u, d.raw_grab && !d.effective_grab ? 1u : 0u,
        d.keyboard ? 1u : 0u, d.focused ? 1u : 0u, d.device, d.published_device,
        d.brake ? 1u : 0u, d.jump ? 1u : 0u,
        log_value(d.lease_age_ms), log_value(d.lease_remaining_ms), d.traffic_scoped ? 1u : 0u,
        static_cast<unsigned long long>(d.traffic_epoch), current_failure(d.current.failure),
        log_value(d.current.initial_age * 1000., d.current.initial_age_valid),
        log_value(d.current.final_age * 1000., d.current.final_age_valid),
        static_cast<unsigned>(d.current.initial_failure), d.current.identity_verified ? 1u : 0u,
        d.current.motion_warming ? 1u : 0u,
        d.ground_valid ? 1u : 0u, d.ground, log_value(d.physics_seconds),
        log_value(d.result.elapsed_seconds*1000., d.result_valid),
        d.target_valid ? 1u : 0u, static_cast<unsigned long long>(d.target.id), target_status(d.target.status),
        log_value(d.target.distance, d.target_valid), d.target_motion ? 1u : 0u,
        log_value(d.target_forward.x, d.target_motion), log_value(d.target_forward.y, d.target_motion), log_value(d.target_forward.z, d.target_motion),
        log_value(d.target_velocity.x, d.target_motion), log_value(d.target_velocity.y, d.target_motion), log_value(d.target_velocity.z, d.target_motion),
        log_value(d.target_age_ms, d.target_motion), log_value(d.rear_projection, d.target_motion),
        d.skater_motion ? 1u : 0u,
        log_value(d.skater_position.x, d.skater_motion), log_value(d.skater_position.y, d.skater_motion), log_value(d.skater_position.z, d.skater_motion),
        log_value(d.skater_velocity.x, d.skater_motion), log_value(d.skater_velocity.y, d.skater_motion), log_value(d.skater_velocity.z, d.skater_motion),
        d.result_valid ? 1u : 0u,
        static_cast<unsigned long long>(d.result.vehicle_id), log_value(d.result_distance, d.result_valid && (d.result.attached || d.target_valid)), d.result.attached ? 1u : 0u,
        static_cast<unsigned>(d.result.mode), static_cast<unsigned>(d.result.reason),
        d.result.suspended ? 1u : 0u, d.result.write_velocity ? 1u : 0u);
}
const char* probe_stage(car_grab_native::SurfaceProbeStage stage) noexcept {
    using S = car_grab_native::SurfaceProbeStage;
    switch (stage) {
    case S::api_unavailable: return "api_unavailable";
    case S::invalid_input: return "invalid_input";
    case S::no_world: return "no_world";
    case S::no_owned_rear: return "no_owned_rear";
    case S::neighborhood: return "neighborhood";
    case S::footprint: return "footprint";
    case S::ready_face: return "ready_face";
    case S::ready_lip: return "ready_lip";
    default: return "none";
    }
}
void search_diagnostic(Bridge& state, const car_grab_native::TrafficSnapshot& traffic,
                       car_grab::TargetStatus target) noexcept {
    const auto now = GetTickCount64();
    if (now - state.diagnostic_logged < 1000) return;
    state.diagnostic_logged = now;
    // The capture copied these values under its publication lock. Logging here
    // invokes no collision/entity API and holds no provider/native lock.
    const auto& d = traffic.diagnostic;
    const bool retained_probe = d.probes == 0 && d.last_probe_vehicle != 0;
    const auto& probe = retained_probe ? d.last_probe : d.probe;
    const auto probe_vehicle = retained_probe ? d.last_probe_vehicle : d.selected_attempt;
    logging::printf(logging::Level::info, logging::Channel::skater,
        "Car Grab search=%s observed=%u posed=%u sampled=%u nearby=%u surfaces=%u grips=%u probes=%u "
        "rays=%u hits=%u owner_rejects=%u no_world=%u no_owned_rear=%u neighborhood=%u footprint=%u "
        "probe_retained=%u probe_age_ms_capped=%.2f probe=%s probe_rays=%u probe_hits=%u probe_owner_rejects=%u raw_owner=%016llx mapped_owner=%016llx "
        "raw_valid=%u owner_stage=%u mapping_status=%u ancestor_depth=%u ancestor_owner=%016llx ancestor_mapped=%016llx "
        "peer_status=%u peer_owner=%016llx peer_selected=%016llx "
        "peer_ancestor=%016llx peer_ancestor_depth=%u "
        "self_available=%u self_entity=%016llx self_excluded=%u self_changed=%u "
        "core_publish_ms_capped=%.2f discovery_ms_capped=%.2f discovery_deferred=%u discovery_late=%u "
        "skater_valid=%u skater_status=%u skater=(%.2f,%.2f,%.2f) "
        "nearest_posed=%016llx posed_xz=%.2f posed_y=%.2f nearest_sampled=%016llx sampled_xz=%.2f sampled_y=%.2f probe_vehicle=%016llx.",
        target_status(target), d.observed, d.posed, d.sampled, d.nearby, d.surfaces, d.grips, d.probes,
        d.ray_calls, d.closest_hits, d.owner_rejects, d.no_world, d.no_owned_rear, d.neighborhood, d.footprint,
        retained_probe ? 1u : 0u, log_value(d.last_probe_age_seconds*1000., d.last_probe_vehicle != 0),
        probe_stage(probe.stage), probe.ray_calls, probe.closest_hits, probe.owner_rejects,
        static_cast<unsigned long long>(probe.raw_owner), static_cast<unsigned long long>(probe.mapped_owner),
        probe.raw_valid ? 1u : 0u, static_cast<unsigned>(probe.owner_stage), static_cast<unsigned>(probe.mapping_status),
        probe.ancestor_depth, static_cast<unsigned long long>(probe.ancestor_owner), static_cast<unsigned long long>(probe.ancestor_mapped),
        static_cast<unsigned>(probe.peer_status), static_cast<unsigned long long>(probe.peer_owner),
        static_cast<unsigned long long>(probe.peer_selected),
        static_cast<unsigned long long>(probe.peer_ancestor_owner), probe.peer_ancestor_depth,
        probe.self_available ? 1u : 0u, static_cast<unsigned long long>(probe.self_entity),
        probe.self_excluded, probe.self_changed ? 1u : 0u,
        log_value(d.core_publish_age_seconds*1000.), log_value(d.discovery_seconds*1000.),
        d.discovery_deferred, d.discovery_late ? 1u : 0u,
        d.skater_valid ? 1u : 0u, static_cast<unsigned>(d.skater_status),
        d.skater_position.x, d.skater_position.y, d.skater_position.z,
        static_cast<unsigned long long>(d.nearest_posed.valid ? d.nearest_posed.id : 0),
        d.nearest_posed.horizontal_distance, d.nearest_posed.vertical_distance,
        static_cast<unsigned long long>(d.nearest_sampled.valid ? d.nearest_sampled.id : 0),
        d.nearest_sampled.horizontal_distance, d.nearest_sampled.vertical_distance,
        static_cast<unsigned long long>(probe_vehicle));
}
}

void publish_car_grab_client(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    SourceLastError preserve;
    try {
        ClientLease next;
        next.base = base;
        next.client = client;
        next.ready = ready && source_object(client) && memory::peek(client + 8, next.context) && source_object(next.context) &&
            memory::peek(client + 0xc4, next.client_state) && (next.client_state == 13 || next.client_state == 21) &&
            memory::peek(client + 0xc0, next.game_type) && next.game_type <= 1;
        const auto multiplayer = multiplayer::model();
        next.ready = next.ready && !multiplayer.active && !multiplayer.lobby_joining;
        DingoSDKOverlayReadControllerInput(&next.input);
        // Separate sampler: never resets freecam's shared mouse baseline.
        DingoSDKOverlayReadCarGrabInput(&next.analog);
        next.ready = next.ready && next.analog.active;
        next.published = GetTickCount64();
        next.expires = next.ready ? next.published + lease_ms : 0;
        // Copy the existing verified native camera on the local client phase.
        // Present only receives values; a missing camera just hides the marker.
        auto& native = source_state();
        if (next.ready && native.initialized.load(std::memory_order_acquire) &&
            !native.busy.test_and_set(std::memory_order_acquire)) {
            SourceBusyScope scope{native.busy};
            try {
                source_require(native.trial.base == base, "Car Grab camera build changed.");
                const auto camera = source_camera_snapshot(native.trial, client);
                next.camera = debug_view_matrix(native.trial, camera);
                SourceReader reader;
                next.camera_fov = reader.value<float>(camera.active, 0xac);
                reader.verify();
                next.camera_valid = std::isfinite(next.camera_fov) && next.camera_fov > 1 && next.camera_fov < 175;
            } catch (...) {}
        }
        car_grab_native::publish_client(base, client, next.context, next.ready);
        if (!next.ready) { car_grab_hud::clear(); car_grab_native::clear_reach(); }
        auto& state = bridge();
        std::lock_guard lock(state.mutex);
        state.lease = std::move(next);
    } catch (...) {
        car_grab_native::publish_client(base, client, 0, false);
        car_grab_hud::clear(); car_grab_native::clear_reach();
        auto& state = bridge();
        std::lock_guard lock(state.mutex);
        state.lease.ready = false;
        state.lease.expires = 0;
    }
}
}

namespace dingosdk::client_source::detail {
void car_grab_physics_tick(std::uintptr_t core) noexcept {
    SourceLastError preserve;
    auto& state = source_state();
    auto& runtime = bridge();
    PhysicsDiagnostic diagnostic;
    // Caller owns busy. Keep this function independent of noclip/No Bail leases.
    try {
        ClientLease lease;
        { std::lock_guard lock(runtime.mutex); lease = runtime.lease; }
        const auto lease_now = GetTickCount64();
        diagnostic.lease_age_ms = lease.published && lease_now >= lease.published
            ? static_cast<double>(lease_now - lease.published) : -1.;
        diagnostic.lease_remaining_ms = lease.expires
            ? static_cast<double>(lease.expires) - static_cast<double>(lease_now) : -1.;
        diagnostic.published_device = lease.input.device;
        car_grab_hud::Feedback status;
        status.controller = lease.input.available && !key_held(lease.input, 'G');
        const auto reject = [&](car_grab_hud::Cause cause, bool hard = true) {
            runtime.controller.reset();
            runtime.continuity.clear(); runtime.held_vehicle = 0; runtime.held_traffic = {};
            if (hard) runtime.input_gate.invalidate();
            runtime.attached = false;
            car_grab_native::clear_reach();
            status.phase = lease.ready ? car_grab_hud::Phase::blocked : car_grab_hud::Phase::hidden;
            status.cause = cause;
            feedback(runtime, status);
            diagnostic.outcome = PhysicsOutcome::rejected;
            physics_diagnostic(runtime, diagnostic);
        };
        if (!state.initialized.load(std::memory_order_acquire) || !state.velocity_guard_active.load(std::memory_order_acquire) ||
            !lease.ready || state.trial.base != lease.base || !source_object(core)) {
            reject(car_grab_hud::Cause::controls_busy); return;
        }
        diagnostic.gate = PhysicsGate::debug_controls;
        const auto& debug = state.trial.debug;
        if (debug.park_editor || debug.noclip || debug.camera_owned || debug.camera_ambiguous ||
            debug.forward_velocity.valid || debug.up_velocity.valid || debug.offboard_up_velocity.valid) {
            reject(car_grab_hud::Cause::controls_busy); return;
        }
        diagnostic.gate = PhysicsGate::multiplayer;
        const auto multiplayer = multiplayer::model();
        if (multiplayer.active || multiplayer.lobby_joining) { reject(car_grab_hud::Cause::controls_busy); return; }
        diagnostic.gate = PhysicsGate::local_context;
        SourceReader reader;
        source_require(reader.pointer(lease.client) == lease.base + addr::engine::client_vtable &&
            reader.pointer(lease.client, 8) == lease.context &&
            reader.value<DWORD>(lease.client, 0xc4) == lease.client_state &&
            reader.value<DWORD>(lease.client, 0xc0) == lease.game_type &&
            dingosdk::runtime::detail::native_context(lease.base, lease.client, lease.game_type), "Car Grab local context changed.");
        overlay::DebugModel skater;
        (void)debug_skater(lease.base, lease.client, skater);
        const auto bodies = debug_noclip_bodies(lease.base, lease.client, skater.skater_identity);
        // This hook runs for every physics core, including ambient AI skaters.
        // A different core must neither write nor cancel the local attachment.
        diagnostic.gate = PhysicsGate::local_core;
        if (bodies.core != core) return;
        diagnostic.physics_seconds = bodies.seconds;
        diagnostic.gate = PhysicsGate::input;
        ControllerInput fresh;
        DingoSDKOverlayReadControllerInput(&fresh); // Focus/menu/device checks happen at physics time too.
        overlay::FlightInput analog;
        DingoSDKOverlayReadCarGrabInput(&analog);
        const auto raw = controls(fresh);
        // A trusted release is observed even while traffic is warming. A
        // recent client lease authorizes the device; physics uses fresh buttons.
        const auto grab = runtime.input_gate.observe(
            {analog.active, raw.grab, key_held(fresh, 'G'), fresh.device},
            {lease.analog.active, controls(lease.input).grab, key_held(lease.input, 'G'), lease.input.device});
        diagnostic.input_sampled = true;
        diagnostic.raw_grab = raw.grab; diagnostic.effective_grab = grab;
        diagnostic.keyboard = key_held(fresh, 'G'); diagnostic.focused = analog.active;
        diagnostic.device = fresh.device; diagnostic.brake = raw.brake; diagnostic.jump = raw.jump;
        status.controller = fresh.available && !key_held(fresh, 'G');
        if (!analog.active) { reject(car_grab_hud::Cause::controls_busy); return; }
        diagnostic.gate = PhysicsGate::ground;
        if (!bodies.offboard && !bodies.wipeout) {
            diagnostic.ground = reader.value<std::uint32_t>(bodies.context, 0x1414);
            diagnostic.ground_valid = true;
        }
        if (bodies.offboard || bodies.wipeout ||
            diagnostic.ground != 100 ||
            reader.value<std::uint8_t>(skater.skater_identity, 0x7e0) != 0 ||
            !std::isfinite(bodies.seconds) || bodies.seconds <= 0 || bodies.seconds > native_grip_config().max_frame_seconds) {
            reject(car_grab_hud::Cause::not_on_board, runtime.attached); return;
        }
        // Read native braking and verify local ownership before every no-write
        // wait too. A stale traffic update must never swallow a release/ollie.
        const auto velocity = reader.value<std::array<float, 3>>(bodies.parts[0], 0x70);
        const auto requests = reader.value<std::uint32_t>(bodies.context, 0x13c4);
        const bool braking = (requests & 4u) != 0 ||
            ((requests & 2u) != 0 && std::abs(reader.value<float>(bodies.context, 0x1880)) > .05f);
        reader.verify();
        diagnostic.skater_motion = true;
        diagnostic.skater_position = {skater.skater_position[0], skater.skater_position[1], skater.skater_position[2]};
        diagnostic.skater_velocity = {velocity[0], velocity[1], velocity[2]};
        auto input = controls(fresh);
        input.grab = grab; input.brake = input.brake || braking;
        input.steer = analog.right;
        diagnostic.brake = input.brake;
        const auto suspend = [&](bool client_expired, bool inflight_removal = false) {
            if (!runtime.attached || !runtime.held_vehicle || !input.grab || input.brake || input.jump || !std::isfinite(input.steer) ||
                runtime.held_traffic.scope.base != lease.base || runtime.held_traffic.scope.client != lease.client ||
                runtime.held_traffic.scope.client_context != lease.context) return false;
            car_grab_native::CurrentReport retained;
            const bool verified = client_expired || inflight_removal
                ? car_grab_native::retained_identity_current(runtime.held_traffic, runtime.held_vehicle, inflight_removal)
                : car_grab_native::current(runtime.held_traffic, runtime.held_vehicle, &retained) || retained.identity_verified;
            if (!runtime.continuity.can_wait(now_seconds(), runtime.held_traffic.epoch,
                    skater.skater_identity, runtime.held_vehicle, input.grab, verified)) return false;
            // Keep only the existing lock. Do not advance the controller here,
            // write velocity/reach, or refresh the continuity deadline.
            runtime.reach = {}; car_grab_native::clear_reach();
            status.world = runtime.held_traffic.epoch;
            status.phase = car_grab_hud::Phase::unavailable;
            status.cause = car_grab_hud::Cause::vehicle_stale;
            diagnostic.result_valid = true;
            diagnostic.result.attached = diagnostic.result.suspended = true;
            diagnostic.result.write_velocity = false; diagnostic.result.velocity_delta = {};
            diagnostic.result.vehicle_id = runtime.held_vehicle;
            diagnostic.outcome = PhysicsOutcome::unavailable;
            feedback(runtime, status); physics_diagnostic(runtime, diagnostic);
            return true;
        };
        if (lease_now >= lease.expires) {
            diagnostic.gate = PhysicsGate::client_lease;
            if (suspend(true)) return;
            if (runtime.attached && runtime.held_vehicle) {
                (void)car_grab_native::current(runtime.held_traffic, runtime.held_vehicle, &diagnostic.current);
                if (removal_waitable(diagnostic.current) && suspend(true, true)) return;
            }
            reject(input.brake ? car_grab_hud::Cause::braked : input.jump ? car_grab_hud::Cause::jumped :
                !raw.grab ? car_grab_hud::Cause::let_go : car_grab_hud::Cause::controls_busy, raw.grab);
            return;
        }
        diagnostic.gate = PhysicsGate::traffic_scope;
        car_grab_native::TrafficSnapshot traffic;
        const bool traffic_scope = latest_traffic(lease, traffic);
        diagnostic.traffic_scoped = traffic_scope;
        diagnostic.traffic_epoch = traffic.epoch;
        if (traffic_scope) status.world = traffic.epoch;
        if (traffic_scope) diagnostic.gate = PhysicsGate::traffic_current;
        if (!traffic_scope || !car_grab_native::current(traffic, 0, &diagnostic.current)) {
            if (traffic_scope && ((diagnostic.current.identity_verified && suspend(false)) ||
                (removal_waitable(diagnostic.current) && suspend(false, true)))) return;
            // Warming cannot arm a write, but it must not discard an already
            // observed button release when there has been no attachment.
            const bool lost_attachment = runtime.attached;
            runtime.controller.reset(); runtime.attached = false;
            runtime.continuity.clear(); runtime.held_vehicle = 0; runtime.held_traffic = {};
            if (lost_attachment && raw.grab) runtime.input_gate.invalidate();
            car_grab_native::clear_reach();
            status.phase = raw.grab ? car_grab_hud::Phase::unavailable : car_grab_hud::Phase::hidden;
            status.cause = car_grab_hud::Cause::vehicle_stale;
            if (raw.grab) search_diagnostic(runtime, traffic, car_grab::TargetStatus::stale_or_invalid);
            feedback(runtime, status);
            diagnostic.outcome = PhysicsOutcome::unavailable;
            physics_diagnostic(runtime, diagnostic); return;
        }
        // A ready collection can contain other cars while the selected car's
        // motion warms up. Preserve its exact incarnation, not a substitute.
        if (runtime.attached && runtime.held_vehicle &&
            std::none_of(traffic.vehicles.begin(), traffic.vehicles.end(),
                [&](const auto& vehicle) { return vehicle.id == runtime.held_vehicle; })) {
            car_grab_native::CurrentReport retained;
            if (!car_grab_native::current(runtime.held_traffic, runtime.held_vehicle, &retained) && retained.motion_warming) {
                diagnostic.current = retained;
                if (retained.identity_verified && suspend(false)) return;
                reject(car_grab_hud::Cause::vehicle_stale, raw.grab); return;
            }
        }
        diagnostic.gate = PhysicsGate::frame;
        car_grab::Frame frame;
        frame.now = now_seconds();
        frame.seconds = bodies.seconds;
        frame.world = traffic.epoch;
        frame.offline = true;
        frame.interactive = lease.analog.active && analog.active;
        frame.skater = {skater.skater_identity,
            {skater.skater_position[0], skater.skater_position[1], skater.skater_position[2]},
            {velocity[0], velocity[1], velocity[2]}, true, true, false, false};
        car_grab_native::ReachFeedback hand_feedback;
        const bool have_hand_feedback = runtime.reach.entity == skater.skater_identity &&
            runtime.reach.world == frame.world && runtime.reach.base == lease.base &&
            runtime.reach.client == lease.client && runtime.reach.context == lease.context &&
            car_grab_native::read_reach_feedback(runtime.reach, hand_feedback) &&
            hand_feedback.grip_reach.valid && std::isfinite(hand_feedback.grip_reach.sampled_at) &&
            frame.now >= hand_feedback.grip_reach.sampled_at &&
            frame.now - hand_feedback.grip_reach.sampled_at <= .15;
        if (have_hand_feedback) frame.skater.grip_reach = hand_feedback.grip_reach;
        frame.input = input;
        frame.vehicles = traffic.vehicles;
        diagnostic.gate = PhysicsGate::controller;
        const auto result = runtime.controller.step(frame);
        const auto target = car_grab::inspect_target(frame, native_grip_config(), result.attached ? result.vehicle_id : 0);
        diagnostic.result = result; diagnostic.result_valid = true;
        diagnostic.target = target; diagnostic.target_valid = target.available && target.eligible;
        for (const auto& vehicle : frame.vehicles) if (target.id && vehicle.id == target.id) {
            diagnostic.target_motion = true;
            diagnostic.target_forward = vehicle.forward; diagnostic.target_velocity = vehicle.velocity;
            diagnostic.target_age_ms = (frame.now - vehicle.sampled_at) * 1000.;
            diagnostic.rear_projection =
                (static_cast<double>(vehicle.position.x) - frame.skater.position.x) * vehicle.forward.x +
                (static_cast<double>(vehicle.position.z) - frame.skater.position.z) * vehicle.forward.z;
            break;
        }
        const auto search = car_grab_hud::search_feedback(target);
        if (raw.grab && (!result.attached || !target.grip.valid)) search_diagnostic(runtime, traffic, target.status);
        status.speed_mps = target.speed; status.distance = target.distance;
        if (result.attached) {
            // Preview geometry can gain a surface while a grab is in progress.
            // Display the controller's locked tether rather than that preview.
            status.distance = static_cast<float>(std::hypot(
                static_cast<double>(result.anchor.x)-frame.skater.position.x,
                static_cast<double>(result.anchor.y)-frame.skater.position.y,
                static_cast<double>(result.anchor.z)-frame.skater.position.z));
        }
        diagnostic.result_distance = status.distance;
        status.camera_valid = lease.camera_valid;
        status.camera = lease.camera; status.vertical_fov = lease.camera_fov;
        status.anchor = {target.grip.point.x, target.grip.point.y, target.grip.point.z};
        status.marker = target.available && target.eligible && target.grip.valid && target.grip.surface_id &&
            (search.phase == car_grab_hud::Phase::available || result.attached);
        status.phase = result.attached ? (result.mode == car_grab::TowMode::kinematic ?
            car_grab_hud::Phase::towing : car_grab_hud::Phase::approaching) :
            raw.grab && !grab ? car_grab_hud::Phase::blocked :
            result.reason != car_grab::DetachReason::none && result.reason != car_grab::DetachReason::released ? car_grab_hud::Phase::blocked :
            search.phase == car_grab_hud::Phase::available ? car_grab_hud::Phase::available :
            input.grab ? search.phase : car_grab_hud::Phase::idle;
        status.cause = raw.grab && !grab ? car_grab_hud::Cause::reset : reason_for(result.reason);
        if (!result.attached && input.grab && result.reason == car_grab::DetachReason::none)
            status.cause = search.cause;
        if (!result.attached && input.brake) status.cause = car_grab_hud::Cause::braked;
        if (!result.attached && input.jump) status.cause = car_grab_hud::Cause::jumped;
        if (runtime.attached && !result.attached && result.reason == car_grab::DetachReason::released)
            status.phase = car_grab_hud::Phase::released;
        if (!result.attached) {
            if (runtime.attached && result.reason == car_grab::DetachReason::released &&
                car_grab_native::current(traffic, runtime.reach.vehicle) &&
                car_grab_native::grip_current(frame.world, runtime.reach.vehicle, runtime.reach.surface_id)) {
                const auto predict = static_cast<float>(std::clamp(frame.now - runtime.reach.sampled_at, 0., .1));
                for (auto* point : {&runtime.reach.point, &runtime.reach.top_point, &runtime.reach.bottom_point}) {
                    point->x += runtime.reach.velocity.x * predict;
                    point->y += runtime.reach.velocity.y * predict;
                    point->z += runtime.reach.velocity.z * predict;
                }
                runtime.reach.active = false; runtime.reach.sampled_at = frame.now;
                car_grab_native::publish_reach(runtime.reach);
            } else {
                // Preserve the single inactive release publication through
                // its 150 ms animation blend; subsequent idle physics ticks
                // must not immediately erase it. Revalidate its car and scope.
                const auto release_age = frame.now - runtime.reach.sampled_at;
                const bool fading = !runtime.reach.active && runtime.reach.entity == skater.skater_identity &&
                    runtime.reach.world == frame.world && release_age >= 0 && release_age <= .15 &&
                    result.reason == car_grab::DetachReason::none && !input.brake && !input.jump &&
                    car_grab_native::current(traffic, runtime.reach.vehicle) &&
                    car_grab_native::grip_current(frame.world, runtime.reach.vehicle, runtime.reach.surface_id);
                if (!fading) car_grab_native::clear_reach();
            }
            runtime.attached = false;
            runtime.continuity.clear(); runtime.held_vehicle = 0; runtime.held_traffic = {};
            feedback(runtime, status);
            diagnostic.gate = PhysicsGate::ready;
            physics_diagnostic(runtime, diagnostic); return;
        }
        diagnostic.gate = PhysicsGate::velocity;
        const bool measured_tow = result.mode == car_grab::TowMode::measured_surface;
        const bool surface_tow = measured_tow || result.transitioning_surface;
        source_require((measured_tow || result.mode == car_grab::TowMode::kinematic) &&
            (!surface_tow || (result.grip.valid && result.grip.surface_id)) &&
            std::isfinite(result.predicted_at) && result.predicted_at == frame.now,
            "Car Grab current attachment unavailable.");
        source_require(std::isfinite(result.velocity_delta.x) && std::isfinite(result.velocity_delta.z) &&
            result.velocity_delta.y == 0, "Car Grab velocity correction rejected.");
        std::array<std::array<float, 3>, 32> staged{};
        std::array<std::uint32_t, 32> flags{};
        SourceReader current;
        for (std::size_t i = 0; result.write_velocity && i < bodies.parts.size(); ++i) {
            staged[i] = current.value<std::array<float, 3>>(bodies.parts[i], 0x70);
            flags[i] = current.value<std::uint32_t>(bodies.parts[i], 0x60);
            staged[i][0] += result.velocity_delta.x;
            staged[i][2] += result.velocity_delta.z;
            for (const auto value : staged[i])
                source_require(std::isfinite(value) && std::abs(value) <= 100000, "Car Grab body velocity rejected.");
        }
        current.verify();
        diagnostic.gate = PhysicsGate::selected_current;
        if (!car_grab_native::current(traffic, result.vehicle_id, &diagnostic.current)) {
            if ((diagnostic.current.identity_verified && suspend(false)) ||
                (removal_waitable(diagnostic.current) && suspend(false, true))) return;
            reject(car_grab_hud::Cause::vehicle_stale); return;
        }
        diagnostic.gate = PhysicsGate::surface_current;
        if (surface_tow && !car_grab_native::grip_current(frame.world, result.vehicle_id, result.grip.surface_id)) {
            // A busy or expired optional contact cannot authorize this write,
            // but a separately verified car can retain its bounded private lock.
            if (suspend(false)) return;
            (void)car_grab_native::current(traffic, result.vehicle_id, &diagnostic.current);
            if (removal_waitable(diagnostic.current) && suspend(false, true)) return;
            reject(car_grab_hud::Cause::vehicle_stale); return;
        }
        if (result.suspended) {
            // Ownership/current leases remain mandatory on a no-velocity-write
            // resync frame. The normal hand path below may publish only this
            // frame's newly evaluated, separately current measured contact.
            const auto now = GetTickCount64();
            // A one-frame event can fall between the general logger's 250 ms
            // records. Keep a separate bounded record of the resync evidence.
            if (now-runtime.suspension_logged >= 1000) {
                logging::printf(logging::Level::info, logging::Channel::skater,
                    "Car Grab resync suspended=1 vehicle=%016llx epoch=%llu mode=%u "
                    "controller_elapsed_ms_capped=%.2f seconds_capped=%.4f traffic_final_ms_capped=%.2f.",
                    static_cast<unsigned long long>(result.vehicle_id), static_cast<unsigned long long>(frame.world),
                    static_cast<unsigned>(result.mode), log_value(result.elapsed_seconds*1000.),
                    log_value(frame.seconds), log_value(diagnostic.current.final_age*1000., diagnostic.current.final_age_valid));
                runtime.suspension_logged = now;
            }
        }
        diagnostic.gate = PhysicsGate::write;
        // debug_write copies only the three XYZ floats, leaving SIMD padding,
        // per-body Y, angular velocity and all unrelated flags untouched.
        for (std::size_t i = 0; result.write_velocity && i < bodies.parts.size(); ++i) {
            debug_write(bodies.parts[i] + 0x70, staged[i]);
            debug_write(bodies.parts[i] + 0x60, flags[i] | 8u);
        }
        // Contact can guide a bounded stance transition on this exact car.
        // Never manufacture a bumper plane from the rear policy offset.
        const auto& contact = surface_tow ? result.grip : target.grip;
        const bool hand_contact = contact.valid && contact.surface_id && target.id == result.vehicle_id &&
            car_grab_native::grip_current(frame.world, result.vehicle_id, contact.surface_id);
        status.marker = hand_contact;
        if (hand_contact) {
            status.anchor = {contact.point.x, contact.point.y, contact.point.z};
            const bool same_feedback = have_hand_feedback && runtime.reach.vehicle == result.vehicle_id &&
                hand_feedback.surface_id == contact.surface_id;
            runtime.reach = {lease.base, lease.client, lease.context, skater.skater_identity,
                frame.world, result.vehicle_id, contact.point, frame.now, true,
                contact.normal, contact.tangent, contact.velocity, contact.surface_id,
                contact.wrap_valid, contact.top_point, contact.bottom_point,
                contact.top_normal, contact.bottom_normal};
            car_grab_native::publish_reach(runtime.reach);
            if (same_feedback) {
                if (hand_feedback.finger_grip) status.phase = car_grab_hud::Phase::attached;
                else if (hand_feedback.palm_contact) status.phase = car_grab_hud::Phase::palm_contact;
            }
        } else {
            runtime.reach = {};
            car_grab_native::clear_reach();
        }
        if (!runtime.attached) {
            logging::printf(logging::Level::info, logging::Channel::skater,
                "Car Grab attached vehicle=%016llx mode=%s distance=%.2f m surface=%016llx.",
                static_cast<unsigned long long>(result.vehicle_id), measured_tow ? "measured_surface" : "kinematic",
                status.distance, static_cast<unsigned long long>(contact.surface_id));
        }
        runtime.attached = true;
        runtime.held_vehicle = result.vehicle_id;
        runtime.held_traffic = std::move(traffic);
        runtime.continuity.remember(frame.now, frame.world, skater.skater_identity, result.vehicle_id);
        const auto pose_status = car_grab_native::reach_status();
        if (pose_status != runtime.pose_status) {
            const auto text = pose_status == car_grab_native::ReachStatus::applied ? "applied" :
                pose_status == car_grab_native::ReachStatus::contact ? "palm contact" :
                pose_status == car_grab_native::ReachStatus::approaching ? "approaching" :
                pose_status == car_grab_native::ReachStatus::blending ? "blending" :
                pose_status == car_grab_native::ReachStatus::unavailable ? "unavailable (native pose guard)" : "idle";
            logging::printf(logging::Level::info, logging::Channel::skater, "Car Grab hand reach: %s.", text);
            runtime.pose_status = pose_status;
        }
        feedback(runtime, status);
        diagnostic.gate = PhysicsGate::ready;
        physics_diagnostic(runtime, diagnostic);
    } catch (const SourceGuard& failure) {
        runtime.controller.reset(); runtime.input_gate.invalidate(); runtime.attached = false;
        runtime.continuity.clear(); runtime.held_vehicle = 0; runtime.held_traffic = {};
        car_grab_native::clear_reach(); car_grab_hud::clear();
        const auto now = GetTickCount64();
        if (now - runtime.logged >= 2000) {
            logging::printf(logging::Level::info, logging::Channel::skater, "Car Grab gated: %s", failure.message);
            runtime.logged = now;
        }
        diagnostic.outcome = PhysicsOutcome::source_guard;
        physics_diagnostic(runtime, diagnostic);
    } catch (...) {
        runtime.controller.reset(); runtime.input_gate.invalidate(); runtime.attached = false;
        runtime.continuity.clear(); runtime.held_vehicle = 0; runtime.held_traffic = {};
        car_grab_native::clear_reach(); car_grab_hud::clear();
        diagnostic.outcome = PhysicsOutcome::unexpected;
        physics_diagnostic(runtime, diagnostic);
    }
}
}
