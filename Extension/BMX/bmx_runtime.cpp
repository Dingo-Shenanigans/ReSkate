#include "bmx_runtime.h"
#include "Engine/Game/BMX/bmx_coupled_trial_marker_policy.h"
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "bmx_native_coupled.h"
#endif
#include "bmx_native_motion.h"
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "Engine/Game/BMX/bmx_native_startup_owner.h"
#endif
#include "bmx_chassis_ride.h"
#include "bmx_ride_trace.h"
#include "bmx_fall_lifecycle.h"
#include "bmx_bounded_fifo.h"
#include "bmx_getup_lab.h"
#include "bmx_intent_observer.h"
#include "bmx_skeleton_phase_probe.h"
#include "bmx_native_chassis.h"
#include "bmx_bail.h"
#include "bmx_impact_test.h"
#include "bmx_impact_receipt.h"
#include "bmx_catalog_test.h"
#include "Engine/Game/BMX/bmx_automated_ride.h"
#include "Engine/Game/BMX/bmx_automated_switch.h"
#include "Engine/Game/BMX/bmx_quick_switch_rehearsal.h"
#include "bmx_world_transform.h"
#include "bmx_player_startup.h"
#include "bmx_ecs_diagnostics.h"
#include "bmx_visual.h"
#include "bmx_rider.h"
#if defined(DINGOSDK_BMX_NATIVE_COUPLED) && defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
#include "bmx_coupled_ride.h"
#include "bmx_coupled_avatar_frame.h"
#endif
#include "bmx_board_handoff_trace.h"
#include "bmx_rider_motion.h"
#include "bmx_selftest.h"
#include "bmx_ramp_test.h"
#include "bmx_ramp_rehearsal.h"
#include "bmx_world.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/BMX/bmx_controls.h"
#include "Engine/Game/BMX/physics_clock.h"
#include "Engine/Game/BMX/equipment_switch.h"
#include "Engine/Game/BMX/bmx_interaction.h"
#include "Engine/Game/BMX/mount_readiness.h"
#include "Engine/Game/BMX/bmx_owner_observation.h"
#include "Engine/Game/BMX/chassis_transform.h"
#include "Engine/Game/BMX/bmx_mount_transition.h"
#include "bmx_collision_envelope.h"
#include "bmx_crash_recovery.h"
#include "Engine/Game/BMX/bmx_geometry.h"
#include "Engine/Game/Skater/source_inventory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <memory>
#include <sstream>
#include <fstream>
#include <chrono>

namespace dingosdk::bmx {
namespace {
Json queried_surface_json(const approach_surface::Surface& v) {
    return Json{{"available",v.available},{"verified_plane",v.plane},{"point",v.available?Json(v.point):Json(nullptr)},
        {"normal",v.available?Json(v.normal):Json(nullptr)},{"forward",v.available?Json(v.forward):Json(nullptr)},
        {"separation",v.available?Json(v.separation):Json(nullptr)},{"hit_entity",v.entity},{"hit_body",v.body}};
}
Json approach_geometry_json(const approach_surface::Result& v) {
    Json rays=Json::array();for(unsigned i=0;i<v.rays&&i<v.observations.size();++i){const auto& r=v.observations[i];
        rays.push_back(Json{{"from",r.from},{"to",r.to},{"available",r.hit.available},{"hit",r.hit.hit},
            {"point",r.hit.available&&r.hit.hit?Json(r.hit.point):Json(nullptr)},
            {"fraction",r.hit.available&&r.hit.hit?Json(r.hit.fraction):Json(nullptr)},
            {"hit_entity",r.hit.entity},{"hit_body",r.hit.body},{"native_casts",r.hit.native_casts},
            {"elapsed_microseconds",r.hit.elapsed_microseconds},{"budget_exhausted",r.hit.budget_exhausted}});}
    return Json{{"selected",v.selected},{"considered",v.considered},{"qualified_geometry",v.qualified},
        {"issue",approach_surface::name(v.issue)},{"epoch_before",v.epoch_before},{"epoch_after",v.epoch_after},
        {"ray_count",v.rays},{"native_casts",v.native_casts},{"query_microseconds",v.query_microseconds},
        {"normal_velocity",v.normal_velocity},{"linear_end_gap",v.linear_end_gap},
        {"rotation_remainder",v.rotation_remainder},{"time_to_plane",v.time_to_plane},{"target_normal_velocity",v.target_normal_velocity},
        {"projected_center",v.considered?Json(v.projected_center):Json(nullptr)},
        {"fitted_point",v.qualified?Json(v.fitted_point):Json(nullptr)},{"fitted_normal",v.qualified?Json(v.fitted_normal):Json(nullptr)},
        {"maximum_plane_error",v.maximum_plane_error},{"normal_dot",v.normal_dot},{"fit_residual",v.fit_residual},
        {"rays",std::move(rays)},{"boundary","geometry witness; separate private normal solver may consume it; not actual contact/support/native consumption"}};
}
Json landing_contact_compliance_json(const landing_contact_compliance::Result& v) {
    return Json{{"phase",landing_contact_compliance::name(v.phase)},{"active",v.active},
        {"activated",v.activated},{"released",v.released},
        {"boundary","existing qualified pump landing pulse; phase selection only; actual compliant mask and fallback remain separate"}};
}
Json contact_compliance_json(bool admitted,const contact_compliance::Result& v,double predicted_gap,bool deflection_satisfied) {
    if(!admitted)return Json(nullptr);
    return Json{{"raw_normal_impulse",v.impulse},{"raw_soft_impulse",v.soft_impulse},{"raw_deflection_floor_impulse",v.deflection_floor_impulse},
        {"deflection_stop_active",v.backstop_active},{"gamma",v.gamma},{"equivalent_damping",v.equivalent_damping},
        {"legacy_target_velocity",v.target_velocity},{"deflection_limit_velocity",v.safe_velocity},
        {"stiffness",contact_compliance::stiffness},{"maximum_deflection",contact_compliance::maximum_deflection},
        {"predicted_gap_after_shared_scale",predicted_gap},{"linear_deflection_satisfied",deflection_satisfied},
        {"boundary","linear current-point prediction after original caps; not native collision clearance or shape-sweep proof"}};
}
Json precontact_prediction_json(bool available,const tire::Body& v) {
    if(!available)return Json(nullptr);
    return Json{{"center_of_mass",v.center_of_mass},{"orientation",v.orientation},{"mass",v.mass},{"inertia",v.inertia_body},
        {"linear_velocity",v.linear_velocity},{"angular_velocity",v.angular_velocity},
        {"boundary","exact existing pure pre-contact prediction; one native gravity step already included; no second command"}};
}
constexpr std::string_view mounted_controls =
    "BMX mounted. Left stick steers; right down/up hops; A/Cross pedals, B/Circle brakes, R3 quick hops, Y/Triangle dismounts.";
Tuning prototype_tuning() {
    Tuning t;
    // The first native adapter uses wheel rays and a small chassis ray fan. Keep the
    // speed conservative until a proper swept tire/chassis solver has been verified.
    t.forward_limit = 6.0; t.reverse_limit = 3.0;
    return t;
}
struct GetupLab {
    bool requested{},accepted{},down_before_action{},fall_receipt_fresh{},logged{},trace_flushed{},report_written{};
    bool audit_started{};
    RetainedBikeIdentity identity;
    std::uint64_t requested_serial{},fall_request_id{},fall_generation{};
    double requested_time{};
    Json preflight,baseline;
    std::filesystem::path root;
    std::string trace_filename;
    unsigned write_attempts{};
    ULONGLONG write_after{};
};
struct PendingFallReport {
    fall_lifecycle::History<> history;
    std::filesystem::path root;
    std::uint64_t getup_generation{},sequence{};
    ULONGLONG write_after{};
    unsigned write_attempts{};
};
struct QuickTestObservation {
    bool present{},motion_enabled{},native_read_attempted{},lifecycle_allowed{},physical_quiet{},focused{};
    bool ui_available{},ui_menu{},overlay_ready{},overlay_menu{},capture_blocked{},menu_requested{};
    bool pool_ready{},pool_failed{},pool_blocked{},pool_quarantined{},pool_suspended{},pool_deferred{},pool_detached{};
    std::uint64_t wall_ms{};
    const char* stage="none";
    const char* phase="idle";
    quick_switch_rehearsal::Frame frame;
    std::string native_issue,pool_issue;
};
struct Runtime {
    std::atomic<bool> active{};
    std::atomic<int> request{};
    std::atomic<std::uintptr_t> observed_client{};
    std::atomic<std::uint32_t> observed_physics{};
    std::atomic<bool> observed_physics_valid{}, observed_capture{};
    overlay::BmxInputSample observed_input; // the one existing client-tick sample; never repoll here
    bool native_board_control_input_allowed{};
    std::mutex status_mutex;
    std::string interaction_status = "; bike mode=none; rider mode=none; setdown verified=false; body token=0";
    std::string message = "BMX prototype idle. Step off the skateboard, then run bmx mount.";
    Model bike{prototype_tuning()};
    InputMapper mapper;
    PhysicsClock clock;
    std::uintptr_t client{}, entity{}, core{}, context{}, rig{};
    float root_height{};
    double barspin{}, tailwhip{}, bar_time{}, whip_time{};
    float preload{}, front_weight{}, rear_weight{};
    visual::Frame world_frame;
    bool interrupted{}, pending{}, observation_input_rearm{};
    EquipmentToggleKey toggle_key;
    EquipmentToggleKey interact_key,carry_key;
    BikeInteractionButton interaction_button;
    BikeQuickSwitchButton quick_switch_button;
    quick_switch_rehearsal::Preflight quick_preflight;
    quick_switch_rehearsal::Scenario quick_test;
    quick_switch_rehearsal::BoardTransitionEvidence quick_board_request;
    QuickTestObservation quick_first_attempt,quick_last_attempt,quick_first_rejection;
    std::string quick_rejection_issue,quick_rejection_relation;
    bool quick_test_tick_visited{};
    unsigned quick_test_strict_setdowns{},quick_test_setdown_followups{},quick_test_report_attempts{};
    bool quick_test_setdown_mount_queued{};
    automated_switch::Lease switch_test_lease;
    bool switch_test_expiry_reported{};
    visual::Frame parked_bike;
    EquipmentOwner parked_owner;
    std::uintptr_t parked_world{},parked_context{};
    std::uint64_t parked_token{};
    bool bike_parked{},bike_carried{},mount_from_parked{};
    bool bike_crashed{};
    bool crash_interaction_ready{};
    crash_recovery::InteractionAdmission crash_interaction;
    native_chassis::Frame crash_body;
    RetainedBikeIdentity crash_identity; // one full association; owner handoff spends a new token
    RetainedBikeIdentity parked_identity;
    bool owner_gameplay_ready{};
    owner_observation::ReleasedUnwind owner_unwind;
    double crash_native_time{};
    std::uint64_t crash_serial{};
    ULONGLONG crash_last_observed{};
    Json::Array crash_samples;
    bool parked_ground_verified{};
    float mount_blend{1};
    mount_transition::Transition mount_entry;
    std::uint64_t mount_entry_token{};
    bool mount_entry_began{},mount_entry_completed{};
    bool stepping_off{},carry_after_step{};
    std::array<float,3> step_from{},step_to{};
    float step_progress{};
    std::uint64_t step_serial{};
    SkateboardSwitch board_switch;
    StepOffBoardIntent step_off_board;
    native_input_handoff::Ticket private_native_input_ticket;
    board_handoff_policy::TerminalClientTarget private_native_input_terminal;
    bool private_native_input_started{};
    bool human_native_input_pending{};
    bool human_native_return_pending{},human_native_return_mount{};
    bool human_native_return_tick_visited{};
    std::uint64_t human_native_return_until{};
    native_input_handoff::Ticket human_native_input_ticket;
    unsigned human_native_reports{},human_native_report_drops{};
    PendingMountOwner mount_owner;
    PendingMountDismount mount_dismount;
    NearbyMountSearch mount_search;
    SetDownSearch setdown_search;
    bool setdown_mount_after{},setdown_from_obstacle{},carry_obstacle_refused{};
    ULONGLONG setdown_until{};
    ULONGLONG pending_until{}, dismount_retry_after{};
    std::array<float, 3> velocity{};
    bool dismount_requested{}, rehearsal{};
    double rehearsal_seconds{}, max_distance{}, max_height{}, max_turn{}, initial_yaw{};
    selftest::RightTurn right_turn;
    selftest::ObservedBikeTurn observed_bike_turn;
    double before_brake_speed{}, after_brake_speed = 1000, final_speed = 1000;
    bool before_brake_measured{}, after_brake_measured{}, final_speed_measured{};
    std::array<float, 3> initial_position{};
    std::array<float, 3> previous_position{};
    std::uint64_t initial_updates{}, initial_draws{}, initial_poses{};
    std::uint64_t initial_world_placements{}, initial_render_poses{};
    ULONGLONG rehearsal_started{};
    selftest::Mode rehearsal_mode{selftest::Mode::focused};
    std::filesystem::path rehearsal_root;
    bool background_input_used{}, rehearsal_expired{};
    std::uint64_t unfocused_ticks{};
    double background_seconds{};
    bool native_chassis_mode{}, chassis_started{};
    chassis_ride::Step chassis_step;
    ride_trace::History<> ride_history, pending_ride_history;
    fall_lifecycle::History<> fall_history,lab_fall_history;
    // Runtime is heap-owned. All eight large report slots are filled in place;
    // native sampling and sealing never allocate a report or copy it to stack.
    BoundedFifo<PendingFallReport,8> pending_fall_reports;
    std::filesystem::path fall_root;
    std::uint64_t fall_getup_generation{},fall_sequence{},fall_last_dropped_sequence{};
    ULONGLONG fall_started{};
    unsigned fall_files{},fall_dropped{},fall_queue_dropped{},fall_write_dropped{};
    GetupLab getup_lab;
    std::filesystem::path ride_trace_root,pending_ride_trace_root;
    bool ride_trace_pending{};
    ULONGLONG ride_trace_after{};
    unsigned ride_trace_files{},ride_trace_dropped{};
    std::string ride_trace_reason;
    std::uintptr_t ride_trace_entity{};
    bool ride_trace_has_fall_request{},ride_trace_fall_accepted{},ride_trace_fall_status_fresh{};
    bail::Status ride_trace_fall_status;
    Json mount_seed_last;
    std::uint64_t mount_seed_attempts{};
    ULONGLONG mount_seed_next_log{};
    Json::Array chassis_samples;
    double chassis_tracking_error{};
    float chassis_ground_height{};
    bool chassis_ground_known{};
    std::array<float,3> chassis_initial_com{};
    bool chassis_com_known{}, chassis_before_measured{}, chassis_after_measured{}, chassis_final_measured{};
    double chassis_distance{}, chassis_before_brake{}, chassis_after_brake = 1000, chassis_final_speed = 1000;
    double chassis_pre_hop_y{}, chassis_hop_rise{}, chassis_takeoff_speed{};
    bool chassis_pause_requested{}, chassis_pause_active{};
    native_chassis::Frame chassis_pause_frame;
    native_chassis::Status chassis_pause_status;
    double chassis_pause_until{}, chassis_pause_position_error{}, chassis_pause_rotation_error{};
    ULONGLONG chassis_pause_started{};
    bool trick_rehearsal{};
    bool catalog_rehearsal{};
    std::uint64_t catalog_peak_render_poses{},catalog_last_render_count{};
    float catalog_max_render_phase{};
    // Sampled completed Superman render writes only; skipped count deltas are not inferred.
    std::uint64_t catalog_observed_requested_writes{},catalog_observed_applied_writes{},catalog_observed_invalid_writes{};
    float catalog_observed_min_strength{},catalog_observed_max_strength{};
    bool ramp_rehearsal{},ramp_request_consumed{}; // one measured route within the verified run-up and landing footprint
    ramp_test::RideRoute ramp_route;
    ::dingosdk::bmx::ramp_rehearsal::Driver ramp_driver;
    double trick_bar_release{},trick_tail_release{},trick_pitch_command{},trick_pitch_torque{},trick_pitch_rate{};
    std::uint64_t trick_bar_catches{},trick_tail_catches{};
    std::uint64_t trick_pitch_serial{},trick_pitch_response_serial{};
    std::array<float,3> trick_pitch_axis{};
    double trick_pitch_before_rate{},trick_pitch_response{};
    double chassis_min_contact_scale = 1;
    ULONGLONG chassis_next_progress{};
    double chassis_bridge_ms{},chassis_bridge_peak_ms{},chassis_world_ms{},chassis_world_peak_ms{};
    std::uint64_t chassis_bridge_calls{},chassis_world_calls{};
    bool impact_pending{};
    ULONGLONG impact_started{};
    chassis_ride::Step impact_step;
    impact_receipt::Receipt impact_receipt;
};
Runtime& runtime() { static auto* value = new Runtime; return *value; }

void retain_quick_test_rejection(Runtime& r,const char* issue,bool current_attempt) {
    if(!r.quick_rejection_issue.empty())return;
    r.quick_rejection_issue=issue;
    if(r.quick_last_attempt.present) {
        r.quick_first_rejection=r.quick_last_attempt;
        r.quick_rejection_relation=current_attempt?"current_native_attempt":"prior_native_attempt_no_current_read";
    } else r.quick_rejection_relation="no_native_attempt_available";
}


Json quick_test_frame_json(const quick_switch_rehearsal::Frame& v) {
    const auto& w=v.board_transition;
    const Json board{{"pending_for_owner",w.pending_for_owner},{"request_clock_valid",w.request_clock_valid},
        {"parked_pool_proven",w.parked_pool_proven},{"clock_unchanged",w.clock_unchanged},{"physics_unchanged",w.physics_unchanged},
        {"accepted_requests",w.accepted_requests},{"observed_wall_ms",w.observed_wall_ms},{"pending_deadline_ms",w.pending_deadline_ms},
        {"request_wall_ms",w.request_wall_ms},{"request_serial",w.request_serial},
        {"request_native_time",std::isfinite(w.request_native_time)?Json(w.request_native_time):Json()},
        {"world",w.world},{"token",w.token},
        {"request_owner",std::array<std::uintptr_t,5>{w.request_owner.client,w.request_owner.entity,w.request_owner.core,w.request_owner.context,w.request_owner.rig}}};
    return Json{{"owner",std::array<std::uintptr_t,5>{v.owner.client,v.owner.entity,v.owner.core,v.owner.context,v.owner.rig}},
        {"available",v.available},{"timing_available",v.timing_available},{"physics_available",v.physics_available},
        {"physics",v.physics},{"offboard",v.offboard},{"standable",v.standable},{"motion_supported",v.motion_supported},
        {"serial",v.serial},{"native_time",std::isfinite(v.native_time)?Json(v.native_time):Json()},
        {"native_time_finite",std::isfinite(v.native_time)},{"active",v.active},{"pending",v.pending},{"mounting",v.mounting},
        {"parked",v.parked},{"carried",v.carried},{"crashed",v.crashed},{"pool_active",v.pool_active},
        {"pool_parked",v.pool_parked},{"pool_failed",v.pool_failed},{"pool_suspended",v.pool_suspended},
        {"capture",v.capture},{"world",v.world},{"allocations",v.allocations},{"mounts",v.mounts},
        {"token",v.token},{"applications",v.applications},{"parts",v.parts},{"strict_setdowns",v.strict_setdowns},{"board_transition",board}};
}
Json quick_test_observation_json(const QuickTestObservation& v) {
    if(!v.present)return Json();
    return Json{{"stage",v.stage},{"phase",v.phase},{"wall_ms",v.wall_ms},
        {"native_motion_enabled",v.motion_enabled},{"native_frame_read_attempted",v.native_read_attempted},
        {"native_frame_issue",v.native_issue},{"lifecycle_allowed",v.lifecycle_allowed},
        {"physical_quiet",v.physical_quiet},{"focused",v.focused},{"native_ui_available",v.ui_available},
        {"native_ui_menu",v.ui_menu},{"overlay_ready",v.overlay_ready},{"overlay_menu",v.overlay_menu},
        {"capture_blocked",v.capture_blocked},{"menu_requested",v.menu_requested},
        {"pool_ready",v.pool_ready},{"pool_failed",v.pool_failed},{"pool_blocked",v.pool_blocked},
        {"pool_quarantined",v.pool_quarantined},{"pool_suspended",v.pool_suspended},
        {"pool_deferred_park",v.pool_deferred},{"pool_detached",v.pool_detached},{"pool_issue",v.pool_issue},
        {"frame",quick_test_frame_json(v.frame)}};
}
// A data-only terminal receipt. The selector never enables a hardware source;
// a fresh focus/menu/native owner is still required on every test client tick.
void report_quick_test() noexcept {
    auto& r=runtime();if(!r.quick_test.report_pending()||r.quick_test_report_attempts>=3)return;
    ++r.quick_test_report_attempts;
    if(r.quick_test.terminal()&&!r.quick_test.passed())board_handoff_trace::seal_quick_switch_failure();
    try {
        const auto root=user_data::override_root();if(!root||!quick_switch_rehearsal::enabled())return;
        auto observations=Json::array(),pulses=Json::array(),setup=Json::array();
        for(unsigned i=0;i<r.quick_test.observation_count();++i) {
            const auto& v=r.quick_test.observations()[i];
            observations.push_back(Json{{"phase",quick_switch_rehearsal::phase_name(v.phase)},
                {"serial",v.serial},{"native_time",v.native_time},{"token",v.token},{"applications",v.applications},
                {"world",v.world},{"allocations",v.allocations},{"mounts",v.mounts},{"parts",v.parts},
                {"offboard",v.offboard},{"standable",v.standable},{"active",v.active},{"parked",v.parked},
                {"carried",v.carried},{"pending",v.pending},{"mounting",v.mounting},{"crashed",v.crashed},
                {"pool_active",v.pool_active},{"pool_parked",v.pool_parked},
                {"capture",v.capture},{"motion_supported",v.motion_supported},{"physics",v.physics},{"strict_setdowns",v.strict_setdowns},
                {"owner",std::array<std::uintptr_t,5>{v.owner.client,v.owner.entity,v.owner.core,v.owner.context,v.owner.rig}}});
        }
        for(unsigned i=0;i<r.quick_test.pulse_count();++i) {
            const auto& v=r.quick_test.pulses()[i];
            pulses.push_back(Json{{"index",v.index},{"samples",v.samples},{"quick_events",v.quick_events},
                {"ordinary_events",v.ordinary_events},{"route",v.route},{"first_serial",v.first_serial},
                {"edge_serial",v.edge_serial},{"release_serial",v.release_serial},{"last_serial",v.last_serial},
                {"first_time",v.first_time},{"edge_time",v.edge_time},{"release_time",v.release_time},{"last_time",v.last_time},
                {"north_capture_requested",v.north_capture_requested},{"released",v.released}});
        }
        for(unsigned i=0;i<r.quick_test.setup_count();++i)setup.push_back(r.quick_test.setup_requests()[i]);
        const Json preflight{{"completed",r.quick_preflight.terminal()},{"passed",r.quick_preflight.qualified()},
            {"issue",r.quick_preflight.issue()},{"samples",r.quick_preflight.samples()},
            {"wall_started_ms",r.quick_preflight.wall_started()},{"elapsed_ms",r.quick_preflight.elapsed_ms()},
            {"wall_limit_ms",quick_switch_rehearsal::preflight_limit_ms},
            {"first_clock",r.quick_preflight.clock_seen()?quick_test_frame_json(r.quick_preflight.first_clock()):Json()},
            {"qualified_clock",r.quick_preflight.qualified()?quick_test_frame_json(r.quick_preflight.qualified_clock()):Json()}};
        const auto& history=r.quick_test.board_transitions();auto states=Json::array();
        const auto transition_state=[&](unsigned physics,unsigned count,const auto& first,const auto& last) {
            return Json{{"physics",physics},{"samples",count},{"phase","native_board"},
                {"first",count?quick_test_frame_json(first):Json()},{"last",count?quick_test_frame_json(last):Json()}};
        };
        states.push_back(transition_state(200,history.samples200,history.first200,history.last200));
        states.push_back(transition_state(201,history.samples201,history.first201,history.last201));
        const Json transitions{{"observed",history.samples200+history.samples201>0},
            {"samples",history.samples200+history.samples201},{"states",std::move(states)},
            {"grounded_proven",history.grounded_proven},
            {"grounded_first",history.grounded_proven?quick_test_frame_json(history.grounded_first):Json()},
            {"grounded_confirmed",history.grounded_proven?quick_test_frame_json(history.grounded_confirmed):Json()}};
        const auto step_basis=native_motion_basis_status();
        const Json step_off_basis{{"scope","process_sdk_callbacks"},{"policy","native_supplied_step_off_only"},
            {"counter_limit",motion_target::counter_limit},{"target_calls",step_basis.step_off_target_calls},
            {"basis_word_matches",step_basis.step_off_basis_word_matches},{"basis_word_mismatches",step_basis.step_off_basis_word_mismatches},
            {"native_completion_proven",false}};
        const Json report{{"schema_version",3},{"process_id",GetCurrentProcessId()},{"completed",true},
            {"passed",r.quick_test.passed()},{"issue",r.quick_test.issue()},
            {"step_off_motion_basis",step_off_basis},{"board_transition_receipt",transitions},{"preflight",preflight},{"first_native_observation",quick_test_observation_json(r.quick_first_attempt)},
            {"last_native_observation",quick_test_observation_json(r.quick_last_attempt)},
            {"first_rejected_native_observation",quick_test_observation_json(r.quick_first_rejection)},
            {"first_rejection_issue",r.quick_rejection_issue},{"first_rejection_observation_relation",r.quick_rejection_relation},
            {"input_source","synthetic_virtual_device"},{"virtual_device",quick_switch_rehearsal::virtual_device},
            {"physical_controller_verified",false},{"xinput_first_packet_verified",false},{"dpad_hardware_verified",false},
            {"private_native_input_handoff",overlay::detail::private_bmx_native_input_handoff_requested()?
                Json::parse(overlay::detail::private_bmx_native_input_handoff_status()):Json(nullptr)},
            {"hold_native_seconds",quick_switch_rehearsal::hold_seconds},
            {"wall_limit_ms",quick_switch_rehearsal::wall_limit_ms},
            {"guarded_setdown_mount_followup_requests",r.quick_test_setdown_followups},
            {"setup_requests",std::move(setup)},{"pulses",std::move(pulses)},{"observations",std::move(observations)}};
        std::ofstream output(*root/L"bmx-quick-switch.json",std::ios::trunc);
        output<<report.dump(2)<<'\n';output.flush();
        if(output.good())r.quick_test.mark_reported();
    }catch(...){} // Supervisor still fails closed if a terminal receipt cannot be archived.
}

// Final-report serialization only, never called by the native snapshot reader.
Json velocity_rejections_json(const snapshot_retry::VelocityRejections& history) {
    auto records=Json::array();
    const auto header=[](const snapshot_retry::VelocityRingHeader& value) {
        return Json{{"values",value.values},{"elements",value.elements},
            {"current_plane",value.current_plane},{"planes",value.planes}};
    };
    for(unsigned i=0;i<history.stored&&i<history.records.size();++i) {
        const auto& record=history.records[i];const auto& epoch=record.initial_epoch;
        records.push_back(Json{{"initial_owner_world",epoch.owner_world},
            {"body_index",epoch.body_index},{"body_generation",epoch.body_generation},
            {"token",epoch.token},{"serial",epoch.serial},{"native_time",epoch.time},
            {"native_step_seconds",epoch.seconds},{"initial_calibrated",record.initial_calibrated},
            {"read_attempt",record.read_attempt},{"validation_pass",record.validation_pass},
            {"ring_index",record.ring_index},{"ring_offset",record.ring_index==2?0x930:0x940},
            {"first_ring_header",header(record.first_header)},
            {"rejected_ring_header",header(record.rejected_header)},{"raw_uint32",record.raw_words}});
    }
    return Json{{"schema_version",1},{"issue_kind","invalid_velocity"},{"observed",history.observations>0},
        {"capacity",history.records.size()},
        {"stored",history.stored},{"observations",history.observations},
        {"same_key_suppressed",history.same_key_suppressed},{"capacity_drops",history.capacity_drops},
        {"retention","first rejection for first four distinct qualified pool/body/token keys; no eviction through park/reset; later keys counted as drops"},
        {"owner_world_order","client,entity,core,skater_context,rig,world,native_state,client_context"},
        {"owner_provenance","initial SDK qualification only; retained pointers are historical scalars"},
        {"raw_lane_order","x,y,z,unnamed W; W physical meaning unresolved"},
        {"read_key","attempt1..3; validation_pass0 initial, 1 second ring read, 2 final ring read; ring2 linear, 3 angular"},
        {"rejected_observation_postqualification_performed",false},
        {"coherent_rejected_snapshot_proved",false},{"native_completion_proved",false},
        {"records",std::move(records)}};
}
void begin_ride_trace() {
    auto& r=runtime();r.ride_history.reset();r.ride_trace_root.clear();
    wchar_t flag[2]{},image[32768]{};
    if(GetEnvironmentVariableW(L"RESKATE_BMX_RIDE_TRACE",flag,2)!=1||flag[0]!=L'1'||
       !user_data::development_requested()||r.ride_trace_files>=64)return;
    const auto root=user_data::override_root();if(!root)return;
    const auto length=GetModuleFileNameW(nullptr,image,static_cast<DWORD>(std::size(image)));
    if(!length||length>=std::size(image))return;
    const auto executable=user_data::detail::canonical(std::filesystem::path(image));
    const auto isolated=user_data::detail::canonical(root->parent_path()/L"bmx-game");
    if(_wcsicmp(executable.filename().c_str(),L"Skate.exe")||executable.parent_path()!=isolated)return;
    r.ride_trace_root=*root/L"bmx-traces";
}
void freeze_ride_trace(std::string_view reason) {
    auto& r=runtime();
    if(r.ride_trace_files>=64||r.ride_trace_root.empty()||!r.ride_history.seal())return;
    if(r.ride_trace_pending){++r.ride_trace_dropped;return;}
    r.pending_ride_history=r.ride_history;r.ride_trace_reason=reason;
    r.ride_trace_has_fall_request=r.ride_trace_fall_accepted=r.ride_trace_fall_status_fresh=false;r.ride_trace_fall_status={};
    r.pending_ride_trace_root=r.ride_trace_root;
    r.ride_trace_entity=r.entity;r.ride_trace_pending=true;
    // Fall handoff gets the next native callbacks before JSON or file IO runs.
    r.ride_trace_after=GetTickCount64()+1000;
}
void flush_ride_trace() noexcept {
    auto& r=runtime();
    if(!r.ride_trace_pending||GetTickCount64()<r.ride_trace_after)return;
    // Never serialize a previous ride while another BMX interaction is active.
    // Missing diagnostic histories are preferable to adding a gameplay hitch.
    if(r.active.load()||r.chassis_started||r.pending||r.stepping_off||r.bike_carried)return;
    const auto fall=bail::status();
    if((!fall.intent_status_available||fall.intent_reset_pending)&&
       GetTickCount64()<r.ride_trace_after+4000)return;
    r.ride_trace_pending=false;
    try {
        const auto chassis_diagnostics=native_chassis::status();
        auto samples=Json::array();
        r.pending_ride_history.each([&](const ride_trace::Sample& row){
            const auto& s=row.step;const auto& raw=row.input;const auto& p=row.producer;
            samples.push_back(Json{{"serial",s.serial},{"native_time",s.native_simulation_seconds},
                {"native_step_seconds",s.native_step_seconds},{"com",s.observed_com},{"quaternion",s.observed_quaternion},
                {"velocity",s.velocity},{"angular_velocity",s.angular_velocity},{"force",s.force},{"torque",s.torque},
                {"hop_impulse",s.hop_impulse},{"balance_torque",s.balance_torque},{"air_control_torque",s.air_control_torque},
                {"air_damping_torque",s.air_damping_torque},{"air_rotation_reserved",s.air_rotation_reserved},
                {"neutral_air_damping_active",s.neutral_air_damping_active},
                {"pump_force",s.pump_force},{"pump_applied",s.pump.applied},
                {"pump_status",ground_pump::name(s.pump.status)},{"pump_acceleration",s.pump.down_acceleration},
                {"pump_automatic_armed",s.pump.automatic_armed},{"pump_direct_seconds",s.pump.direct_seconds},
                {"pump_automatic_remaining",s.pump.automatic_remaining},{"pump_air_seconds",s.pump.air_seconds},
                {"motor_torque",s.motor_torque},{"contact_torque",s.contact_torque},
                {"raw_left_stick",std::array<double,2>{raw.left_x,raw.left_y}},
                {"raw_right_stick",std::array<double,2>{raw.right_x,raw.right_y}},
                {"raw_pedal",raw.pedal},{"raw_brake",raw.brake},{"raw_quick_hop",raw.quick_hop},
                {"input_stage","after_rehearsal_override_and_mount_impact_gates"},
                {"input_source",static_cast<unsigned>(row.input_source)},{"input_neutralized",row.input_neutralized},
                {"private_switch_gate",row.private_switch_gate},
                {"sampled_left_stick_before_overrides",std::array<double,2>{row.sampled_input.left_x,row.sampled_input.left_y}},
                {"sampled_right_stick_before_overrides",std::array<double,2>{row.sampled_input.right_x,row.sampled_input.right_y}},
                {"sampled_pedal_before_overrides",row.sampled_input.pedal},{"sampled_brake_before_overrides",row.sampled_input.brake},
                {"raw_triggers",std::array<double,2>{raw.left_trigger,raw.right_trigger}},
                {"raw_shoulders",std::array<bool,2>{raw.left_shoulder,raw.right_shoulder}},
                {"input_available",raw.available},{"input_focused",raw.focused},{"input_menu",raw.menu_open},{"input_device",raw.device},
                {"steer_command",s.actions.steer},{"effective_front_steer",s.frame.steer},
                {"requested_front_steer",s.requested_front_steer_angle},{"turning_lean",s.turning_lean_angle},
                {"steering_limited",s.steering_limited},{"grounded",s.grounded},{"actual_support",s.actual_support},
                {"wheel_contacts_sampled",s.wheel_contacts_sampled},
                {"contact_hit",s.contact_hit},{"contact_points",s.contact_points},{"contact_normals",s.contact_normals},
                {"query_geometry_fresh",s.query_geometry_fresh},{"approach_selected",s.approach_selected},
                {"approach_microseconds",s.approach_microseconds},
            {"approach_normal_selected",s.approach_normal_selected},
            {"contact_compliance_selected",s.contact_compliance_selected},{"contact_compliance_revoked",s.contact_compliance_revoked},
            {"landing_contact_compliance_selected",s.landing_contact_compliance_selected},{"landing_contact_compliance_revoked",s.landing_contact_compliance_revoked},
            {"landing_contact_compliance",landing_contact_compliance_json(s.landing_compliance)},
            {"compliant_normal_mask",s.compliant_normal_mask},{"compliance_attempted_mask",s.compliance_attempted_mask},
            {"compliance_budget_fallback",s.compliance_budget_fallback},
            {"compliant_contacts",Json::array({contact_compliance_json((s.compliance_attempted_mask&1u)!=0,s.compliance[0],s.compliant_predicted_gap[0],s.compliant_deflection_satisfied[0]),
                contact_compliance_json((s.compliance_attempted_mask&2u)!=0,s.compliance[1],s.compliant_predicted_gap[1],s.compliant_deflection_satisfied[1])})},{"approach_normal_attempted",s.approach_normal_attempted},
            {"approach_normal_adopted",s.approach_normal_adopted},{"approach_normal_issue",s.approach_normal_issue},
            {"speculative_normal_mask",s.speculative_normal_mask},{"speculative_issues",s.speculative_issues},
            {"speculative_normal_impulse",s.speculative_normal_impulse},{"speculative_normal_force",s.speculative_normal_force},
            {"speculative_linear_impulse",s.speculative_linear_impulse},{"speculative_angular_impulse",s.speculative_angular_impulse},
                {"queried_surfaces",Json::array({queried_surface_json(s.queried_surfaces[0]),queried_surface_json(s.queried_surfaces[1])})},
                {"approach_witnesses",Json::array({approach_geometry_json(s.approach[0]),approach_geometry_json(s.approach[1])})},
                {"pre_contact_prediction",precontact_prediction_json(s.pre_contact_prediction_available,s.pre_contact_prediction)},
                {"contact_balance_normals",s.contact_balance_normals},{"contact_balance_normal_available",s.contact_balance_normal_available},
                {"balance_reference_up",s.balance_reference_up},{"desired_balance_up",s.desired_balance_up},
                {"balance_support_mask",s.balance_support_mask},{"balance_continuation_mask",s.balance_continuation_mask},
                {"wheel_separation",s.wheel_separation},{"normal_load",s.normal_load},{"contact_features",s.contact_features},
                {"wheel_opening_speed_mps",s.wheel_opening_speed},{"wheel_recovery_velocity_mps",s.wheel_recovery_velocity},
                {"landing_compression",s.frame.landing_compression},
                {"air_extension_blend",s.frame.air_extension_blend},{"air_extension_available",s.frame.air_extension_available},
                {"rider_posture_available",s.frame.rider_posture_available},
                {"rider_uphill_blend",s.frame.rider_uphill_blend},{"rider_gaze_counterpitch_rad",s.frame.rider_gaze_counterpitch},
                {"contact_force_scale",s.contact_force_scale},{"actual_wheel_support",s.actual_wheel_support},
                {"bail",s.bail.bail},{"bail_reason",static_cast<unsigned>(s.bail.reason)},
                {"bail_alignment",s.bail.alignment},{"bail_closing_speed",s.bail.closing_speed},
                {"residual_available",s.impact_sample.residual_available},{"residual_impulse_ns",s.impact_sample.residual_impulse_ns},
                {"command_submitted",s.command_submitted},{"applications",s.native_applications},
                {"producer_applications",p.applications},{"producer_serial",p.last_serial},
                {"producer_active",p.active},{"producer_last_force",p.last_force},{"producer_last_torque",p.last_torque},
                {"rider_root",row.rider_position},{"rider_offboard",row.offboard},{"rider_standable",row.standable},
                {"rider_motion_supported",row.motion_supported}});
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
            // The accepted step captured this report on the client thread.
            // Serialize only during the existing deferred trace flush.
            if(s.shadow.enabled) {
                const auto& shadow=s.shadow;
                samples.at(samples.size()-1)["standalone_movement_shadow"]=Json{
                    {"selected",true},{"evaluated",shadow.evaluated},{"valid",shadow.valid},{"within_caps",shadow.within_caps},
                    {"serial",shadow.serial},{"body_token",shadow.token},{"issue",static_cast<unsigned>(shadow.issue)},
                    {"discontinuity",shadow.issue==standalone_shadow::Issue::discontinuity},
                    {"active",shadow.active},{"grounded",shadow.grounded},{"diagnostic_clamped",shadow.diagnostic_clamped},
                    {"force",shadow.force},{"torque",shadow.torque},{"hop_impulse",shadow.hop_impulse},
                    {"combined_force_norm",shadow.combined_force_norm},{"torque_norm",shadow.torque_norm},{"hop_norm",shadow.hop_norm},
                    {"steering_angle",shadow.steering_angle},{"comparison_available",shadow.comparison_available},
                    {"force_difference_norm",shadow.force_difference_norm},{"torque_difference_norm",shadow.torque_difference_norm},
                    {"hop_difference_norm",shadow.hop_difference_norm},{"shadow_selection",shadow.shadow_selection},
                    {"current_selection",shadow.current_selection},{"selection_matches",shadow.selection_matches},{"active_matches",shadow.active_matches},
                    {"evaluated_total",shadow.evaluated_total},{"accepted_total",shadow.accepted_total},
                    {"duplicate_total",shadow.duplicate_total},{"rejected_total",shadow.rejected_total},{"reset_total",shadow.reset_total}};
            }
#endif
        });
        const Json report{{"schema",1},{"process_id",GetCurrentProcessId()},{"rider_entity",r.ride_trace_entity},
            {"body_token",r.pending_ride_history.token()},{"termination_reason",r.ride_trace_reason},
            {"dropped_histories",r.ride_trace_dropped},{"fall_generation_at_flush",fall.intent_generation},
            {"fall_status_available",fall.intent_status_available},{"fall_reset_pending",fall.intent_reset_pending},
            {"fall_request_observed",r.ride_trace_has_fall_request},
            {"fall_request_accepted",r.ride_trace_fall_accepted},
            {"fall_request_status_fresh",r.ride_trace_fall_status_fresh},
            {"fall_request_ready",r.ride_trace_fall_status.ready},
            {"fall_request_status_owner_matches",r.ride_trace_fall_status.entity==r.ride_trace_entity},
            {"fall_request_id",r.ride_trace_fall_status.request_id},
            {"fall_request_generation",r.ride_trace_fall_status.intent_generation},
            {"fall_request_status_available",r.ride_trace_fall_status.intent_status_available},
            {"fall_request_protected_owner",r.ride_trace_fall_status.protected_owner},
            {"fall_request_native_flags",r.ride_trace_fall_status.native_offboard_flags},
            {"fall_request_issue",r.ride_trace_fall_status.issue},
            {"snapshot_reads_at_flush",chassis_diagnostics.snapshot_reads},
            {"snapshot_retries_at_flush",chassis_diagnostics.snapshot_retries},
            {"snapshot_recoveries_at_flush",chassis_diagnostics.snapshot_recoveries},
            {"snapshot_retry_exhaustions_at_flush",chassis_diagnostics.snapshot_retry_exhaustions},
            {"last_snapshot_issue_at_flush",static_cast<unsigned>(chassis_diagnostics.last_snapshot_issue)},
            {"native_velocity_rejections_at_flush",velocity_rejections_json(chassis_diagnostics.velocity_rejections)},
            {"input_source_key","0=adapter sample; 1=flat rehearsal; 2=ramp rehearsal; 3=catalog rehearsal; 4=trick rehearsal; 5=private switch neutral"},
            {"samples",std::move(samples)}};
        std::filesystem::create_directories(r.pending_ride_trace_root);
        const auto path=r.pending_ride_trace_root/("ride-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(++r.ride_trace_files)+".json");
        std::ofstream file(path,std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);
        file<<report.dump(2)<<'\n';file.close();
        logging::log(logging::Level::info,logging::Channel::skater,"BMX ride trace: {} samples; body token={}; report={}",
            r.pending_ride_history.size(),r.pending_ride_history.token(),path.string());
    } catch(...) {
        logging::write(logging::Level::warning,logging::Channel::skater,"BMX ride trace could not be written.");
    }
}
fall_lifecycle::Observation fall_observation(std::uintptr_t base,const NativeFrame& frame,const native_chassis::Frame& body,
    bool details=true,bool handoff_details=false) {
    auto& r=runtime();fall_lifecycle::Observation observation;
    observation.rider_identity={frame.available,{frame.client,frame.entity,frame.core,frame.context,frame.rig}};
    observation.body_identity={body.available,{body.world,body.native_state,body.body_index,body.body_generation,body.token}};
    const auto native=details?observe_native_rider(base,frame,false,handoff_details):NativeRiderObservation{};
    std::uint64_t serial{};double time{};
    observation.clock.available=frame.available&&read_native_clock(frame.entity,frame.core,serial,time);
    observation.clock.value={serial,time,frame.seconds};
    observation.coherent_epoch=observation.clock.available&&frame.timing_available&&serial==frame.serial&&time==frame.simulation_time;
    observation.rider.available=frame.available;
    auto& rider=observation.rider.value;
    rider.root=frame.position;rider.offboard=frame.offboard;rider.standable=frame.standable;rider.motion_supported=frame.motion_supported;
    rider.handoff_requested=handoff_details;
    if(native.available) {
        rider.physics_state={true,native.physics_state};rider.offboard_flags={true,native.offboard_flags};
        rider.impact_flags={true,native.impact_flags};rider.animation_request_mask={true,native.animation_request_mask};
        rider.state_kind={native.state_available,native.state_kind};
        rider.active_offset={native.state_available,static_cast<std::uint32_t>(native.active_offset)};
        rider.pending_offset={native.pending_offset_available,static_cast<std::uint32_t>(native.pending_offset)};
        rider.active_vtable_rva={native.state_available,native.active_vtable_rva};
        rider.transition_lock={native.transition_lock_available,native.transition_lock};
        rider.ragdoll_byte={native.ragdoll_available,native.ragdoll_byte};
        rider.ragdoll_mode={native.state_available&&native.state_kind!=0,native.state_kind>=4};
        rider.motion_velocity={native.velocity_available,native.motion_velocity};
        rider.protection_active={true,native.protected_owner};
        rider.context_830={native.context_830_available,native.context_830};
        rider.context_7c0={native.context_7c0_available,native.context_7c0};
        rider.active_160={native.active_160_available,native.active_160};
        rider.rig_4720={native.rig_4720_available,native.rig_4720};
        rider.rig_4760={native.rig_4760_available,native.rig_4760};
        rider.context_830_words={native.context_830_raw_available,native.context_830_words};
        rider.context_7c0_words={native.context_7c0_raw_available,native.context_7c0_words};
        rider.active_160_words={native.active_160_raw_available,native.active_160_words};
        rider.context_830_xyz={native.context_830_xyz_available,{native.context_830[0],native.context_830[1],native.context_830[2]}};
        rider.context_7c0_xyz={native.context_7c0_xyz_available,{native.context_7c0[0],native.context_7c0[1],native.context_7c0[2]}};
        rider.active_160_xyz={native.active_160_xyz_available,{native.active_160[0],native.active_160[1],native.active_160[2]}};
        rider.rig_4720_words={native.rig_4720_raw_available,native.rig_4720_words};
        rider.rig_4760_words={native.rig_4760_raw_available,native.rig_4760_words};
        rider.rig_4720_translation={native.rig_4720_translation_available,{native.rig_4720[12],native.rig_4720[13],native.rig_4720[14]}};
        rider.rig_4760_translation={native.rig_4760_translation_available,{native.rig_4760[12],native.rig_4760[13],native.rig_4760[14]}};
        rider.physical_rig={native.physical_rig_available,native.physical_rig};
        rider.physical_rig_parts={native.physical_rig_available,native.physical_rig_parts};
        for(unsigned i=0;i<4;++i) {
            const auto& part=native.physical_rig_bodies[i];
            rider.physical_body_positions[i]={part.available,part.position};
            rider.physical_body_position_words[i]={part.position_raw_available,part.position_words};
            rider.physical_body_position_xyz[i]={part.position_xyz_available,{part.position[0],part.position[1],part.position[2]}};
            rider.physical_body_flags[i]={part.flags_available,part.flags};
        }
    }
    if(r.observed_input.available) {
        rider.input_buttons={true,r.observed_input.buttons};rider.input_device={true,r.observed_input.device};
        rider.input_focused={true,r.observed_input.focused};
    }
    const auto semantic=details?intent_observer::getup_observation_status():intent_observer::GetupObservationStatus{};
    if(r.fall_getup_generation&&semantic.available&&!semantic.owner_changed&&semantic.generation==r.fall_getup_generation&&
       semantic.entity==frame.entity&&semantic.serial==frame.serial&&semantic.simulation_time==frame.simulation_time&&
       semantic.observed_ms&&GetTickCount64()-semantic.observed_ms<250) {
        rider.getup_generation={true,semantic.generation};
        rider.getup_allowed={semantic.is_getup_allowed.asset_verified&&semantic.is_getup_allowed.getter_available,semantic.is_getup_allowed.value};
        rider.getup_intent={semantic.getup.asset_verified&&semantic.getup.getter_available,semantic.getup.value};
        rider.getup_allowed_binding_present={semantic.is_getup_allowed.binding_present_known,semantic.is_getup_allowed.binding_present};
        rider.getup_intent_binding_present={semantic.getup.binding_present_known,semantic.getup.binding_present};
    }
    const bool body_epoch=body.available&&observation.coherent_epoch&&body.serial==serial&&body.simulation_time==time&&body.step_seconds==frame.seconds;
    observation.body={body_epoch,{body.position,body.linear_velocity,body.angular_velocity,body.orientation}};
    if(body.available&&!body_epoch)observation.snapshot_issue=fall_lifecycle::SnapshotIssue::epoch_mismatch;
    const auto chassis_status=native_chassis::status();
    observation.counters={true,{native_motion_updates(),rider::updates(),rider::render_updates(),chassis_status.applications}};
    observation.capture.bmx_capture={true,overlay::detail::bmx_input_capture_active()};
    observation.capture.interaction_capture={true,overlay::detail::bmx_interaction_capture_active()};
    const auto motion_lease=native_motion_lease_status();
    observation.capture.rider_motion_lease={motion_lease.available,motion_lease.active};
    observation.capture.drive_active={body_epoch,body.active};
    observation.capture.board_switch_pending={true,r.board_switch.active()};
    return observation;
}
void queue_fall_lifecycle(fall_lifecycle::Reason reason) {
    auto& r=runtime();
    (void)r.fall_history.finish(reason);
    if(r.fall_history.status().phase!=fall_lifecycle::Phase::frozen||r.fall_root.empty())return;
    const auto sequence=++r.fall_sequence;
    if(!r.pending_fall_reports.try_emplace([&](PendingFallReport& report) noexcept {
        report.history=r.fall_history;report.root.swap(r.fall_root);
        report.getup_generation=r.fall_getup_generation;report.sequence=sequence;
        report.write_after=GetTickCount64()+1000;report.write_attempts=0;
    })) {
        ++r.fall_dropped;++r.fall_queue_dropped;r.fall_last_dropped_sequence=sequence;
    }
    r.fall_history.reset();r.fall_root.clear();r.fall_getup_generation=0;
}
void begin_fall_lifecycle(std::uintptr_t base,const NativeFrame& frame,bool accepted,
                          const bail::Status& request,bool fresh_request) {
    auto& r=runtime();
    if(r.ride_trace_root.empty()||!r.bike_crashed)return;
    if(r.fall_history.status().phase!=fall_lifecycle::Phase::idle)
        queue_fall_lifecycle(fall_lifecycle::Reason::body_released);
    if(r.fall_files+r.pending_fall_reports.size()>=64)return;
    r.fall_root=r.ride_trace_root;
    intent_observer::cancel_getup_observation();r.fall_getup_generation=0;
    const auto observation=fall_observation(base,frame,r.crash_body,true,true);
    const fall_lifecycle::Identity identity{
        {frame.client,frame.entity,frame.core,frame.context,frame.rig},
        {r.crash_body.world,r.crash_body.native_state,r.crash_body.body_index,r.crash_body.body_generation,r.crash_body.token}};
    const fall_lifecycle::RequestReceipt receipt{true,accepted,fresh_request,request.intent_status_available,
        request.activation_enabled,request.entity,request.request_id,request.intent_generation};
    r.fall_started=GetTickCount64();
    if(r.fall_history.begin(identity,receipt,observation)) {
        if(accepted&&fresh_request) {
            const auto canonical=read_native_frame(base,frame.client);
            if(canonical.available&&fall_lifecycle::RiderIdentity{canonical.client,canonical.entity,canonical.core,canonical.context,canonical.rig}==identity.rider&&
               intent_observer::begin_getup_observation(base,canonical))
                r.fall_getup_generation=intent_observer::getup_observation_status().generation;
        }
    } else queue_fall_lifecycle(fall_lifecycle::Reason::caller_cancelled);
}
void sample_fall_lifecycle(std::uintptr_t base,const NativeFrame& frame,const native_chassis::Frame& body) {
    auto& r=runtime();if(r.fall_history.status().phase!=fall_lifecycle::Phase::recording)return;
    if(GetTickCount64()-r.fall_started>45000) {
        queue_fall_lifecycle(fall_lifecycle::Reason::wall_timeout);return;
    }
    const auto& identity=r.fall_history.status().identity;
    const bool owner_changed=frame.available&&fall_lifecycle::RiderIdentity{frame.client,frame.entity,frame.core,frame.context,frame.rig}!=identity.rider;
    const bool body_changed=body.available&&fall_lifecycle::BodyIdentity{body.world,body.native_state,body.body_index,body.body_generation,body.token}!=identity.body;
    const bool due=r.fall_history.sample_due({frame.serial,frame.simulation_time,frame.seconds});
    const auto& fall_status=r.fall_history.status();
    const double since_fall=frame.simulation_time-fall_status.initial_clock.time;
    const bool early_edge=frame.available&&frame.timing_available&&frame.seconds>0&&
        frame.serial>fall_status.last_clock.serial&&since_fall>=0&&since_fall<=.25;
    if(!r.fall_getup_generation&&!owner_changed&&frame.available&&
       r.fall_history.status().receipt_binding==fall_lifecycle::ReceiptBinding::accepted_request&&
       frame.simulation_time-r.fall_history.status().initial_clock.time<1&&intent_observer::begin_getup_observation(base,frame))
        r.fall_getup_generation=intent_observer::getup_observation_status().generation;
    // Every native edge reaches the fixed recorder. Only optional detailed
    // state/semantic reads use its cadence; short readiness changes and paused
    // serial edges still pass through the clock/fact checks below.
    const bool readiness_change=frame.available&&frame.standable&&!r.fall_history.status().standable_after_nonstandable&&
        r.fall_history.status().nonstandable_observed;
    auto observation=fall_observation(base,frame,body,due||early_edge||readiness_change||owner_changed||body_changed,early_edge);
    observation.record_edge=early_edge;
    (void)r.fall_history.observe(observation);
    if(r.fall_history.status().phase==fall_lifecycle::Phase::frozen)
        queue_fall_lifecycle(fall_lifecycle::Reason::caller_cancelled);
}
template<class T> Json fall_value(const fall_lifecycle::Measured<T>& value) {
    return value.available?Json(value.value):Json();
}
Json fall_sample_json(const fall_lifecycle::Sample& sample) {
    Json result{{"serial",sample.clock.serial},{"native_time",sample.clock.time},{"step_seconds",sample.clock.seconds},
        {"elapsed",sample.elapsed},{"snapshot_issue",static_cast<unsigned>(sample.snapshot_issue)}};
    if(sample.rider.available) {
        const auto& state=sample.rider.value;
        result["rider"]=Json{{"root",state.root},{"offboard",state.offboard},{"standable",state.standable},
            {"motion_supported",state.motion_supported},{"physics_state",fall_value(state.physics_state)},
            {"offboard_flags",fall_value(state.offboard_flags)},{"impact_flags",fall_value(state.impact_flags)},
            {"animation_request_mask",fall_value(state.animation_request_mask)},{"state_kind",fall_value(state.state_kind)},
            {"active_offset",fall_value(state.active_offset)},{"pending_offset",fall_value(state.pending_offset)},
            {"active_vtable_rva",fall_value(state.active_vtable_rva)},{"transition_lock",fall_value(state.transition_lock)},
            {"ragdoll_byte",fall_value(state.ragdoll_byte)},{"known_ragdoll_substate",fall_value(state.ragdoll_mode)},
            {"native_cached_velocity",fall_value(state.motion_velocity)},{"native_protection",fall_value(state.protection_active)},
            {"getup_allowed_value",fall_value(state.getup_allowed)},{"getup_intent_value",fall_value(state.getup_intent)},
            {"getup_allowed_binding_present",fall_value(state.getup_allowed_binding_present)},
            {"getup_intent_binding_present",fall_value(state.getup_intent_binding_present)},
            {"getup_observer_generation",fall_value(state.getup_generation)},
            {"observed_pad_buttons",fall_value(state.input_buttons)},{"observed_input_device",fall_value(state.input_device)},
            {"observed_input_focused",fall_value(state.input_focused)}};
        auto raw_bodies=Json::array();constexpr std::array<unsigned,4> indices{0,1,12,23};
        for(unsigned i=0;i<4;++i)raw_bodies.push_back(Json{{"record_index",indices[i]},
            {"translation_50",fall_value(state.physical_body_positions[i])},
            {"translation_50_words",fall_value(state.physical_body_position_words[i])},
            {"translation_50_xyz",fall_value(state.physical_body_position_xyz[i])},
            {"flags_60",fall_value(state.physical_body_flags[i])}});
        result["rider"]["handoff"]=Json{{"requested",state.handoff_requested},
            {"context_830",fall_value(state.context_830)},{"context_7c0",fall_value(state.context_7c0)},
            {"active_160",fall_value(state.active_160)},{"rig_4720",fall_value(state.rig_4720)},
            {"rig_4760",fall_value(state.rig_4760)},
            {"context_830_words",fall_value(state.context_830_words)},{"context_7c0_words",fall_value(state.context_7c0_words)},
            {"active_160_words",fall_value(state.active_160_words)},
            {"context_830_xyz",fall_value(state.context_830_xyz)},{"context_7c0_xyz",fall_value(state.context_7c0_xyz)},
            {"active_160_xyz",fall_value(state.active_160_xyz)},
            {"rig_4720_words",fall_value(state.rig_4720_words)},{"rig_4760_words",fall_value(state.rig_4760_words)},
            {"rig_4720_translation",fall_value(state.rig_4720_translation)},{"rig_4760_translation",fall_value(state.rig_4760_translation)},
            {"physical_rig",fall_value(state.physical_rig)},
            {"physical_rig_parts",fall_value(state.physical_rig_parts)},{"physical_body_records",std::move(raw_bodies)},
            {"boundary","Read-only native handoff caches, captured only at release and first0.25 native seconds. Raw words require a successful guarded read; finite XYZ/translation is qualified separately from the fourth/padding lanes. Context830 upstream producer and body-record roles remain unknown. Context9c0 bit0x4 selects7c0 rather than830 in the verified FollowRagdoll update; no contact meaning is assigned. No native synchronization routine is invoked."}};
    } else result["rider"]=nullptr;
    if(sample.body.available) {
        const auto& body=sample.body.value;
        result["body"]=Json{{"com",body.com},{"velocity",body.linear_velocity},{"angular_velocity",body.angular_velocity},{"quaternion",body.orientation}};
    } else result["body"]=nullptr;
    if(sample.counters.available) {
        const auto& counters=sample.counters.value;
        result["completed_counters"]=Json{{"velocity_updates",counters.velocity_updates},{"rider_pose_updates",counters.rider_poses},
            {"rider_render_updates",counters.render_poses},{"force_applications",counters.force_applications}};
    } else result["completed_counters"]=nullptr;
    result["capture_leases"]=Json{{"bmx",fall_value(sample.capture.bmx_capture)},
        {"interaction",fall_value(sample.capture.interaction_capture)},{"rider_motion",fall_value(sample.capture.rider_motion_lease)},
        {"bike_drive",fall_value(sample.capture.drive_active)},
        {"board_switch_pending",fall_value(sample.capture.board_switch_pending)}};
    return result;
}
void flush_fall_lifecycle(std::uintptr_t base,std::uintptr_t client) noexcept {
    auto& r=runtime();
    auto* pending=r.pending_fall_reports.front();
    if(!pending||GetTickCount64()<pending->write_after||r.active.load()||r.pending||r.chassis_started||
       r.stepping_off||r.bike_carried||r.board_switch.active()||r.fall_history.status().phase==fall_lifecycle::Phase::recording)return;
    // One explicit development trace write, never in an active BMX ride or
    // during an ordinary native fall/get-up. Private impact diagnostics may
    // serialize their completed passive window while the rider remains down.
    if(!impact_test::variant()) {
        const auto current=read_native_frame(base,client);
        std::uint32_t physics{};
        const bool board_grounded=current.available&&!current.offboard&&memory::peek(current.context+0x1414,physics)&&physics==100;
        if(!current.available||(!current.standable&&!board_grounded))return;
    }
    const auto fall=bail::status();if(fall.intent_reset_pending)return;
    bool committed{};
    try {
        const auto& status=pending->history.status();const auto& identity=status.identity;const auto& receipt=status.receipt;
        auto rows=Json::array();pending->history.each([&](const auto& sample){rows.push_back(fall_sample_json(sample));});
        Json report{{"event","bmx_passive_fall_lifecycle"},{"process_id",GetCurrentProcessId()},
            {"reason",fall_lifecycle::describe(status.reason)},{"reason_id",static_cast<unsigned>(status.reason)},
            {"native_elapsed",status.native_elapsed},{"window_complete",status.reason==fall_lifecycle::Reason::window_elapsed},
            {"identity",Json{{"client",identity.rider.client},{"entity",identity.rider.entity},{"core",identity.rider.core},
                {"context",identity.rider.context},{"rig",identity.rider.rig},{"world",identity.body.world},
                {"native_state",identity.body.native_state},{"body_index",identity.body.index},{"body_generation",identity.body.generation},{"body_token",identity.body.token}}},
            {"request",Json{{"attempted",receipt.attempted},{"accepted",receipt.accepted},{"status_fresh",receipt.status_fresh},
                {"status_available",receipt.status_available},{"typed",receipt.typed},{"entity",receipt.entity},
                {"id",receipt.request_id},{"generation",receipt.generation},{"binding",static_cast<unsigned>(status.receipt_binding)}}},
            {"standable_after_nonstandable_observed",status.standable_after_nonstandable},
            {"standable_after_ragdoll_observed",status.standable_after_ragdoll},{"known_ragdoll_observed",status.ragdoll_observed},
            {"any_completed_counter_advance",status.counter_advance_observed},{"counter_regression",status.counter_regression_observed},
            {"snapshot_failures",status.snapshot_failures},{"written_samples",status.written_samples},{"overwritten_samples",status.overwritten_samples},
            {"dropped_histories_at_flush",r.fall_dropped},
            {"report_sequence",pending->sequence},{"canonical_getup_generation",pending->getup_generation},
            {"pending_queue",Json{{"capacity",r.pending_fall_reports.capacity()},{"depth_at_flush",r.pending_fall_reports.size()},
                {"write_attempts_before_success",pending->write_attempts},{"capacity_drops",r.fall_queue_dropped},
                {"write_failure_drops",r.fall_write_dropped},{"last_dropped_sequence",r.fall_last_dropped_sequence}}},
            {"initial",fall_sample_json(pending->history.initial())},{"samples",std::move(rows)},
            {"boundary","Passive sampled state; null means unavailable. Capture fields are BMX lease status, not proof of platform input delivery. Pad bits are observed input, not identified native get-up actions. Getter false can be an asset default when binding presence is unknown. Counter changes may include already-admitted callbacks. No native get-up input, state write, automatic recovery or visual acceptance is supplied."}};
        const auto semantic_trace=intent_observer::getup_observation_trace();
        if(pending->getup_generation&&semantic_trace.available&&semantic_trace.status.available&&
           semantic_trace.status.generation==pending->getup_generation&&
           semantic_trace.status.entity==identity.rider.entity) {
            auto semantic_rows=Json::array();
            unsigned outside_window{};
            const auto property_json=[](const intent_observer::GetupBoolObservation& property) {
                return Json{{"guid",property.guid},{"asset_verified",property.asset_verified},
                    {"getter_available",property.getter_available},{"value",property.getter_available?Json(property.value):Json()},
                    {"binding_present",property.binding_present_known?Json(property.binding_present):Json()}};
            };
            for(unsigned i=0;i<semantic_trace.count&&i<semantic_trace.samples.size();++i) {
                const auto& row=semantic_trace.samples[i];
                if(row.generation!=pending->getup_generation||row.entity!=identity.rider.entity||
                   row.entity_handle!=semantic_trace.status.entity_handle||!std::isfinite(row.simulation_time)||
                   row.serial<status.initial_clock.serial||row.serial>status.last_clock.serial||
                   row.simulation_time<status.initial_clock.time||row.simulation_time>status.last_clock.time) {
                    ++outside_window;continue;
                }
                semantic_rows.push_back(Json{{"generation",row.generation},{"entity",row.entity},{"entity_handle",row.entity_handle},
                    {"serial",row.serial},{"native_time",row.simulation_time},{"observed_ms",row.observed_ms},
                    {"getup",property_json(row.getup)},{"is_getup_allowed",property_json(row.is_getup_allowed)}});
            }
            const auto& state=semantic_trace.status;
            report["canonical_getup_observation"]=Json{{"generation",state.generation},{"entity",state.entity},
                {"entity_handle",state.entity_handle},{"completed",state.completed},{"owner_changed",state.owner_changed},
                {"issue",state.issue},{"failures",state.failures},{"contention_drops",state.contention_drops},
                {"getter_calls",state.getter_calls},{"asset_lookup_calls",state.asset_lookup_calls},{"samples",std::move(semantic_rows)},
                {"status_snapshot_phase","flush"},{"rows_outside_sealed_window",outside_window},
                {"boundary","Only the exact stored observer generation/entity and callback rows within this sealed fall's native serial/time bounds are attached. Observer status is copied at flush, independently from the immutable sampled body/rider rows. A getter value may be an asset default. Unknown binding presence is null; no input mapping or setter use is inferred."}};
        } else report["canonical_getup_observation"]=nullptr;
        std::filesystem::create_directories(pending->root);
        const auto path=pending->root/("fall-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(pending->sequence)+".json");
        const RetainedBikeIdentity report_identity{{identity.rider.client,identity.rider.entity,identity.rider.core,
            identity.rider.context,identity.rider.rig},identity.body.world,identity.body.native_state,
            identity.body.index,identity.body.generation,identity.body.token};
        const bool lab_matches=r.getup_lab.requested&&r.getup_lab.fall_request_id==receipt.request_id&&
            r.getup_lab.fall_generation==receipt.generation&&r.getup_lab.identity==report_identity;
        std::filesystem::path lab_root;std::string lab_filename;
        if(lab_matches) {lab_root=pending->root;lab_filename=path.filename().string();}
        const auto path_text=path.string();const auto sample_count=pending->history.size();
        const auto native_elapsed=status.native_elapsed;const auto* reason=fall_lifecycle::describe(status.reason);
        auto temporary=path;temporary+=L".tmp";
        std::ofstream file(temporary,std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);file<<report.dump(2)<<'\n';file.close();
        std::filesystem::rename(temporary,path);
        if(lab_matches) {
            r.lab_fall_history=pending->history;
            r.getup_lab.root.swap(lab_root);r.getup_lab.trace_filename.swap(lab_filename);
            r.getup_lab.trace_flushed=true;
        }
        ++r.fall_files;(void)r.pending_fall_reports.pop();committed=true;
        logging::log(logging::Level::info,logging::Channel::skater,"BMX passive fall lifecycle: {} samples; {} native seconds; {}; report={}",
            sample_count,native_elapsed,reason,path_text);
    }catch(...) {
        if(committed)return;
        pending->write_after=GetTickCount64()+2000;
        const bool dropped=++pending->write_attempts>=3;
        if(dropped) {
            ++r.fall_dropped;++r.fall_write_dropped;r.fall_last_dropped_sequence=pending->sequence;
            (void)r.pending_fall_reports.pop();
        }
        logging::write(logging::Level::warning,logging::Channel::skater,
            !dropped?"BMX passive fall report write failed; retaining the same bounded FIFO head for retry.":
            "BMX passive fall report dropped after three write failures.");
    }
}
Json retained_identity_json(const RetainedBikeIdentity& identity) {
    return Json{{"client",identity.owner.client},{"entity",identity.owner.entity},{"core",identity.owner.core},
        {"context",identity.owner.context},{"rig",identity.owner.rig},{"world",identity.world},
        {"native_state",identity.native_state},{"body_index",identity.body_index},
        {"body_generation",identity.body_generation},{"body_token",identity.token}};
}
bool getup_status_matches(const GetupLab& lab,const intent_observer::GetupActionStatus& action) {
    return action.available&&action.armed&&action.generation&&
        EquipmentOwner{action.client,action.entity,action.core,action.context,action.rig}==lab.identity.owner&&
        action.request.fall_request_id==lab.fall_request_id&&action.request.fall_generation==lab.fall_generation&&
        action.request.world==lab.identity.world&&action.request.native_state==lab.identity.native_state&&
        action.request.body_index==lab.identity.body_index&&action.request.body_generation==lab.identity.body_generation&&
        action.request.body_token==lab.identity.token&&action.requested_serial==lab.requested_serial&&
        action.requested_native_time==lab.requested_time;
}
Json getup_callback_audit_json(const GetupLab& lab,const intent_observer::GetupActionStatus& action) {
    // This report runs beneath the native client update stack. Keep the fixed
    // audit bank off that stack, including the copy API's destination buffer.
    auto storage=std::make_unique<intent_observer::GetupCallbackAuditTrace>();
    if(!intent_observer::copy_getup_callback_audit_trace(*storage))return Json{{"started",lab.audit_started},{"bound",false},{"samples",nullptr}};
    const auto& trace=*storage;const auto& state=trace.status;
    const auto& identity=lab.identity;const auto& owner=identity.owner;const auto& request=state.request;
    const bool action_handle_matches=lab.accepted&&getup_status_matches(lab,action)&&state.entity_handle&&
        state.entity_handle==action.entity_handle;
    const bool bound=lab.audit_started&&trace.available&&state.available&&state.armed&&
        (!lab.accepted||action_handle_matches)&&
        EquipmentOwner{state.client,state.entity,state.core,state.context,state.rig}==owner&&
        state.requested_serial==lab.requested_serial&&state.requested_native_time==lab.requested_time&&
        request.fall_request_id==lab.fall_request_id&&request.fall_generation==lab.fall_generation&&
        request.world==identity.world&&request.native_state==identity.native_state&&
        request.body_index==identity.body_index&&request.body_generation==identity.body_generation&&request.body_token==identity.token;
    if(!bound)return Json{{"started",lab.audit_started},{"bound",false},
        {"action_handle_matches",lab.accepted?Json(action_handle_matches):Json()},{"samples",nullptr}};
    const auto property_json=[](const intent_observer::GetupBoolObservation& property) {
        return Json{{"guid",property.guid},{"asset_verified",property.asset_verified},
            {"getter_available",property.getter_available},{"value",property.getter_available?Json(property.value):Json()},
            {"binding_present",property.binding_present_known?Json(property.binding_present):Json()}};
    };
    auto rows=Json::array();
    for(unsigned i=0;i<trace.count&&i<trace.samples.size();++i) {
        const auto& row=trace.samples[i];const auto& dispatch=row.dispatch;
        Json dispatch_json;
        if(dispatch.available)dispatch_json=Json{{"observer_admitted",dispatch.observer_admitted},{"called",dispatch.called},
            {"serial",dispatch.clock_available?Json(dispatch.serial):Json()},
            {"native_time",dispatch.clock_available?Json(dispatch.native_time):Json()},
            {"serial_already_reserved",dispatch.serial_already_reserved},{"serial_reserved_this_callback",dispatch.serial_reserved_this_callback},
            {"true_entry_this_callback",dispatch.true_entry_this_callback},{"native_set_called",dispatch.native_set_called},
            {"generation",dispatch.generation},{"preflight_before",dispatch.preflight_before},{"preflight_after",dispatch.preflight_after},
            {"true_calls_before",dispatch.true_calls_before},{"true_calls_after",dispatch.true_calls_after},
            {"phase_before",dispatch.phase_before},{"phase_after",dispatch.phase_after},{"issue",static_cast<unsigned>(dispatch.issue)}};
        rows.push_back(Json{{"ordinal",row.ordinal},{"serial",row.clock_available?Json(row.serial):Json()},
            {"native_time",row.clock_available?Json(row.native_time):Json()},{"observed_ms",row.observed_ms},
            {"entity_handle",row.entity_handle},
            {"owner_verified",row.owner_verified},{"event_params",row.event_params},{"node",row.node},{"root",row.root},
            {"data",row.data},{"binding",row.binding},{"context_wrapper",row.context_wrapper},
            {"data_guid",row.data_guid_available?Json(row.data_guid):Json()},
            {"input_guid",row.input_guid_available?Json(row.input_guid):Json()},
            {"mirror_guid",row.mirror_guid_available?Json(row.mirror_guid):Json()},
            {"native_update_after_before_sdk_action",row.update_after},{"getup",property_json(row.getup)},
            {"is_getup_allowed",property_json(row.is_getup_allowed)},{"dispatch",std::move(dispatch_json)},
            {"issue",static_cast<unsigned>(row.issue)}});
    }
    return Json{{"started",true},{"bound",true},{"generation",state.generation},{"entity_handle",state.entity_handle},
        {"action_handle_matches",lab.accepted?Json(action_handle_matches):Json()},
        {"requested_serial",state.requested_serial},{"native_time",state.requested_native_time},
        {"started_ms",state.started_ms},{"deadline_ms",state.deadline_ms},{"active",state.active},
        {"completed",state.completed},{"cancelled",state.cancelled},{"owner_changed",state.owner_changed},
        {"callbacks",state.callbacks},{"candidates",state.candidates},{"getter_calls",state.getter_calls},
        {"asset_lookup_calls",state.asset_lookup_calls},{"overflow_drops",state.overflow_drops},
        {"contention_drops",state.contention_drops},{"rejected",state.rejected},{"count",trace.count},{"samples",std::move(rows)},
        {"boundary","Getter-only candidate observations after the unchanged GetUp action observer. Native Update bytes were captured before that SDK observer; getters are measured afterward. Dispatch retains its separately measured clock when available; it is not assumed equal to the later getter row clock. Unknown slot presence stays null. Later callback reads cannot establish the value before action dispatch. Capacity, rejection and contention counts limit coverage. Audit values never authorize an action or change the pulse reservation."}};
}
void poll_getup_lab_report() noexcept {
    auto& r=runtime();auto& lab=r.getup_lab;
    if(!lab.requested||lab.report_written)return;
    try {
        const auto action=intent_observer::getup_action_status();const bool fresh=getup_status_matches(lab,action);
        if(lab.accepted&&fresh&&!lab.logged) {
            lab.logged=true;
            logging::log(logging::Level::info,logging::Channel::skater,
                "BMX GetUp action requested: request={}; fall request={}; fall generation={}; serial={}; native time={}.",
                action.generation,lab.fall_request_id,lab.fall_generation,lab.requested_serial,lab.requested_time);
        }
        if(!lab.trace_flushed||lab.write_attempts>=3||GetTickCount64()<lab.write_after)return;
        const bool pulse_passed=lab.accepted&&fresh&&action.allowed_before_action&&
            action.allowed_serial==action.true_serial&&action.prior_false&&action.true_returned&&action.true_readback&&
            action.intent_written&&action.intent_reset&&action.reset_readback&&!action.reset_pending&&
            !action.owner_changed&&!action.owner_expired&&action.true_calls==1&&action.true_serial>0&&
            action.reset_serial>action.true_serial&&
            (action.reset_calls==1||(action.reset_calls==0&&action.native_cleared));
        double standing_start{},standing_end{};bool standing_suffix{};
        r.lab_fall_history.each([&](const fall_lifecycle::Sample& sample) {
            if(fresh&&action.reset_serial&&sample.clock.serial>action.reset_serial&&
               sample.snapshot_issue==fall_lifecycle::SnapshotIssue::none&&sample.rider.available&&sample.body.available&&
               sample.rider.value.offboard&&sample.rider.value.standable) {
                if(!standing_suffix)standing_start=sample.clock.time;
                standing_suffix=true;standing_end=sample.clock.time;
            } else {standing_suffix=false;standing_start=standing_end=0;}
        });
        const bool standing=standing_suffix&&standing_end-standing_start>=2;
        const Json pulse{{"generation",action.generation},{"prior_false",action.prior_false},
            {"true_returned",action.true_returned},{"true_readback",action.true_readback},
            {"intent_written",action.intent_written},{"intent_reset",action.intent_reset},
            {"reset_readback",action.reset_readback},{"reset_pending",action.reset_pending},
            {"owner_changed",action.owner_changed},{"owner_expired",action.owner_expired},
            {"native_cleared",action.native_cleared},{"true_serial",action.true_serial},{"reset_serial",action.reset_serial},
            {"true_calls",action.true_calls},{"reset_calls",action.reset_calls},
            {"completed",action.completed},{"expired",action.expired},{"cancelled",action.cancelled},
            {"binding_present",action.binding_present_known?Json(action.binding_present):Json()},
            {"issue",action.issue}};
        const Json report{{"event","bmx_getup_action_test"},{"process_id",GetCurrentProcessId()},{"completed",true},
            {"lab_only",true},{"accepted_request",lab.accepted},{"status_fresh",fresh},{"status_available",action.available},
            {"request_id",action.generation},{"fall_request_id",lab.fall_request_id},{"fall_generation",lab.fall_generation},
            {"entity_handle",action.entity_handle},{"identity",retained_identity_json(lab.identity)},
            {"requested_serial",lab.requested_serial},{"native_time",lab.requested_time},
            {"down_before_action",lab.down_before_action},{"allowed_before_action",action.allowed_before_action},
            {"allowed_serial",action.allowed_serial},{"allowed_native_time",action.allowed_native_time},
            {"pulse_passed",pulse_passed},{"standability_observed",standing},{"pulse",pulse},
            {"preflight",lab.preflight},{"baseline",lab.baseline},{"fall_trace",lab.trace_filename},
            {"callback_audit",getup_callback_audit_json(lab,action)},
            {"standing_suffix_seconds",standing_suffix?standing_end-standing_start:0},
            {"boundary","Explicit private diagnostic intent only. The authored GetUp output and proven Bool setter are used; full script-level ForceDisable/time/block/clearance predicates and physical-button mapping are not established. No other native guard, position, physics state or collision field is written. A previously absent binding slot may be created and is not claimed topologically restored. Native standability is observed separately from pulse delivery; no visual acceptance."}};
        const auto path=lab.root.parent_path()/L"bmx-getup-action-test.json";
        auto temporary=path;temporary+=L".tmp";
        std::ofstream file(temporary,std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);
        file<<report.dump(2)<<'\n';file.close();std::filesystem::rename(temporary,path);lab.report_written=true;
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX GetUp lab complete: pulse={}; native standing suffix={}s; report={}",
            pulse_passed,standing_suffix?standing_end-standing_start:0,path.string());
    }catch(...) {
        ++lab.write_attempts;lab.write_after=GetTickCount64()+2000;
        logging::write(logging::Level::warning,logging::Channel::skater,"BMX private GetUp report unavailable; bounded diagnostic retry only.");
    }
}
void message(std::string value, bool notice = false) {
    auto& r = runtime();
    bool changed{};
    {
        std::lock_guard lock(r.status_mutex);
        changed = r.message != value;
        r.message = value;
    }
    if (changed && notice) {
        overlay::notify_latest(overlay::NoticeSlot::bmx_status, overlay::NoticeLevel::info, "BMX", value);
        logging::write(logging::Level::info, logging::Channel::skater, value);
    } else if (changed && !value.starts_with("BMX mounted:")) {
        logging::write(logging::Level::info, logging::Channel::skater, value);
    }
}
void release_control() {
    overlay::detail::cancel_human_native_admission("bike_controls_released");
    release_native_motion();
    DingoSDKOverlaySetBmxInputCapture(false);
    auto& r = runtime();
    if (r.chassis_started) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto owner = read_native_frame(base, r.client);
        if (owner.available) chassis_ride::pause(base, owner);
        else native_chassis::forget();
    }
    r.observed_capture.store(false);
    (void)update_bmx_protection(r.client, r.entity, false);
}
void withdraw_owner_recreation();
void withdraw_unavailable_bike_observation() {
    runtime().mount_dismount.interrupt();
    overlay::detail::cancel_human_native_admission("bike_observation_withdrawn");
    if(native_chassis::status().owner_quarantined){withdraw_owner_recreation();return;}
    auto& r=runtime();
    native_chassis::suspend();release_native_motion();r.crash_interaction.withdraw();r.setdown_search.cancel();r.carry_obstacle_refused=true;
    r.observation_input_rearm=true;
    (void)owner_observation::reset_interactions(r.request,r.interaction_button,r.toggle_key,r.interact_key,r.carry_key,&r.quick_switch_button);
    DingoSDKOverlaySetBmxQuickSwitchCapture(false);
    DingoSDKOverlaySetBmxInputCapture(false);DingoSDKOverlaySetBmxInteractionCapture(false);
    r.observed_capture.store(false);(void)update_bmx_protection(r.client,r.entity,false);
    cancel_bmx_dismount();cancel_bmx_board_mount();r.board_switch.cancel();r.step_off_board.cancel();
    r.stepping_off=r.carry_after_step=false;r.mapper.reset();r.clock.reset();r.interrupted=true;
    r.world_frame={};visual::publish({});
    message("Bike observation unavailable; BMX controls and presentation released.");
}
void finish_rehearsal(bool completed, std::string_view reason = {}) {
    auto& r = runtime();
    if (!r.rehearsal) return;
    r.rehearsal = false;
    const auto updates = native_motion_updates() - r.initial_updates;
    const auto draws = visual::draws() - r.initial_draws;
    const auto poses = rider::updates() - r.initial_poses;
    const auto world_model = world::snapshot();
    const auto placements = world_model.entity_placements - r.initial_world_placements;
    const auto contacts = rider::metrics();
    const auto total_render_poses = rider::render_updates();
    const auto render_poses = total_render_poses >= r.initial_render_poses ? total_render_poses - r.initial_render_poses : 0;
    const auto now_ms = GetTickCount64();
    const auto render = selftest::assess_render({r.rehearsal_seconds, r.initial_render_poses,
        total_render_poses, contacts.render_count, r.entity, contacts.render_entity,
        now_ms, contacts.render_observed_ms, contacts.hand_error, contacts.foot_error, contacts.render_ready});
    const auto& tally = r.chassis_step.tricks.tally;
    const bool catalog_passed = !r.catalog_rehearsal ||
        (tally.landed_movements >= 1 &&
         tally.landed_by_id[static_cast<unsigned>(catalog::TrickId::superman)] >= 1 &&
         r.catalog_peak_render_poses >= 3);
    const bool tricks_passed = !r.trick_rehearsal ||
        (r.catalog_rehearsal ? catalog_passed :
        (tally.landed_barspins >= 1 && tally.landed_tailwhips >= 1 && tally.landed_combos >= 1 &&
         r.trick_bar_catches >= 1 && r.trick_tail_catches >= 1 && r.trick_bar_release > .9 &&
         r.trick_tail_release > .9 && r.trick_pitch_command > .1 && r.trick_pitch_torque > 1 &&
         r.trick_pitch_response_serial == r.trick_pitch_serial+1 && r.trick_pitch_response > .01));
    const bool chassis_common = r.chassis_step.ready && r.chassis_step.native_applications > 60 &&
         r.chassis_samples.size() > 120 && r.chassis_tracking_error < .75 && r.chassis_distance > 1;
    const bool ramp_passed = r.ramp_rehearsal && r.ramp_driver.tally().passed &&
        r.chassis_step.grounded && r.chassis_step.landing_body_alignment >= .5 &&
        std::hypot(r.chassis_step.velocity[0],r.chassis_step.velocity[2]) < .75 && r.chassis_step.hop_events == 0;
    const bool physical_chassis_passed = !r.native_chassis_mode || (r.ramp_rehearsal ?
        chassis_common && ramp_passed :
        (r.chassis_step.ready && r.chassis_step.native_applications > 60 && r.chassis_samples.size() > 120 &&
         r.chassis_tracking_error < .75 && r.chassis_distance > 1 && r.chassis_step.hop_events >= 1 &&
         r.chassis_hop_rise > .1 && r.chassis_takeoff_speed > 1 && r.chassis_before_measured &&
         r.chassis_after_measured && r.chassis_final_measured && r.chassis_before_brake > .5 &&
         r.chassis_after_brake < r.chassis_before_brake * .7 && r.chassis_final_speed < .75));
    const bool required_turn_passed = r.native_chassis_mode ? r.observed_bike_turn.passed() :
        r.max_turn > .01 && r.right_turn.passed();
    const bool flat_measurements = r.before_brake_measured && r.after_brake_measured && r.final_speed_measured &&
        r.max_distance > 1 && r.max_height > .1 && required_turn_passed &&
        r.before_brake_speed > .5 && r.after_brake_speed < r.before_brake_speed * .7 && r.final_speed < .75;
    const bool passed = completed && physical_chassis_passed && tricks_passed &&
        (r.ramp_rehearsal ? ramp_passed : flat_measurements) &&
        updates > 10 && world_model.ready && world_model.live_parts == world::part_count && placements > 60 &&
        poses > 0 && render.passed();
    // Keep the report in the isolated root that authorized this request, even if
    // the process environment changes before completion or cancellation.
    const auto result = r.rehearsal_root / L"bmx-selftest.json";
    std::ofstream file(result);
    const auto physical_status = native_chassis::status();
    const Json report{{"process_id",GetCurrentProcessId()}, {"completed", completed}, {"passed", passed},
        {"failure_reason", passed ? std::string{} : reason.empty() ? "Native riding measurements did not meet the rehearsal thresholds." : std::string(reason)},
        {"input", "synthetic BMX controls in the native game; physical controller not tested"},
        {"unattended", r.rehearsal_mode == selftest::Mode::unattended},
        {"background_input_used", r.background_input_used}, {"unfocused_ticks", r.unfocused_ticks},
        {"background_native_seconds", r.background_seconds}, {"native_simulation_seconds", r.rehearsal_seconds},
        {"wall_elapsed_ms", now_ms - r.rehearsal_started},
        {"wall_limit_ms", selftest::wall_limit_ms}, {"wall_clock_expired", r.rehearsal_expired},
        {"max_native_distance_m", r.max_distance}, {"max_native_air_height_m", r.max_height},
        {"max_native_turn_radians", r.max_turn},
        {"native_right_turn_radians", r.right_turn.measured ? Json(r.right_turn.delta) : Json{}},
        {"native_right_turn_passed", r.right_turn.passed()},
        {"observed_bike_heading_required",r.native_chassis_mode&&!r.ramp_rehearsal},
        {"observed_bike_max_turn_radians",r.observed_bike_turn.initialized?Json(r.observed_bike_turn.maximum_turn):Json{}},
        {"observed_bike_right_turn_radians",r.observed_bike_turn.right_turn.measured?Json(r.observed_bike_turn.right_turn.delta):Json{}},
        {"observed_bike_right_turn_window_qualified",r.observed_bike_turn.window_qualified&&r.observed_bike_turn.right_turn.measured},
        {"observed_bike_right_turn_passed",r.observed_bike_turn.passed()},
        {"observed_bike_heading_samples",r.observed_bike_turn.accepted},
        {"observed_bike_heading_rejections",r.observed_bike_turn.rejected},
        {"native_speed_before_brake_mps", r.before_brake_measured ? Json(r.before_brake_speed) : Json{}},
        {"native_speed_after_brake_mps", r.after_brake_measured ? Json(r.after_brake_speed) : Json{}},
        {"native_final_speed_mps", r.final_speed_measured ? Json(r.final_speed) : Json{}},
        {"native_velocity_updates", updates}, {"bike_draw_frames", draws}, {"rider_pose_updates", poses},
        {"native_bike_live_parts", world_model.live_parts}, {"native_bike_placements", placements},
        {"native_bike_ready", world_model.ready}, {"rider_render_pose_updates", render_poses},
        {"rider_render_pose_minimum", render.minimum_poses}, {"rider_render_pose_density_passed", render.density},
        {"rider_render_pose_ready", contacts.render_ready}, {"rider_render_pose_fresh", render.fresh},
        {"rider_render_pose_owner_matches", render.current_owner}, {"rider_render_pose_count_coherent", render.coherent_count},
        {"rider_render_pose_contacts_passed", render.contacts},
        {"rider_render_pose_observed_ms", contacts.render_ready ? Json(contacts.render_observed_ms) : Json{}},
        {"rider_render_pose_age_ms", contacts.render_ready && now_ms >= contacts.render_observed_ms ? Json(now_ms - contacts.render_observed_ms) : Json{}},
        {"rider_hand_error_m", contacts.render_ready ? Json(contacts.hand_error) : Json{}},
        {"rider_foot_error_m", contacts.render_ready ? Json(contacts.foot_error) : Json{}},
        {"rider_posture_requested", contacts.render_ready ? Json(contacts.posture_requested) : Json{}},
        {"rider_posture_attempts", contacts.render_ready ? Json(contacts.posture_attempts) : Json{}},
        {"rider_posture_scale", contacts.render_ready ? Json(contacts.posture_scale) : Json{}},
        {"rider_posture_fit_adjustment_m", contacts.render_ready ? Json(contacts.posture_fit_adjustment) : Json{}},
        {"rider_posture_gaze_applied_rad", contacts.render_ready ? Json(contacts.posture_gaze) : Json{}},
        {"rider_posture_fallback", contacts.render_ready ? Json(contacts.posture_fallback) : Json{}},
        {"bike_presentation", "eight native world entities; overlay bike drawing disabled"},
        {"physical_chassis_mode",r.native_chassis_mode}, {"physical_chassis_passed",physical_chassis_passed},
        {"ramp_rehearsal",r.ramp_rehearsal}, {"ramp_passed",ramp_passed},
        {"ramp_ascent",r.ramp_driver.tally().ascent}, {"ramp_flight",r.ramp_driver.tally().flight},
        {"ramp_landed",r.ramp_driver.tally().landed}, {"ramp_issue",r.ramp_driver.tally().issue},
        {"ramp_max_com_rise_m",r.ramp_driver.tally().max_rise},
        {"ramp_flight_seconds",r.ramp_driver.tally().flight_seconds},
        {"ramp_maximum_ballistic_seconds",r.ramp_driver.tally().maximum_ballistic_seconds},
        {"ramp_maximum_natural_flight_seconds",r.ramp_driver.tally().maximum_natural_flight_seconds},
        {"ramp_passive_angular_intervals",r.ramp_driver.tally().passive_angular_intervals},
        {"ramp_flight_contract","Gravity-only COM translation; zero total torque or separately qualified bounded passive neutral angular damping. Pure zero-total-torque ballistic counters remain separate."},
        {"ramp_ballistic_velocity_tolerance_mps",ramp_rehearsal::ballistic_velocity_tolerance},
        {"ramp_landing_support_seconds",r.ramp_driver.tally().landing_support_seconds},
        {"ramp_origin",r.ramp_route.origin}, {"ramp_uphill_direction",r.ramp_route.forward},
        {"ramp_fixture_object_id",r.ramp_route.object_id},
        {"chassis_native_force_applications",physical_status.applications},
        {"chassis_min_contact_scale",r.chassis_min_contact_scale},
        {"chassis_bridge_average_ms",r.chassis_bridge_calls ? r.chassis_bridge_ms/r.chassis_bridge_calls : 0},
        {"chassis_bridge_peak_ms",r.chassis_bridge_peak_ms},
        {"chassis_world_average_ms",r.chassis_world_calls ? r.chassis_world_ms/r.chassis_world_calls : 0},
        {"chassis_world_peak_ms",r.chassis_world_peak_ms},
        {"chassis_backend_issue",physical_status.issue}, {"chassis_backend_rejections",physical_status.rejected},
        {"native_velocity_rejections",velocity_rejections_json(physical_status.velocity_rejections)},
        {"mount_seed_attempts",r.mount_seed_attempts}, {"mount_seed_last",r.mount_seed_last},
        {"chassis_producer_calls",physical_status.producer_calls}, {"chassis_wrapper_matches",physical_status.wrapper_matches},
        {"chassis_max_rider_tracking_error_m",r.chassis_tracking_error},
        {"tricks_required",r.trick_rehearsal}, {"tricks_passed",tricks_passed},
        {"catalog_required",r.catalog_rehearsal},
        {"catalog_id",static_cast<unsigned>(r.catalog_rehearsal ? catalog::TrickId::superman : catalog::TrickId::none)},
        {"catalog_passed",catalog_passed}, {"catalog_peak_render_poses",r.catalog_peak_render_poses},
        {"catalog_max_render_phase",r.catalog_max_render_phase},
        {"catalog_observed_requested_render_writes",r.catalog_observed_requested_writes},
        {"catalog_observed_applied_render_writes",r.catalog_observed_applied_writes},
        {"catalog_observed_invalid_applied_writes",r.catalog_observed_invalid_writes},
        {"catalog_observed_min_applied_strength",r.catalog_observed_applied_writes?Json(r.catalog_observed_min_strength):Json(nullptr)},
        {"catalog_observed_max_applied_strength",r.catalog_observed_applied_writes?Json(r.catalog_observed_max_strength):Json(nullptr)},
        {"catalog_observed_scope","distinct sampled completed native Superman render writes; not count deltas, final GPU consumption or visual quality"},
        {"landed_barspins",tally.landed_barspins}, {"landed_tailwhips",tally.landed_tailwhips},
        {"landed_combos",tally.landed_combos}, {"failed_combos",tally.failed_combos}, {"canceled_combos",tally.canceled_combos},
        {"landed_movements",tally.landed_movements}, {"landed_by_id",tally.landed_by_id},
        {"landed_front_flips",tally.landed_front_flips}, {"landed_back_flips",tally.landed_back_flips},
        {"landed_barrel_rolls",tally.landed_barrel_rolls},
        {"bar_catch_events",r.trick_bar_catches}, {"tail_catch_events",r.trick_tail_catches},
        {"max_bar_release",r.trick_bar_release}, {"max_tail_release",r.trick_tail_release},
        {"air_pitch_control","left_stick_y"},{"max_air_pitch_command",r.trick_pitch_command},
        {"max_air_pitch_axis_torque_nm",r.trick_pitch_torque}, {"max_air_pitch_axis_rate_rad_s",r.trick_pitch_rate},
        {"air_pitch_first_serial",r.trick_pitch_serial}, {"air_pitch_response_serial",r.trick_pitch_response_serial},
        {"air_pitch_before_rate_rad_s",r.trick_pitch_before_rate}, {"air_pitch_response_delta_rad_s",r.trick_pitch_response},
        {"chassis_com_horizontal_distance_m",r.chassis_distance},
        {"chassis_com_speed_before_brake_mps",r.chassis_before_brake},
        {"chassis_com_speed_after_brake_mps",r.chassis_after_brake},
        {"chassis_com_final_speed_mps",r.chassis_final_speed},
        {"chassis_com_hop_rise_m",r.chassis_hop_rise}, {"chassis_com_takeoff_speed_mps",r.chassis_takeoff_speed},
        {"chassis_samples",Json(r.chassis_samples)},
        {"chassis_model_limit","Single rigid chassis with approximate capsule wheels, zero-camber tire constraints and balance assistance; static surface velocity assumed."}};
    file << report.dump(2) << '\n';
    logging::log(logging::Level::info, logging::Channel::skater,
        "BMX native rehearsal {}: moved {}m, air height {}m, right turn {}rad, velocity {}, native bike placements {}, rider poses {}, render poses {} (minimum {}, fresh {}, owner {}). Report: {}",
        passed ? "passed" : "did not pass", r.max_distance, r.max_height, r.right_turn.delta, updates, placements, poses,
        render_poses, render.minimum_poses, render.fresh, render.current_owner, result.string());
}
void end_ride(std::string reason) {
    auto& r = runtime();
    overlay::detail::cancel_human_native_admission("bike_lifecycle_ended");
    overlay::detail::cancel_human_native_return("bike_lifecycle_ended");
    r.human_native_return_pending=r.human_native_return_mount=false;r.human_native_return_until=0;
    intent_observer::cancel_getup_action();
    intent_observer::cancel_getup_callback_audit();
    queue_fall_lifecycle(fall_lifecycle::Reason::body_released);
    freeze_ride_trace(reason);
    finish_rehearsal(false, reason);
    if (r.chassis_started) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto owner = read_native_frame(base, r.client);
        if (owner.available) chassis_ride::stop(base, owner);
        else native_chassis::forget();
        r.chassis_started = false;
    }
    if(r.bike_crashed) {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto owner=read_native_frame(base,r.client);
        (void)native_chassis::park(base,owner,true,true);
    }
    r.active.store(false);
    r.bike_crashed=r.crash_interaction_ready=false;r.crash_interaction.reset();r.crash_body={};r.crash_identity={};
    r.bike_parked=r.bike_carried=r.mount_from_parked=false;r.parked_bike={};r.parked_owner={};
    r.setdown_search.cancel();r.setdown_mount_after=r.setdown_from_obstacle=r.carry_obstacle_refused=false;r.setdown_until=0;
    r.parked_ground_verified=false;
    r.parked_world=r.parked_context=0;r.parked_token=0;r.parked_identity={};
    r.stepping_off=r.carry_after_step=false;
    r.mount_entry={};r.mount_entry_token=0;r.mount_entry_began=r.mount_entry_completed=false;
    DingoSDKOverlaySetBmxInteractionCapture(false);
    r.chassis_pause_requested = r.chassis_pause_active = false;
    r.pending = false;
    r.mount_owner.reset();r.mount_dismount.reset();r.mount_search.cancel();
    r.dismount_requested = false;
    r.dismount_retry_after = 0;
    r.board_switch.cancel();
    r.step_off_board.cancel();
    cancel_bmx_dismount();
    cancel_bmx_board_mount();
    release_control();
    r.mapper.reset(); r.clock.reset();
    r.velocity = {};
    r.barspin = r.tailwhip = r.bar_time = r.whip_time = 0;
    r.preload = r.front_weight = r.rear_weight = 0;
    r.world_frame = {};
    visual::publish({});
    rider::finish_private_grip_probe();
    if(native_chassis::status().observation_suspended)withdraw_unavailable_bike_observation();
    message(std::move(reason), true);
}

native_chassis::ReleasedKey runtime_retained_key() {
    const auto& r=runtime();const auto& id=r.bike_crashed?r.crash_identity:r.parked_identity;
    return {{id.owner.client,id.owner.entity,id.owner.core,id.owner.context,id.owner.rig},
        {id.world,id.native_state,r.parked_context},id.body_index,id.body_generation,id.token};
}
native_chassis::ReleaseWitness runtime_release_witness() {
    const auto& r=runtime();
    const owner_observation::RuntimeRelease state{r.bike_parked,r.bike_crashed,r.active.load(),r.pending,
        r.chassis_started,r.bike_carried,r.stepping_off,r.step_off_board.active(),r.board_switch.active(),
        r.rehearsal,r.mount_from_parked};
    return {runtime_retained_key(),owner_observation::runtime_released(state),r.bike_parked,r.bike_crashed};
}
void withdraw_owner_recreation() {
    auto& r=runtime();release_native_motion();r.crash_interaction.withdraw();r.setdown_search.cancel();r.carry_obstacle_refused=true;
    // service_released_owner/quarantine_recreation already revoked the chassis
    // producer under its publication lock. Do not pause/park/forget this pool.
    r.observation_input_rearm=true;
    (void)owner_observation::reset_interactions(r.request,r.interaction_button,r.toggle_key,r.interact_key,r.carry_key,&r.quick_switch_button);
    DingoSDKOverlaySetBmxQuickSwitchCapture(false);
    DingoSDKOverlaySetBmxInputCapture(false);DingoSDKOverlaySetBmxInteractionCapture(false);
    r.observed_capture.store(false);(void)update_bmx_protection(r.client,r.entity,false);
    cancel_bmx_dismount();cancel_bmx_board_mount();
    intent_observer::cancel_getup_action();intent_observer::cancel_getup_callback_audit();
    r.mapper.reset();r.clock.reset();r.interrupted=true;r.crash_interaction_ready=false;
    r.world_frame={};visual::publish({});
}
// true means this whole tick/action must unwind: no local exchanged request,
// copied RawInput, mount proposal or cached active WorldTick publication survives.
bool service_retained_owner(std::uintptr_t base,std::uintptr_t client,const NativeFrame* hint=nullptr) {
    auto& r=runtime();
    const bool retained=(r.bike_parked||r.bike_crashed)&&!r.chassis_started&&!r.bike_carried;
    bool gameplay=false;
    if(retained&&r.owner_gameplay_ready){
        const auto ui=multiplayer::read_game_ui_state(base);
        gameplay=owner_observation::custody_gameplay(r.owner_gameplay_ready,ui.available,ui.in_menu);
    }
    if(!gameplay){
        const auto waiting=native_chassis::service_released_owner(base,client,runtime_release_witness(),false);
        if(waiting.result==native_chassis::ReleasedOwnerResult::idle)return false;
        r.owner_unwind.stop();withdraw_owner_recreation();return true;
    }
    if(r.bike_parked&&r.mount_from_parked&&!r.chassis_started&&!r.rehearsal&&!r.bike_carried&&
       !r.stepping_off&&!r.step_off_board.active()&&!r.board_switch.active()&&(r.pending||r.active.load())) {
        const auto current=hint?*hint:read_native_frame(base,client);
        const auto key=runtime_retained_key();
        const owner_observation::Owner owner{current.client,current.entity,current.core,current.context,current.rig};
        if(native_chassis::status().owner_quarantined||
           (current.available&&owner_observation::same_actor(key.owner,owner)&&
            owner.context!=key.owner.context&&owner.rig!=key.owner.rig)) {
            // Cancel only a not-yet-acquired retained remount. A focus-paused
            // acquired ride (chassis_started) never reaches this conversion.
            release_native_motion();r.active.store(false);r.pending=false;r.mount_from_parked=false;
            r.mount_owner.reset();r.mount_dismount.reset();r.mount_search.cancel();r.mount_entry={};r.mount_entry_token=0;
            r.mount_entry_began=r.mount_entry_completed=false;
            (void)owner_observation::reset_interactions(r.request,r.interaction_button,r.toggle_key,r.interact_key,r.carry_key,&r.quick_switch_button);
            DingoSDKOverlaySetBmxQuickSwitchCapture(false);
        }
    }
    const auto witness=runtime_release_witness();
    const auto receipt=native_chassis::service_released_owner(base,client,witness,true);
    if(receipt.result==native_chassis::ReleasedOwnerResult::idle)return false;
    r.owner_unwind.stop();
    if(receipt.result==native_chassis::ReleasedOwnerResult::invalidated){
        withdraw_owner_recreation();end_ride("Bike released because its actor, world or native custody changed.");return true;
    }
    if(receipt.result==native_chassis::ReleasedOwnerResult::committed) {
        if(!witness.available||witness.key!=receipt.before||receipt.after.token!=receipt.before.token+1||
           receipt.parked!=r.bike_parked||receipt.detached!=r.bike_crashed) {
            native_chassis::forget();withdraw_owner_recreation();
            end_ride("Bike released because its owner handoff receipt no longer matches.");return true;
        }
        const auto& after=receipt.after;
        const EquipmentOwner owner{after.owner.client,after.owner.entity,after.owner.core,after.owner.context,after.owner.rig};
        const RetainedBikeIdentity id{owner,after.world.world,after.world.state,after.body_index,after.body_generation,after.token};
        r.parked_owner=owner;r.parked_world=after.world.world;r.parked_context=after.world.context;r.parked_token=after.token;
        if(r.bike_crashed){
            r.crash_identity=id;r.crash_body={};r.crash_last_observed=GetTickCount64();
            // Recovery timing belongs to the new association, not an old rig's
            // clock epoch. Historical fall/GetUp records retain their old key.
            r.crash_native_time=receipt.rider.simulation_time;r.crash_serial=receipt.rider.serial;
            r.crash_interaction.adopt_recovered(id,receipt.rider.serial,receipt.rider.simulation_time);
        }
        else r.parked_identity=id;
        r.client=owner.client;r.entity=owner.entity;r.core=owner.core;r.context=owner.context;r.rig=owner.rig;
        const auto& measured=receipt.measured_body;
        std::array<float,3> bike_root{};
        if(measured.available&&chassis::root_from_com(measured.position,measured.orientation,
            collision_envelope::com_from_root,bike_root)){
            r.parked_bike.position=bike_root;r.parked_bike.chassis_quaternion=measured.orientation;
            r.parked_bike.chassis_quaternion_available=true;
        }
        sample_fall_lifecycle(base,receipt.rider,{}); // seals old full-owner history; never rebound

        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX released owner handoff: token {} -> {}; body {}/{}; actor {}/{}/{}; context/rig {}/{} -> {}/{}; no native setters or allocation; explicit retry required.",
            receipt.before.token,after.token,after.body_index,after.body_generation,owner.client,owner.entity,owner.core,
            receipt.before.owner.context,receipt.before.owner.rig,owner.context,owner.rig);
        withdraw_owner_recreation();
        message("Bike retained after rider recovery. Move beside it, center the controls, then tap Y/Triangle again.",true);return true;
    }
    withdraw_owner_recreation();
    message("Bike retained while the recovered rider is requalified; controls released.");return true;
}

// Only a geometrically exhausted or expired, not-yet-acquired retained mount
// reaches this path. Keep custody, not an admission intent or support witness.
// In particular the outer timeout runs before gameplay/UI qualification, so
// cancellation performs no native owner/handle reads and no chassis setters.
bool cancel_unacquired_retained_mount(std::string reason) {
    auto& r=runtime();const auto pool=native_chassis::status();
    const bool pool_parked=pool.ready&&pool.parked&&!pool.active&&!pool.detached&&!pool.failed&&!pool.blocked&&
        pool.world==r.parked_world;
    const RetainedMountCancellation state{r.parked_identity,r.parked_owner,r.parked_world,r.parked_context,r.parked_token,
        r.native_chassis_mode,r.bike_parked,r.mount_from_parked,r.active.load(),r.pending,r.bike_carried,r.bike_crashed,
        r.chassis_started,r.rehearsal,impact_test::variant()!=0,r.stepping_off,r.step_off_board.active(),r.board_switch.active(),pool_parked};
    if(!retained_mount_cancellation_allowed(state))return false;
    // chassis_started is false: release_control cannot pause/forget a body.
    release_control();native_chassis::revoke_released_witness();
    r.active.store(false);r.pending=false;r.pending_until=0;r.mount_from_parked=false;
    r.mount_owner.reset();r.mount_dismount.reset();r.mount_search.cancel();r.mount_entry={};r.mount_entry_token=0;
    r.mount_entry_began=r.mount_entry_completed=false;
    r.dismount_requested=false;r.dismount_retry_after=0;cancel_bmx_dismount();cancel_bmx_board_mount();
    r.request.store(0);r.interaction_button.reset();r.toggle_key={};r.interact_key={};r.carry_key={};
    // A claimed held shortcut remains quarantined through its release; never
    // reset quick_switch_button or its independent North-only capture here.
    DingoSDKOverlaySetBmxInteractionCapture(false);
    r.mapper.reset();r.clock.reset();r.interrupted=true;r.velocity={};
    r.world_frame={};visual::publish({});
    // The next ordinary idle tick rechecks gameplay, canonical owner and
    // parked body before publishing the retained placement. No stale cached
    // root or parked_ground_verified flag authorizes this failed mount.
    message(std::move(reason)+" Bike retained; center the controls, stand beside it and retry.",true);
    return true;
}
void stop_mount_admission(std::string reason) {
    if(!cancel_unacquired_retained_mount(reason))end_ride(std::move(reason));
}

void wait_for_mount(std::string reason) {
    auto& r = runtime();
    release_control();
    r.mapper.reset(); r.clock.reset(); r.interrupted = true; r.velocity = {};
    r.world_frame = {};
    visual::publish({});
    message(std::move(reason));
}
struct WheelProbe {
    Contacts contacts;
    float height{}, pitch{};
    bool ready{}, supported{};
    std::uintptr_t world{},context{};
};
WheelProbe wheels(const State& s, float root_height, std::uintptr_t entity,std::uintptr_t expected_world=0,std::uintptr_t expected_context=0) {
    WheelProbe result;
    const auto direction = std::array<float, 3>{static_cast<float>(std::sin(s.yaw)), 0, static_cast<float>(std::cos(s.yaw))};
    std::array<LocalSurfaceProbe, 2> probes;
    for (unsigned i = 0; i != 2; ++i) {
        const float along = (i ? geometry::rear_axle : geometry::front_axle)[2];
        const float x = static_cast<float>(s.x) + direction[0] * along;
        const float z = static_cast<float>(s.z) + direction[2] * along;
        const float y = static_cast<float>(s.y) - root_height;
        probes[i] = probe_local_surface({x, y + .45f, z}, {x, y - 2.0f, z}, {}, entity);
    }
    result.ready = probes[0].available && probes[1].available && probes[0].world && probes[0].context &&
        probes[0].world==probes[1].world && probes[0].context==probes[1].context;
    if(result.ready) {result.world=probes[0].world;result.context=probes[0].context;}
    if(expected_world)for(const auto& probe:probes)
        if(probe.world!=expected_world||probe.context!=expected_context)result.ready=false;
    if (!result.ready) return result;
    result.supported = probes[0].hit && probes[1].hit;
    auto convert = [&](const LocalSurfaceProbe& p) {
        return Support{p.hit ? QueryState::hit : QueryState::clear, p.hit ? p.position[1] + root_height : 0.0};
    };
    result.contacts = {convert(probes[0]), convert(probes[1])};
    if (result.supported) {
        const float delta = probes[0].position[1] - probes[1].position[1];
        // Do not climb a wall or bridge a sharp discontinuity with averaged wheel heights.
        if (std::abs(delta) > .65f) { result.ready = false; return result; }
        result.height = (probes[0].position[1] + probes[1].position[1]) * .5f;
        result.pitch = -std::atan2(delta, static_cast<float>(geometry::wheelbase));
    }
    return result;
}
bool fresh_mount_seed(std::uintptr_t base,const NativeFrame& frame,std::array<float,3> requested_root,
                      float yaw,mount_pose::Seed& seed,bool& owner_changed,bool& measured_rejection,
                      MountSearchSupport expected_support,MountSearchSupport& observed_support,bool retained_placement=false) {
    auto& r=runtime();owner_changed=measured_rejection=false;observed_support={};
    const bool retained=r.mount_from_parked||retained_placement;
    auto& diagnostic=r.mount_seed_last;
    diagnostic=Json{{"stage","begin"},{"attempt",++r.mount_seed_attempts},{"requested_root",requested_root},
        {"rider_position",frame.position},{"root_height",r.root_height},{"yaw",yaw},
        {"serial",frame.serial},{"simulation_time",frame.simulation_time},{"from_parked",r.mount_from_parked},{"for_setdown",retained_placement}};
    if(r.mount_search.active()) {
        diagnostic["search_index"]=r.mount_search.index();
        diagnostic["search_radius_limit"]=NearbyMountSearch::maximum_radius;
    }
    Json::Array surfaces;
    const auto fail=[&](const char* stage,bool rejected=false) {
        measured_rejection=rejected;diagnostic["stage"]=stage;diagnostic["surfaces"]=surfaces;
        if(r.mount_search.active())diagnostic["search_measured_rejection"]=rejected;
        const auto now=GetTickCount64();
        if(now>=r.mount_seed_next_log) {
            r.mount_seed_next_log=now+1000;
            logging::log(logging::Level::info,logging::Channel::skater,"BMX mount seed diagnostic: {}",diagnostic.dump());
        }
        return false;
    };
    if(retained&&!native_chassis::parked_owner_matches(base,frame,r.parked_world,r.parked_token)) {
        owner_changed=true;return fail("parked_owner");
    }
    std::uintptr_t context{};
    if(!memory::peek(frame.client+8,context)||!context)return fail("client_context_unavailable");
    diagnostic["client_context"]=context;observed_support.context=context;
    if(expected_support.context&&context!=expected_support.context) {owner_changed=true;return fail("search_context_changed");}
    if(retained&&context!=r.parked_context) {owner_changed=true;return fail("parked_context");}
    State at;at.x=requested_root[0];at.y=requested_root[1];at.z=requested_root[2];at.yaw=yaw;
    const auto floor=wheels(at,0,frame.entity,retained?r.parked_world:expected_support.world,context);
    observed_support.world=floor.world;
    if(expected_support.world&&floor.world&&floor.world!=expected_support.world) {owner_changed=true;return fail("search_world_changed");}
    diagnostic["floor"]=Json{{"ready",floor.ready},{"supported",floor.supported},{"height",floor.height},
        {"pitch",floor.pitch},{"world",floor.world},{"context",floor.context},
        {"height_delta",floor.height-requested_root[1]}};
    if(retained&&floor.world&&floor.world!=r.parked_world) {owner_changed=true;return fail("parked_world");}
    if(!floor.ready||!floor.supported||floor.context!=context||!std::isfinite(floor.pitch)||
       std::abs(floor.pitch)>mount_pose::maximum_slope||std::abs(floor.height-requested_root[1])>.35f)return fail("initial_floor_guard",floor.ready||floor.supported);
    requested_root[1]=floor.height;
    const mount_pose::Request request{requested_root,yaw,floor.world,context,frame.serial,frame.simulation_time};
    // Two heights choose only an initial query orientation. Actual measured
    // plane normals and final rotated-hub witnesses authorize the real seed.
    const float sy=std::sin(yaw*.5f),cy=std::cos(yaw*.5f),sp=std::sin(floor.pitch*.5f),cp=std::cos(floor.pitch*.5f);
    const chassis::Quaternion initial_rotation{cy*sp,sy*cp,-sy*sp,cy*cp};
    auto initial_root=requested_root;initial_root[1]+=geometry::parked_root_clearance(floor.pitch);
    diagnostic["query_root"]=initial_root;diagnostic["query_orientation"]=initial_rotation;
    unsigned query_calls=0;
    const auto query=[&](std::array<float,3> from,std::array<float,3> to) -> wheel_surface::RaySample {
        const auto hit=probe_local_surface(from,to,{},frame.entity);
        ++query_calls;
        if(!hit.available||hit.world!=request.world||hit.context!=request.context) {
            if(hit.available)owner_changed=true;
            diagnostic["query_failure"]=Json{{"from",from},{"to",to},{"available",hit.available},
                {"hit",hit.hit},{"world",hit.world},{"context",hit.context}};
            return {};
        }
        return {true,hit.hit,hit.position,hit.fraction};
    };
    bool surface_rejected{};
    const auto sample=[&](const char* phase,std::array<float,3> root,chassis::Quaternion orientation,
                          std::array<mount_pose::Sample,2>& output) {
        surface_rejected=false;
        const auto up=chassis::rotate_unit(orientation,{0,1,0});
        const auto forward=chassis::rotate_unit(orientation,{0,0,1});
        const auto left=chassis::rotate_unit(orientation,{1,0,0});
        for(unsigned i=0;i<2;++i) {
            const auto local=chassis::rotate_unit(orientation,geometry::wheel_hub(i==0));
            std::array<float,3> hub;for(unsigned j=0;j<3;++j)hub[j]=root[j]+local[j];
            wheel_surface::Settings settings;settings.radius=static_cast<float>(geometry::wheel_radius);
            const auto surface=wheel_surface::search(hub,up,forward,left,query,settings);
            surfaces.emplace_back(Json{{"phase",phase},{"wheel",i},{"hub",hub},{"up",up},
                {"available",surface.available},{"hit",surface.hit},{"feature",int(surface.feature)},
                {"issue",int(surface.issue)},{"separation",surface.separation},{"point",surface.point},
                {"normal",surface.normal},{"rays",surface.rays}});
            diagnostic["query_calls"]=query_calls;
            if(!surface.available||!surface.hit||surface.feature!=wheel_surface::Feature::plane) {
                surface_rejected=surface.available;return false;
            }
            output[i]={true,true,true,surface.point,surface.normal,request.world,request.context,
                request.serial,request.simulation_time};
        }
        return true;
    };
    std::array<mount_pose::Sample,2> initial{},final{};
    if(!sample("initial",initial_root,initial_rotation,initial))return fail("initial_surface",surface_rejected);
    const auto candidate=mount_pose::prepare(request,initial);
    diagnostic["prepare"]=Json{{"prepared",candidate.prepared},{"issue",int(candidate.issue)},
        {"root",candidate.root},{"orientation",candidate.orientation},{"up",candidate.up},
        {"pitch",candidate.pitch},{"roll",candidate.roll},{"cross_plane_residual",candidate.cross_plane_residual},
        {"cross_plane_allowance",candidate.cross_plane_allowance}};
    if(!candidate.prepared)return fail("prepare",true);
    if(!sample("final",candidate.root,candidate.orientation,final))return fail("final_surface",surface_rejected);
    seed=mount_pose::verify_final(candidate,final);
    diagnostic["final_issue"]=int(seed.issue);if(!seed.ready)return fail("final_witness_guard",true);
    const auto fresh=read_native_frame(base,frame.client);std::uintptr_t final_context{};
    diagnostic["final_clock"]=Json{{"available",fresh.available},{"serial",fresh.serial},{"simulation_time",fresh.simulation_time}};
    if(!fresh.available||!memory::peek(frame.client+8,final_context)||!final_context)return fail("final_owner_unavailable");
    if(EquipmentOwner{fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig}!=
       EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}||final_context!=context) {
        owner_changed=true;return fail("final_owner_changed");
    }
    if(fresh.serial!=request.serial||fresh.simulation_time!=request.simulation_time)return fail("clock_changed");
    if(retained&&!native_chassis::parked_owner_matches(base,fresh,r.parked_world,r.parked_token)) {
        owner_changed=true;return fail("final_parked_owner");
    }
    diagnostic["stage"]="ready";diagnostic["surfaces"]=surfaces;
    return true;
}
bool place_bike(std::uintptr_t base,const NativeFrame& rider,visual::Frame& placed) {
    (void)base;auto& r=runtime();
    if(!rider.available)return false;
    std::uintptr_t context{};
    if(!memory::peek(rider.client+8,context)||!context)return false;
    const bool retained=r.bike_parked||r.bike_crashed;
    const auto expected_world=retained?r.parked_world:native_chassis::status().world;
    if(!expected_world||(retained&&context!=r.parked_context))return false;
    const float yaw=std::atan2(rider.transform[8],rider.transform[10]);
    const float side=-.65f;const std::array<float,3> target{rider.position[0]+side*std::cos(yaw),
        rider.position[1]-r.root_height,rider.position[2]-side*std::sin(yaw)};
    State placement;placement.x=target[0];placement.y=target[1];placement.z=target[2];placement.yaw=yaw;
    const auto floor=wheels(placement,0,rider.entity,expected_world,context);
    if(!floor.ready||!floor.supported||std::abs(floor.height-target[1])>.3f||std::abs(floor.pitch)>.18f)return false;
    const auto clear=probe_local_surface(rider.position,{target[0],floor.height+r.root_height,target[2]}, {},rider.entity);
    if(!clear.available||clear.world!=expected_world||clear.context!=context||clear.hit)return false;
    // Keep both tire bottoms at the sampled floor when setting down on a mild
    // slope. The supplied model's root is not exactly a wheel radius below its hubs.
    const float floor_clearance=geometry::parked_root_clearance(floor.pitch);
    placed={};placed.active=true;placed.entity=rider.entity;placed.position={target[0],floor.height+floor_clearance,target[2]};
    placed.yaw=yaw;placed.pitch=floor.pitch;placed.rider_mode=visual::RiderMode::none;
    return true;
}

void cancel_carried_setdown() {
    auto& r=runtime();r.setdown_search.cancel();r.setdown_mount_after=r.setdown_from_obstacle=false;r.setdown_until=0;r.carry_obstacle_refused=true;
}
bool begin_carried_setdown(const NativeFrame& frame,bool mount_after,bool obstacle) {
    auto& r=runtime();cancel_carried_setdown();
    if(!r.bike_parked||!r.bike_carried||r.stepping_off||!frame.available||!frame.timing_available||
       !frame.offboard||!frame.standable||EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}!=r.parked_owner)return false;
    const float yaw=std::atan2(frame.transform[8],frame.transform[10]),side=-.65f;
    const std::array<float,3> root{frame.position[0]+side*std::cos(yaw),frame.position[1]-r.root_height,
        frame.position[2]-side*std::sin(yaw)};
    if(!r.setdown_search.begin(r.parked_identity,r.parked_context,root,yaw))return false;
    r.setdown_mount_after=mount_after;r.setdown_from_obstacle=obstacle;r.carry_obstacle_refused=obstacle;
    r.setdown_until=GetTickCount64()+15000;return true;
}
void tick_carried_setdown(std::uintptr_t base,const NativeFrame& frame) {
    auto& r=runtime();if(!r.setdown_search.active())return;
    const auto stop=[&](const char* reason){cancel_carried_setdown();message(reason,true);};
    if(!r.bike_carried||!r.bike_parked||r.stepping_off||!frame.available||!frame.timing_available||
       !frame.offboard||!frame.standable||GetTickCount64()>=r.setdown_until){
        stop("Set-down canceled; bike kept in hand. Stand on clear ground and try again.");return;
    }
    std::uintptr_t context{};
    if(!memory::peek(frame.client+8,context)||!context){stop("Set-down observation unavailable; bike kept in hand.");return;}
    const RetainedBikeIdentity current{{frame.client,frame.entity,frame.core,frame.context,frame.rig},
        r.parked_identity.world,r.parked_identity.native_state,r.parked_identity.body_index,r.parked_identity.body_generation,r.parked_token};
    const auto decision=r.setdown_search.next(current,context,frame.serial,frame.simulation_time);
    if(decision==NearbyMountDecision::waiting)return; // no second query in this native epoch
    if(decision!=NearbyMountDecision::query){stop("No safe set-down point found; bike kept in hand. Move and try again.");return;}
    const auto reject=[&](){
        r.setdown_search.finish_attempt(true);
        if(r.setdown_search.exhausted())stop("No safe nearby set-down support; bike kept in hand. Move and try again.");
    };
    mount_pose::Seed seed;bool owner_changed{},measured_rejection{};MountSearchSupport observed;
    const bool ready=fresh_mount_seed(base,frame,r.setdown_search.candidate(),r.setdown_search.yaw(),
        seed,owner_changed,measured_rejection,r.setdown_search.support(),observed,true);
    if(!ready||owner_changed){
        if(!owner_changed&&measured_rejection)reject();
        else stop("Set-down proof unavailable or changed; bike kept in hand. Try again.");
        return;
    }
    auto target=seed.root;const auto height=chassis::rotate_unit(seed.orientation,{0,r.root_height,0});
    for(unsigned i=0;i<3;++i)target[i]+=height[i];
    if(!mount_transition::interaction_distance(mount_transition::length(mount_transition::sub(target,frame.position)),false)){reject();return;}
    const auto approach=probe_local_surface(frame.position,target,{},frame.entity);
    if(!approach.available||approach.world!=seed.request.world||approach.context!=seed.request.context){
        stop("Set-down approach unavailable; bike kept in hand.");return;
    }
    if(approach.hit){reject();return;}
    // Join the support lease and approach with a final fresh coherent retained
    // body/rider observation. No native body transform, force or allocation.
    NativeFrame fresh;const auto body=native_chassis::observe(base,frame,&fresh,true);
    if(native_chassis::status().observation_suspended){cancel_carried_setdown();withdraw_unavailable_bike_observation();return;}
    std::uintptr_t final_context{};
    const bool coherent=body.available&&fresh.available&&fresh.timing_available&&!body.active&&!body.detached&&
        body.serial==fresh.serial&&body.simulation_time==fresh.simulation_time&&body.step_seconds==fresh.seconds&&
        memory::peek(fresh.client+8,final_context)&&final_context;
    const RetainedBikeIdentity measured{{fresh.client,fresh.entity,fresh.core,fresh.context,fresh.rig},
        body.world,body.native_state,body.body_index,body.body_generation,body.token};
    if(!r.setdown_search.ready(seed,{measured,final_context,body.serial,body.simulation_time,
        coherent,fresh.offboard&&fresh.standable,approach.available,!approach.hit})){
        stop("Set-down owner or native step changed; bike kept in hand. Try again.");return;
    }
    const auto index=r.setdown_search.index();const bool mount_after=r.setdown_mount_after;
    visual::Frame placed;placed.active=true;placed.entity=fresh.entity;placed.position=seed.root;
    placed.yaw=seed.request.yaw;placed.pitch=seed.pitch;placed.roll=seed.roll;
    placed.chassis_quaternion=seed.orientation;placed.chassis_quaternion_available=true;
    placed.rider_mode=visual::RiderMode::none;
    if(mount_after&&r.quick_test.started()&&!r.quick_test.terminal())++r.quick_test_strict_setdowns;
    cancel_carried_setdown();r.parked_bike=placed;r.world_frame=placed;r.bike_carried=false;r.parked_ground_verified=true;
    logging::log(logging::Level::info,logging::Channel::skater,
        "BMX set-down support verified: candidate={}; token={}; serial={}; world={}; no native body writes.",
        index,body.token,body.serial,body.world);
    message("Bike set down. Tap Y/Triangle to get on.",true);
    if(mount_after){
        int empty=0;const bool queued=r.request.compare_exchange_strong(empty,7);
        if(queued&&r.quick_test.started()&&!r.quick_test.terminal()) {
            r.quick_test_setdown_mount_queued=true;++r.quick_test_setdown_followups;
        }
    }
}

bool park_bike(std::uintptr_t base,const NativeFrame& frame,bool carry) {
    auto& r=runtime();
    if(!frame.available||EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}!=
       EquipmentOwner{r.client,r.entity,r.core,r.context,r.rig}||!frame.offboard||
       frame.serial<r.chassis_step.serial||frame.serial-r.chassis_step.serial>2)return false;
    const auto body=native_chassis::observe(base,frame);std::uintptr_t context{};
    if(!body.available||!body.active||body.token!=r.chassis_step.body_token||
       !memory::peek(frame.client+8,context)||!context)return false;
    if(!r.active.load()||!r.chassis_started||!r.chassis_step.ready||!r.chassis_step.actual_support||
       std::abs(r.chassis_step.speed)>1.2f||r.chassis_step.landing_body_alignment<.85f) {
        message("Brake to a stop on clear ground before getting off the bike.",true);return false;
    }
    visual::Frame beside;
    if(!place_bike(base,frame,beside)) {
        message("There is not enough clear ground beside the bike to get off.",true);return false;
    }
    const auto saved=r.chassis_step.frame;const auto owner=EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig};
    end_ride("Off the bike. Tap Y/Triangle nearby to ride; hold it to pick up the bike. F6 equips the skateboard.");
    r.parked_bike=saved;r.parked_bike.rider_mode=visual::RiderMode::none;
    r.parked_bike.movement_id=catalog::TrickId::none;
    r.parked_bike.movement_bike_pitch=r.parked_bike.movement_bike_roll=0;
    r.parked_bike.barspin=r.parked_bike.tailwhip=0;
    r.parked_owner=owner;r.bike_parked=true;r.bike_carried=false;
    r.parked_ground_verified=true;
    r.parked_world=body.world;r.parked_token=body.token;r.parked_context=context;
    r.parked_identity={owner,body.world,body.native_state,body.body_index,body.body_generation,body.token};
    r.stepping_off=true;r.carry_after_step=carry;r.step_progress=0;r.step_serial=frame.serial;
    r.step_from=frame.position;r.step_to=beside.position;r.step_to[1]+=r.root_height;
    r.world_frame=r.parked_bike;visual::publish(r.world_frame);
    DingoSDKOverlaySetBmxInteractionCapture(true);
    return true;
}
void cancel_step_off_board(std::string_view reason) {
    auto& r=runtime();if(!r.step_off_board.active())return;
    r.step_off_board.cancel();r.stepping_off=r.carry_after_step=false;
    if(r.human_native_input_pending)overlay::detail::cancel_human_native_handoff("step_off_canceled");
    release_control();DingoSDKOverlaySetBmxInteractionCapture(false);
    if(r.bike_parked) {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto current=read_native_frame(base,r.parked_owner.client);
        if(!current.available) {withdraw_unavailable_bike_observation();return;}
        if(!current.offboard||
           EquipmentOwner{current.client,current.entity,current.core,current.context,current.rig}!=r.parked_owner||
           !native_chassis::parked_owner_matches(base,current,r.parked_world,r.parked_token,true)) {
            if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return;}
            native_chassis::forget();
            end_ride("Bike put away because its parked rider or physics ownership changed.");return;
        }
        r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::none;
        visual::publish(r.world_frame);
    }
    message(std::string(reason),true);
}
bool detach_crashed_bike(std::uintptr_t base,const NativeFrame& frame,const chassis_ride::Step& step) {
    auto& r=runtime();std::uintptr_t context{};
    if(!memory::peek(frame.client+8,context)||!context||!native_chassis::detach(base,frame))return false;
    const auto body=native_chassis::observe(base,frame);
    std::array<float,3> bike_root{};
    if(!body.available||!body.detached||body.token!=step.body_token||
       !chassis::root_from_com(body.position,body.orientation,collision_envelope::com_from_root,bike_root))return false;
    // Release rider follow/IK/protection, while keeping the separate native bike
    // body dynamic. end_ride must not park this detached body.
    chassis_ride::detach();r.chassis_started=false;
    end_ride("BMX crash: rider released; bike remains in the world.");
    r.bike_crashed=true;r.crash_body=body;r.crash_native_time=frame.simulation_time;
    r.crash_serial=body.serial;
    r.crash_last_observed=GetTickCount64();r.crash_samples.clear();
    r.parked_bike=step.frame;r.parked_bike.active=true;r.parked_bike.rider_mode=visual::RiderMode::none;
    r.parked_bike.position=bike_root;r.parked_bike.chassis_quaternion=body.orientation;
    r.parked_bike.chassis_quaternion_available=true;
    r.parked_bike.movement_id=catalog::TrickId::none;
    r.parked_bike.movement_bike_pitch=r.parked_bike.movement_bike_roll=0;
    r.parked_owner={frame.client,frame.entity,frame.core,frame.context,frame.rig};
    r.parked_world=body.world;r.parked_context=context;r.parked_token=body.token;
    r.crash_identity={r.parked_owner,body.world,body.native_state,body.body_index,body.body_generation,body.token};
    r.world_frame=r.parked_bike;visual::publish(r.world_frame);return true;
}
bool tick_crashed_bike(std::uintptr_t base,std::uintptr_t client,bool interaction_allowed,bool gameplay_ready) {
    auto& r=runtime();r.crash_interaction_ready=false;if(!r.bike_crashed)return false;
    auto rider=read_native_frame(base,client);
    if(service_retained_owner(base,client,&rider))return true;
    if(gameplay_ready&&!rider.available) {
        withdraw_unavailable_bike_observation();return true;
    }
    if(!gameplay_ready||
       EquipmentOwner{rider.client,rider.entity,rider.core,rider.context,rider.rig}!=r.parked_owner) {
        sample_fall_lifecycle(base,rider,{});
        end_ride("Crashed bike released after its local rider or world changed.");return true;
    }
    NativeFrame coherent_rider;
    const auto body=native_chassis::observe(base,rider,&coherent_rider,true);
    if(body.available)rider=std::move(coherent_rider); // the same freshly accepted owner/body epoch; no additional native read
    if(native_chassis::status().observation_suspended) {
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX released crash observation suspended: issue={}; snapshot issue={}; reads={}; token={}; rejected body not published.",
            body.issue,static_cast<unsigned>(body.snapshot_issue),body.snapshot_reads,r.crash_identity.token);
        withdraw_unavailable_bike_observation();return true;
    }
    sample_fall_lifecycle(base,rider,body);
    if(body.available) {
        std::array<float,3> root{};
        if(!body.detached||body.active||
           RetainedBikeIdentity{r.parked_owner,body.world,body.native_state,body.body_index,body.body_generation,body.token}!=r.crash_identity||
           !chassis::root_from_com(body.position,body.orientation,collision_envelope::com_from_root,root)) {
            end_ride("Crashed bike stopped after its physics ownership changed.");return true;
        }
        r.crash_body=body;r.crash_last_observed=GetTickCount64();
        r.parked_bike.position=root;r.parked_bike.chassis_quaternion=body.orientation;
        r.parked_bike.chassis_quaternion_available=true;
        if(impact_test::variant()&&r.crash_samples.size()<240) {
            const auto parts=world::snapshot();const auto status=native_chassis::status();
            const auto fall=bail::status();
            r.crash_samples.emplace_back(Json{{"serial",body.serial},{"time",body.simulation_time},
                {"position",root},{"quaternion",body.orientation},{"velocity",body.linear_velocity},
                {"angular_velocity",body.angular_velocity},{"parts",parts.live_parts},
                {"drive_active",status.active},{"detached",status.detached},
                {"applications",status.applications},{"offboard",rider.offboard},{"standable",rider.standable},
                {"fall_owner_matches",fall.entity==rider.entity},{"native_offboard_flags",fall.native_offboard_flags},
                {"native_ragdoll_mode",fall.native_ragdoll_mode},{"native_ragdoll_observed",fall.ragdoll_observed},
                {"intent_written",fall.intent_written},{"intent_reset",fall.intent_reset}});
        }
    } else if(GetTickCount64()-r.crash_last_observed>200) {
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX crash observation expiry: snapshot issue={}; reads={}; exhausted={}; rider available={}; timing={}; rider serial={}; native time={}; h={}; last accepted serial={}; native time={}; wall age={}ms; body token={}.",
            static_cast<unsigned>(body.snapshot_issue),body.snapshot_reads,body.snapshot_retry_exhausted,
            rider.available,rider.timing_available,rider.serial,rider.simulation_time,rider.seconds,
            r.crash_body.serial,r.crash_body.simulation_time,GetTickCount64()-r.crash_last_observed,r.crash_identity.token);
        end_ride("Crashed bike observation expired; physics stopped.");return true;
    }
    r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::none;
    const RetainedBikeIdentity current{{rider.client,rider.entity,rider.core,rider.context,rider.rig},
        body.world,body.native_state,body.body_index,body.body_generation,body.token};
    const bool coherent=body.available&&rider.available&&rider.timing_available&&body.detached&&!body.active&&
        body.serial==rider.serial&&body.simulation_time==rider.simulation_time&&body.step_seconds==rider.seconds;
    r.crash_interaction_ready=r.crash_interaction.observe({current,coherent,rider.offboard,rider.standable,
        rider.serial,rider.simulation_time,rider.seconds},bail::status());
    DingoSDKOverlaySetBmxInteractionCapture(interaction_allowed&&r.crash_interaction_ready);
    visual::publish(r.world_frame);return true;
}
bool recover_crashed_bike(std::uintptr_t base,const NativeFrame& rider) {
    auto& r=runtime();if(!r.bike_crashed)return true;
    if(service_retained_owner(base,rider.client,&rider))return false;
    const auto body=native_chassis::observe(base,rider,nullptr,true);
    if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return false;}
    crash_recovery::Input request;
    request.expected={r.crash_identity.owner.client,r.crash_identity.owner.entity,r.crash_identity.owner.core,r.crash_identity.owner.context,
        r.crash_identity.owner.rig,r.crash_identity.world,r.crash_identity.token,r.crash_identity.body_index,r.crash_identity.body_generation};
    request.current={rider.client,rider.entity,rider.core,rider.context,rider.rig,body.world,body.token,body.body_index,body.body_generation};
    request.owner_valid=rider.available;request.body_valid=body.available&&body.detached;
    request.native_standable=rider.standable;request.native_offboard=rider.offboard;
    request.native_started=r.crash_native_time;request.native_now=body.simulation_time;
    request.crash_serial=r.crash_serial;request.current_serial=body.serial;
    if(!chassis::root_from_com(body.position,body.orientation,collision_envelope::com_from_root,request.bike_root))return false;
    request.rider_ground=rider.position;request.rider_ground[1]-=r.root_height;
    request.linear_velocity=body.linear_velocity;
    const auto recovery=crash_recovery::evaluate(request);
    if(!recovery.eligible) {
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX crash recovery waiting: reason={}; distance={}m; height={}m; speed={}m/s; native elapsed={}s; standable={}; offboard={}.",
            static_cast<unsigned>(recovery.issue),recovery.horizontal_distance,recovery.vertical_distance,
            recovery.speed,recovery.elapsed,rider.standable,rider.offboard);
        message("Wait until you and the bike stop falling, then move beside the bike and tap Y/Triangle.",true);return false;
    }
    visual::Frame placed;
    if(!place_bike(base,rider,placed)) {message("Move to clear ground beside the bike to pick it up.",true);return false;}
    if(!native_chassis::park(base,rider,true)) {
        if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return false;}
        end_ride("Bike recovery stopped because physics ownership changed.");return false;
    }
    queue_fall_lifecycle(fall_lifecycle::Reason::body_released);
    r.bike_crashed=false;r.bike_parked=true;r.parked_bike=placed;r.parked_ground_verified=true;
    r.parked_identity=r.crash_identity;
    logging::log(logging::Level::info,logging::Channel::skater,
        "BMX crash recovery verified: body token={}; serial={}; native elapsed={}s; grounded placement accepted.",
        body.token,body.serial,request.native_now-request.native_started);
    r.world_frame=placed;visual::publish(placed);return true;
}
bool preflight_crashed_board_switch(std::uintptr_t base,std::uintptr_t client,EquipmentOwner& accepted_owner) {
    auto& r=runtime();if(!r.bike_crashed)return true;
    const auto rider=read_native_frame(base,client);
    if(service_retained_owner(base,client,&rider))return false;
    const auto body=native_chassis::observe(base,rider,nullptr,true);
    if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return false;}
    CrashedSwitchInput input;
    input.expected=r.crash_identity;
    input.current={{rider.client,rider.entity,rider.core,rider.context,rider.rig},
        body.world,body.native_state,body.body_index,body.body_generation,body.token};
    input.rider_available=rider.available;input.body_available=body.available;
    input.coherent_snapshot=rider.timing_available&&body.available&&body.serial==rider.serial&&
        body.simulation_time==rider.simulation_time&&body.step_seconds==rider.seconds;
    input.native_offboard=rider.offboard;input.native_standable=rider.standable;
    auto decision=crashed_switch_preflight(input);
    if(body.available&&(!body.detached||body.active))decision=CrashedSwitchDecision::invalidated;
    if(decision==CrashedSwitchDecision::allowed) {accepted_owner=input.current.owner;return true;}
    release_native_motion();
    DingoSDKOverlaySetBmxInputCapture(false);DingoSDKOverlaySetBmxInteractionCapture(false);
    r.observed_capture.store(false);r.crash_interaction_ready=false;
    if(decision==CrashedSwitchDecision::invalidated) {
        end_ride("Bike released because its rider or physics ownership changed.");return false;
    }
    logging::log(logging::Level::info,logging::Channel::skater,
        "BMX crashed switch deferred: decision={}; rider available={}; body available={}; coherent={}; standable={}; offboard={}; token={}; bike retained; no board request.",
        static_cast<unsigned>(decision),input.rider_available,input.body_available,input.coherent_snapshot,
        rider.standable,rider.offboard,r.parked_token);
    const auto identity_json=[](const RetainedBikeIdentity& identity) {
        return Json{{"client",identity.owner.client},{"entity",identity.owner.entity},{"core",identity.owner.core},
            {"context",identity.owner.context},{"rig",identity.owner.rig},{"world",identity.world},{"native_state",identity.native_state},
            {"body_index",identity.body_index},{"body_generation",identity.body_generation},{"body_token",identity.token}};
    };
    const auto motion_lease=native_motion_lease_status();const auto guard_chassis_status=native_chassis::status();
    const auto guard=Json{{"process_id",GetCurrentProcessId()},{"decision",static_cast<unsigned>(decision)},
        {"rider_available",input.rider_available},{"body_available",input.body_available},{"coherent",input.coherent_snapshot},
        {"standable",rider.standable},{"offboard",rider.offboard},{"detached",body.detached},{"drive_active",body.active},
        {"board_timer",r.board_switch.active()},{"bmx_capture",overlay::detail::bmx_input_capture_active()},
        {"interaction_capture",overlay::detail::bmx_interaction_capture_active()},
        {"rider_motion_lease",motion_lease.available?Json(motion_lease.active):Json()},
        {"serial",rider.serial},{"native_time",rider.simulation_time},{"step_seconds",rider.seconds},
        {"parts",world::snapshot().live_parts},{"mounts",guard_chassis_status.mounts},{"allocations",guard_chassis_status.allocation_attempts},
        {"velocity_updates",native_motion_updates()},{"rider_pose_updates",rider::updates()},
        {"rider_render_updates",rider::render_updates()},{"force_applications",guard_chassis_status.applications},
        {"identity",identity_json(input.current)},{"expected",identity_json(input.expected)}};
    logging::log(logging::Level::info,logging::Channel::skater,"BMX crashed switch guard: {}",guard.dump());
    message(decision==CrashedSwitchDecision::preserve_wait?
        "Get up using the normal Skate controls, then press F6 to switch to your skateboard.":
        "Waiting for a fresh rider and bike observation. Press F6 again when you are standing.",true);
    return false;
}
void begin_getup_lab(std::uintptr_t base,std::uintptr_t client,bool lifecycle_input,bool solo_gameplay) {
    auto& r=runtime();auto& lab=r.getup_lab;
    if(!impact_test::getup_enabled()) {message("GetUp diagnostic is unavailable outside its explicit private lab.",true);return;}
    if(lab.requested) {message("The private GetUp lab permits one explicit request per process.",true);return;}
    const auto frame=read_native_frame(base,client);
    const auto body=native_chassis::observe(base,frame,nullptr,true);
    if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return;}
    const auto observation=fall_observation(base,frame,body);
    const auto& raw=observation.rider.value;
    const auto fall=bail::status();const auto& history=r.fall_history.status();
    getup_lab::Input input;
    input.enabled=impact_test::getup_enabled();input.already_requested=lab.requested;
    input.crashed=r.bike_crashed;input.solo_gameplay=solo_gameplay;input.lifecycle_input=lifecycle_input;
    input.expected=r.crash_identity;
    input.current={{frame.client,frame.entity,frame.core,frame.context,frame.rig},
        body.world,body.native_state,body.body_index,body.body_generation,body.token};
    input.rider_available=frame.available;input.body_available=body.available;
    input.coherent_epoch=observation.coherent_epoch&&observation.body.available&&
        observation.snapshot_issue==fall_lifecycle::SnapshotIssue::none;
    input.detached=body.detached;input.drive_active=body.active;
    input.fall_receipt_fresh=history.phase==fall_lifecycle::Phase::recording&&
        history.receipt_binding==fall_lifecycle::ReceiptBinding::accepted_request&&
        history.receipt.request_id==r.impact_receipt.request_id&&history.receipt.generation==r.impact_receipt.generation&&
        history.identity.rider==fall_lifecycle::RiderIdentity{frame.client,frame.entity,frame.core,frame.context,frame.rig}&&
        history.identity.body==fall_lifecycle::BodyIdentity{body.world,body.native_state,body.body_index,body.body_generation,body.token}&&
        impact_receipt::matches(r.impact_receipt,fall)&&fall.ragdoll_observed&&fall.activation_enabled&&
        fall.intent_reset&&!fall.intent_reset_pending&&fall.intent_reset_readback;
    input.native_offboard=frame.offboard;input.native_standable=frame.standable;
    input.native_observation_available=observation.rider.available&&raw.physics_state.available&&raw.physics_state.value==504&&
        raw.ragdoll_mode.available&&raw.protection_active.available;
    input.known_ragdoll=raw.ragdoll_mode.available&&raw.ragdoll_mode.value;
    input.protected_owner=raw.protection_active.value;
    input.motion_lease_available=observation.capture.rider_motion_lease.available;
    input.motion_lease_active=observation.capture.rider_motion_lease.value;
    input.bmx_capture=observation.capture.bmx_capture.value;input.interaction_capture=observation.capture.interaction_capture.value;
    input.board_switch_pending=r.board_switch.active();
    input.body_velocity_available=observation.body.available;input.body_angular_velocity_available=observation.body.available;
    input.native_cached_velocity_available=raw.motion_velocity.available;
    input.body_velocity=body.linear_velocity;input.body_angular_velocity=body.angular_velocity;
    input.native_cached_velocity=raw.motion_velocity.value;
    const auto result=getup_lab::evaluate(input);
    // Single verified client thread owns this process-wide claim. Refusal is
    // also terminal for the lab; no background retry can later start the action.
    lab.requested=true;lab.identity=input.expected;lab.fall_receipt_fresh=input.fall_receipt_fresh;
    lab.fall_request_id=r.impact_receipt.request_id;lab.fall_generation=r.impact_receipt.generation;
    lab.requested_serial=frame.serial;lab.requested_time=frame.simulation_time;
    lab.down_before_action=observation.coherent_epoch&&input.native_observation_available&&input.known_ragdoll&&
        input.native_offboard&&!input.native_standable;
    fall_lifecycle::Sample initial;initial.clock=observation.clock.value;initial.rider=observation.rider;
    initial.body=observation.body;initial.counters=observation.counters;initial.capture=observation.capture;
    initial.snapshot_issue=observation.snapshot_issue;
    lab.preflight=fall_sample_json(initial);lab.preflight["eligible"]=result.eligible;
    lab.preflight["issue"]=static_cast<unsigned>(result.issue);
    const auto pool_status=native_chassis::status();const auto& counters=observation.counters.value;
    lab.baseline=Json{{"parts",world::snapshot().live_parts},{"mounts",pool_status.mounts},{"allocations",pool_status.allocation_attempts},
        {"velocity_updates",counters.velocity_updates},{"rider_pose_updates",counters.rider_poses},
        {"rider_render_updates",counters.render_poses},{"force_applications",counters.force_applications}};
    if(result.eligible) {
        const auto final_owner=read_native_frame(base,client);
        const bool final_epoch=final_owner.available&&final_owner.timing_available&&
            EquipmentOwner{final_owner.client,final_owner.entity,final_owner.core,final_owner.context,final_owner.rig}==input.expected.owner&&
            final_owner.serial==frame.serial&&final_owner.simulation_time==frame.simulation_time&&final_owner.seconds==frame.seconds;
        const intent_observer::GetupActionRequest request{lab.fall_request_id,lab.fall_generation,
            body.world,body.native_state,body.body_index,body.body_generation,body.token};
        lab.audit_started=final_epoch&&intent_observer::begin_getup_callback_audit(base,final_owner,request);
        lab.accepted=final_epoch&&intent_observer::arm_getup_action(base,final_owner,request);
    }
    logging::log(logging::Level::info,logging::Channel::skater,
        "BMX GetUp lab preflight: eligible={}; issue={}; accepted={}; serial={}; token={}; one-shot claimed.",
        result.eligible,static_cast<unsigned>(result.issue),lab.accepted,frame.serial,body.token);
    message(lab.accepted?"Private GetUp intent armed once; native animation and reset are being observed.":
        "Private GetUp request declined; no later automatic retry will be made.",true);
}
bool tick_parked_bike(std::uintptr_t base,std::uintptr_t client,bool allowed,bool gameplay_ready) {
    auto& r=runtime();if(!r.bike_parked||r.pending||r.active.load())return false;
    const auto frame=read_native_frame(base,client);
    if(service_retained_owner(base,client,&frame))return true;
    if(gameplay_ready&&!frame.available) {
        withdraw_unavailable_bike_observation();return true;
    }
    if(!gameplay_ready||EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}!=r.parked_owner||!frame.offboard) {
        end_ride("Bike put away because the local rider or world changed.");return true;
    }
    if(!native_chassis::parked_owner_matches(base,frame,r.parked_world,r.parked_token,true)) {
        if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return true;}
        end_ride("Bike put away because its parked physics ownership changed.");return true;
    }
    r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::none;
    DingoSDKOverlaySetBmxInteractionCapture(allowed);
    if(!allowed) {
        cancel_carried_setdown();
        if(r.step_off_board.active())cancel_step_off_board("Skateboard switch canceled; return to the game and press F6 again.");
        else {release_native_motion();DingoSDKOverlaySetBmxInputCapture(false);visual::publish(r.world_frame);}
        return true;
    }
    if(r.stepping_off) {
        if(frame.serial!=r.step_serial) {
            r.step_serial=frame.serial;
            if(frame.timing_available&&frame.seconds>0&&frame.seconds<.05f)r.step_progress=std::min(1.f,r.step_progress+frame.seconds/.4f);
        }
        const float blend=r.step_progress*r.step_progress*(3-2*r.step_progress);
        std::array<float,3> target{},velocity{};
        for(unsigned i=0;i<3;++i) {target[i]=r.step_from[i]+(r.step_to[i]-r.step_from[i])*blend;
            velocity[i]=(r.step_to[i]-r.step_from[i])*6*r.step_progress*(1-r.step_progress)/.4f;}
        if(!submit_native_step_off_motion(frame,velocity,target,r.parked_bike.yaw)) {
            if(r.step_off_board.active()) {cancel_step_off_board("Skateboard switch canceled because the step off was interrupted.");return true;}
            r.stepping_off=false;release_control();message("Step off interrupted; walking control restored.",true);
        } else {
            DingoSDKOverlaySetBmxInputCapture(true);
            r.world_frame.rider_mode=visual::RiderMode::riding;r.world_frame.rider_blend=1-blend;
            if(r.step_progress>=1) {r.stepping_off=false;r.bike_carried=r.carry_after_step;
                if(r.bike_carried)r.parked_ground_verified=false;release_control();}
        }
    } else if(r.bike_carried) {
        const float yaw=std::atan2(frame.transform[8],frame.transform[10]);
        auto carried=r.parked_bike;carried.chassis_quaternion_available=false;carried.yaw=yaw;carried.pitch=0;carried.roll=.25f;
        const auto& offset=rider::motion::carry_bike_offset;
        carried.position={frame.position[0]+offset[0]*std::cos(yaw)+offset[2]*std::sin(yaw),
            frame.position[1]-r.root_height+offset[1],
            frame.position[2]-offset[0]*std::sin(yaw)+offset[2]*std::cos(yaw)};
        carried.rider_mode=visual::RiderMode::carrying;
        // Native walking keeps its normal capsule. Bound the attached bike's
        // translation against fresh rays; this is not a carried convex body.
        std::array<world::Matrix,world::part_count> old_parts{},new_parts{};
        bool clear=world::transforms(r.parked_bike,old_parts)&&world::transforms(carried,new_parts);
        bool measured_obstacle=false;
        struct CarryProbe { std::array<float,3> point; unsigned part; };
        constexpr std::array<CarryProbe,4> carry_probes{{{geometry::rear_axle,2},{geometry::front_axle,3},
            {geometry::left_grip.center,5},{geometry::right_grip.center,5}}};
        for(const auto& probe:carry_probes) {
            if(!clear)break;
            std::array<float,3> from{},to{};float distance2=0;
            for(unsigned i=0;i<3;++i) {
                from[i]=old_parts[probe.part][12+i];to[i]=new_parts[probe.part][12+i];
                for(unsigned j=0;j<3;++j) {from[i]+=old_parts[probe.part][j*4+i]*probe.point[j];
                    to[i]+=new_parts[probe.part][j*4+i]*probe.point[j];}
                distance2+=(to[i]-from[i])*(to[i]-from[i]);
            }
            if(distance2>4) {clear=false;break;}
            if(distance2<.000001f)continue;
            const auto hit=probe_local_surface(from,to,{},frame.entity);
            if(!hit.available||hit.world!=r.parked_world||hit.context!=r.parked_context) {clear=false;break;}
            if(hit.hit) {clear=false;measured_obstacle=true;break;}
        }
        const auto carry_path=clear?CarryPathObservation::clear:
            (measured_obstacle?CarryPathObservation::measured_obstacle:CarryPathObservation::unknown);
        if(clear) {
            if(r.setdown_search.active()&&r.setdown_from_obstacle)cancel_carried_setdown();
            r.carry_obstacle_refused=false;r.parked_bike=carried;r.world_frame=carried;
        } else {
            // Keep the last clear carried frame. Only an actual fresh hit may
            // initiate automatic set-down; an unavailable/path-transform/world
            // observation cancels the intent and leaves the bike carried.
            if(carry_path==CarryPathObservation::unknown)cancel_carried_setdown();
            else if(!r.setdown_search.active()&&!r.carry_obstacle_refused)
                (void)begin_carried_setdown(frame,false,true);
            r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::carrying;
        }
        if(setdown_search_permitted(carry_path,!r.setdown_from_obstacle))tick_carried_setdown(base,frame);
    }
    if(r.step_off_board.active()) {
        const auto switch_owner=r.step_off_board.owner();
        const auto original_switch_deadline=r.step_off_board.pending_deadline_ms();
        const auto motion=native_motion_lease_status();
        StepOffBoardSample sample;
        sample.allowed=allowed;sample.available=frame.available;sample.timing_available=frame.timing_available;
        sample.parked_proven=true; // full parked_owner_matches proof above
        sample.stepping_off=r.stepping_off;sample.offboard=frame.offboard;sample.standable=frame.standable;
        sample.motion_released=motion.available&&!motion.active&&!overlay::detail::bmx_input_capture_active();
        sample.owner={frame.client,frame.entity,frame.core,frame.context,frame.rig};
        sample.world=r.parked_world;sample.token=r.parked_token;
        sample.serial=frame.serial;sample.native_time=frame.simulation_time;sample.root=frame.position;
        const auto decision=r.step_off_board.update(GetTickCount64(),sample);
        if(decision==StepOffBoardResult::ready) {
            logging::log(logging::Level::info,logging::Channel::skater,
                "BMX grounded step off accepted for skateboard: token={}; serial={}; native time={}; endpoint error={}m.",
                r.parked_token,frame.serial,frame.simulation_time,
                mount_transition::length(mount_transition::sub(frame.position,r.step_to)));
            if(r.human_native_input_pending) {
                const auto released=native_chassis::released_ownership_status();
                native_input_handoff::Ticket ticket;
                ticket.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
                ticket.body={released.world,released.native_state,released.body_index,released.body_generation,released.token};
                ticket.deadline_ms=std::min(original_switch_deadline,overlay::detail::human_native_handoff_deadline());
                ticket.serial=frame.serial;ticket.native_time=frame.simulation_time;
                end_ride("BMX cleanup complete. Release LB/L1 and Y/Triangle for the native skateboard.");
                const auto publication=rider::private_handoff_publication();const auto physical=native_chassis::status();
                ticket.counters={native_motion_updates(),publication.poses,publication.renders,publication.board_hides,
                    physical.applications,physical.allocation_attempts,physical.mounts};
                r.human_native_input_ticket=ticket;
                if(!released.available||released.active||released.lease_active||released.failed||
                    !overlay::detail::transfer_human_native_handoff(ticket,GetTickCount64()))
                    overlay::detail::cancel_human_native_handoff("released_body_or_cleanup_ticket_refused");
                return true; // Never starts the software board helper for this leg.
            }
            if(overlay::detail::private_bmx_native_input_handoff_requested()) {
                // Exact first trial: only the single synthetic quick prefix may
                // enter this route. Never fall back to the software board helper.
                if(!r.quick_test.awaiting_native_board()) {
                    end_ride("Private native input handoff refused outside its first released quick prefix.");return true;
                }
                const auto released=native_chassis::released_ownership_status();
                native_input_handoff::Ticket ticket;
                ticket.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
                ticket.body={released.world,released.native_state,released.body_index,released.body_generation,released.token};
                ticket.deadline_ms=original_switch_deadline;ticket.serial=frame.serial;ticket.native_time=frame.simulation_time;
                end_ride("Private BMX cleanup complete; waiting for a native input handoff baseline.");
                const auto publication=rider::private_handoff_publication();const auto physical=native_chassis::status();
                ticket.counters={native_motion_updates(),publication.poses,publication.renders,publication.board_hides,
                    physical.applications,physical.allocation_attempts,physical.mounts};
                r.private_native_input_ticket=ticket;
                r.private_native_input_started=released.available&&!released.active&&!released.lease_active&&!released.failed&&
                    overlay::detail::begin_private_bmx_native_input_handoff(ticket,GetTickCount64());
                if(!r.private_native_input_started)r.quick_test.finish_private_native_input_prefix(false,"private_input_cleanup_ticket_refused");
                return true;
            }
            end_ride("Off the bike. Switching to the native skateboard.");
            r.board_switch.request(GetTickCount64(),switch_owner);return true;
        }
        if(decision==StepOffBoardResult::cancelled||decision==StepOffBoardResult::timed_out) {
            // update consumed the intent; release only its remaining step-off.
            r.stepping_off=r.carry_after_step=false;release_control();DingoSDKOverlaySetBmxInteractionCapture(false);
            r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::none;
            message(decision==StepOffBoardResult::timed_out?
                "Skateboard switch timed out; stand beside the bike and press F6 again.":
                "Skateboard switch canceled because its rider or parked bike changed.",true);
        }
    }
    visual::publish(r.world_frame);return true;
}
// A bounded ray fan catches the static obstacles in front of the rider and wheels.
// This is a prototype approximation: it is not a convex sweep or dynamic bike collision.
bool path_clear(const NativeFrame& native, const State& next, float root_height, bool& available) {
    available = true;
    const float dx = static_cast<float>(next.x) - native.position[0];
    const float dz = static_cast<float>(next.z) - native.position[2];
    const float travel = std::hypot(dx, dz);
    if (travel < .0001f) return true;
    const float fx = dx / travel, fz = dz / travel;
    for (const float side : {-.32f, 0.0f, .32f}) {
        for (const float height : {.4f, 1.0f}) {
            std::array<float, 3> from{native.position[0] + fz * side,
                native.position[1] - root_height + height, native.position[2] - fx * side};
            auto to = from;
            to[0] += fx * (travel + .65f); to[2] += fz * (travel + .65f);
            const auto p = probe_local_surface(from, to, {}, native.entity);
            if (!p.available) { available = false; return false; }
            if (p.hit) return false;
        }
    }
    return true;
}
void publish_visual(const NativeFrame& frame, float pitch) {
    auto& r = runtime();
    const auto& s = r.bike.state();
    visual::Frame render;
    render.active = true;
    render.entity = frame.entity;
    // Keep world-model readiness while admission queries run. No rider pose
    // may be applied before a physical body and its mount entry are acquired.
    if(r.native_chassis_mode&&!r.chassis_started)render.rider_mode=visual::RiderMode::none;
    // Render at the physical rider, so a delayed motion update cannot leave the bike ahead.
    render.position = frame.position;
    render.position[1] -= r.root_height;
    render.yaw = static_cast<float>(s.yaw);
    render.pitch = pitch;
    render.roll = static_cast<float>(-s.steering_angle * std::min(std::abs(s.speed) / 12.0, .5));
    render.steer = static_cast<float>(s.steering_angle);
    render.front_wheel_phase = static_cast<float>(s.front_wheel_phase);
    render.rear_wheel_phase = static_cast<float>(s.rear_wheel_phase);
    render.crank_phase = static_cast<float>(s.crank_phase);
    render.barspin = static_cast<float>(r.barspin);
    render.tailwhip = static_cast<float>(r.tailwhip);
    render.preload = r.preload;
    render.front_weight = r.front_weight;
    render.rear_weight = r.rear_weight;
    render.airborne = !s.grounded;
    r.world_frame = render;
    visual::publish(render);
}
// The physical mode never advances Model or writes a bike pose/velocity target.
// Only the native rider follows the independently simulated chassis.
void finish_chassis_pause_test(bool passed, const char* issue) {
    auto& r = runtime();
    const auto root = user_data::override_root();
    const auto current = native_chassis::status();
    if (root) {
        const Json report{{"process_id",GetCurrentProcessId()}, {"completed",true}, {"passed",passed}, {"issue",issue},
            {"position_drift_m",r.chassis_pause_position_error}, {"quaternion_component_drift",r.chassis_pause_rotation_error},
            {"body_token",r.chassis_pause_frame.token}, {"allocation_attempts_before",r.chassis_pause_status.allocation_attempts},
            {"allocation_attempts_after",current.allocation_attempts}, {"mounts_before",r.chassis_pause_status.mounts},
            {"mounts_after",current.mounts}, {"wall_elapsed_ms",GetTickCount64()-r.chassis_pause_started}};
        std::ofstream file(*root / L"bmx-chassis-lifecycle.json"); file << report.dump(2) << '\n';
    }
    r.chassis_pause_active = false;
    message(passed ? "BMX chassis pause/resume test passed: same body and pose; no allocation or reseed." :
        "BMX chassis pause/resume test failed: " + std::string(issue), true);
}
bool prepared_ramp_entry_matches(const ramp_test::RideRoute& route,const NativeFrame& frame) {
    if(frame.client!=route.owner.client||frame.entity!=route.owner.entity||frame.core!=route.owner.core||
       frame.context!=route.owner.context||frame.rig!=route.owner.rig||
       !ramp_rehearsal::entry_position_valid(frame.position,route.start,route.forward)||
       std::hypot(frame.position[0]-route.owner.position[0],frame.position[2]-route.owner.position[2])>.02f)return false;
    const auto editor=local_park_editor();
    auto from=frame.position,to=frame.position;from[1]+=2;to[1]-=3;
    const auto floor=probe_local_surface(from,to,{},frame.entity);
    return editor.available&&!editor.busy&&!editor.failed&&editor.generation==route.generation&&
        editor.revision==route.revision&&editor.objects.size()==1&&editor.objects.front().id==route.object_id&&
        editor.objects.front().spawned&&floor.available&&floor.hit&&floor.world==route.world&&
        std::isfinite(floor.position[1])&&std::abs(floor.position[1]-route.floor_y)<=.03f;
}
void tick_native_chassis(std::uintptr_t base, const NativeFrame& observed_frame,
                         RawInput raw, bool input_focused, bool private_switch_gate) {
    auto& r = runtime();
    auto frame=observed_frame;
    if(!r.chassis_started&&GetTickCount64()>=r.pending_until) {
        stop_mount_admission("BMX mount timed out waiting for fresh wheel support on gentle ground.");return;
    }
    if (!world::snapshot().ready) {
        if(r.chassis_started&&r.mount_blend<1&&!r.rehearsal) {end_ride("BMX mount canceled because its world model became unavailable.");return;}
        release_control(); r.clock.reset();
        publish_visual(frame, 0);
        message("BMX waiting for its native world model: " + world::snapshot().issue);
        return;
    }
    if (!frame.timing_available || frame.seconds <= 0) {
        if(r.chassis_started&&r.mount_blend<1&&!r.rehearsal) {end_ride("BMX mount canceled because its native timing became unavailable.");return;}
        release_control(); r.clock.reset();
        if (r.chassis_started&&r.chassis_step.frame.active) {
            r.world_frame = r.chassis_step.frame; visual::publish(r.world_frame);
        } else publish_visual(frame, 0);
        message("BMX waiting for the local rider's native physics steps.");
        return;
    }
    if (!r.chassis_started) {
        // World parts and native timing can become ready after pending mount.
        // Recheck the observed entry immediately before allocating the body.
        if(r.ramp_rehearsal&&!prepared_ramp_entry_matches(r.ramp_route,frame)) {
            end_ride("Ramp rehearsal stopped because its prepared start changed before body creation.");return;
        }
        auto root = frame.position; root[1] -= r.root_height;
        auto yaw = r.ramp_rehearsal ? r.ramp_route.yaw : std::atan2(frame.transform[8], frame.transform[10]);
        if(r.mount_from_parked) {root=r.parked_bike.position;yaw=r.parked_bike.yaw;}
        mount_pose::Seed seed;const mount_pose::Seed* selected_seed=nullptr;
        // The private impact variants intentionally retain their established
        // inverted/side drop seed. They are never an ordinary-mount fallback.
        const bool nearby_search=!r.rehearsal&&!impact_test::variant();
        if(nearby_search) {
            const EquipmentOwner owner{frame.client,frame.entity,frame.core,frame.context,frame.rig};
            if(!r.mount_search.active()) {
                const bool began=r.mount_from_parked?
                    r.mount_search.begin_retained(r.parked_identity,r.parked_context,root,yaw):
                    r.mount_search.begin(owner,0,root,yaw);
                if(!began) {end_ride("BMX mount canceled because its nearby search origin is invalid.");return;}
            }
            const auto search=r.mount_from_parked?
                r.mount_search.next_retained(r.parked_identity,r.parked_context,owner,frame.serial,frame.simulation_time):
                r.mount_search.next(owner,0,frame.serial,frame.simulation_time);
            if(search==NearbyMountDecision::cancelled) {
                if(r.mount_from_parked&&service_retained_owner(base,frame.client))return;
                end_ride("BMX mount canceled because its nearby search owner or native clock changed.");return;
            }
            if(search==NearbyMountDecision::exhausted) {
                stop_mount_admission("No safe nearby bike support. Move to clear gentle ground and try again.");return;
            }
            if(search==NearbyMountDecision::waiting) {
                release_control();r.clock.reset();publish_visual(frame,0);
                message("BMX waiting for a fresh native mount-support step.");return;
            }
            root=r.mount_search.candidate();yaw=r.mount_search.yaw();
        }
        if(!impact_test::variant()) {
            bool owner_changed{},measured_rejection{};MountSearchSupport observed_support;
            const bool ready=fresh_mount_seed(base,frame,root,yaw,seed,owner_changed,measured_rejection,
                nearby_search?r.mount_search.support():MountSearchSupport{},observed_support);
            if(nearby_search) {
                if(observed_support.world&&!r.mount_search.bind_support(observed_support))owner_changed=true;
            }
            if(!ready||owner_changed) {
                if(owner_changed) {
                    if(service_retained_owner(base,frame.client))return;
                    end_ride("BMX mount canceled because its support world or rider changed.");return;
                }
                if(nearby_search) {
                    r.mount_search.finish_attempt(measured_rejection);
                    if(r.mount_search.exhausted()) {stop_mount_admission("No safe nearby bike support. Move to clear gentle ground and try again.");return;}
                }
                release_control();r.clock.reset();publish_visual(frame,0);
                message("BMX waiting for fresh wheel support on gentle ground.");return;
            }
            root=seed.root;selected_seed=&seed;
        }
        if(nearby_search&&selected_seed) {
            auto target=root;const auto offset=chassis::rotate_unit(selected_seed->orientation,{0,r.root_height,0});
            for(unsigned i=0;i<3;++i)target[i]+=offset[i];
            const auto distance=mount_transition::length(mount_transition::sub(target,frame.position));
            bool clear=mount_transition::interaction_distance(distance,false);
            // Same standing-root approach witness as set-down placement. The
            // unchanged actual wheel witnesses remain the support authority.
            if(clear&&distance>.001f) {
                const auto approach=probe_local_surface(frame.position,target,{},frame.entity);
                r.mount_seed_last["search_approach"]=Json{{"available",approach.available},{"hit",approach.hit},
                    {"world",approach.world},{"context",approach.context},{"distance",distance}};
                if(approach.available&&(approach.world!=selected_seed->request.world||approach.context!=selected_seed->request.context)) {
                    end_ride("BMX mount canceled because its approach support world changed.");return;
                }
                if(!approach.available) {
                    r.mount_seed_last["stage"]="search_approach_unavailable";
                    r.mount_search.finish_attempt(false);release_control();r.clock.reset();publish_visual(frame,0);
                    message("BMX waiting for a fresh nearby approach query.");return;
                }
                clear=!approach.hit;
            }
            r.mount_seed_last["search_approach_clear"]=clear;
            if(!clear) {
                r.mount_seed_last["stage"]="search_approach_rejected";
                r.mount_search.finish_attempt(true);
                if(r.mount_search.exhausted()) {stop_mount_admission("No clear nearby bike approach. Move to clear ground and try again.");return;}
                release_control();r.clock.reset();publish_visual(frame,0);
                message("BMX checking another nearby mount point.");return;
            }
        }
        if(r.mount_from_parked&&selected_seed&&!r.rehearsal) {
            auto target=root;const auto offset=chassis::rotate_unit(selected_seed->orientation,{0,r.root_height,0});
            for(unsigned i=0;i<3;++i)target[i]+=offset[i];
            if(!mount_transition::interaction_distance(mount_transition::length(mount_transition::sub(target,frame.position)),false)) {
                // Keep the parked world/body and its token intact. The player
                // can step closer and retry without allocating or hiding it.
                release_control();r.mapper.reset();r.clock.reset();r.active.store(false);r.pending=false;
                r.mount_from_parked=false;r.mount_owner.reset();r.mount_dismount.reset();r.mount_entry={};r.mount_entry_token=0;
                r.world_frame=r.parked_bike;r.world_frame.rider_mode=visual::RiderMode::none;visual::publish(r.world_frame);
                message("Stand closer beside the bike before getting on.",true);return;
            }
        }
        mount_transition::BodyIdentity predecessor;
        if(r.mount_from_parked) {
            const auto parked=native_chassis::observe(base,frame);
            if(!parked.available||parked.active||parked.detached||parked.token!=r.parked_token||parked.world!=r.parked_world) {
                end_ride("BMX mount canceled because the parked body changed.");return;
            }
            predecessor={{frame.client,frame.entity,frame.core,frame.context,frame.rig},parked.world,
                parked.native_state,parked.body_index,parked.body_generation,parked.token};
        }
        skeleton_phase_probe::arm(skeleton_phase_probe::Window::before_first_mount);
        if(r.mount_from_parked&&!native_chassis::prepare_retained_acquire(base,runtime_retained_key())) {
            if(service_retained_owner(base,frame.client))return;
            end_ride("BMX retained mount predecessor changed before acquisition.");return;
        }
        if (!chassis_ride::start(base, frame, root, yaw, r.root_height,selected_seed)) {
            if(service_retained_owner(base,frame.client,&frame))return;
            const auto status=native_chassis::status();const std::string_view issue=status.issue;
            if(selected_seed&&!status.failed&&(issue=="fresh mount support lease unavailable"||
                issue=="mount support clock changed before seed"||issue=="mount support clock changed before activation")) {
                release_control();r.clock.reset();publish_visual(frame,0);
                if(nearby_search)r.mount_search.finish_attempt(false);
                message("BMX waiting for the next fresh mount support step.");return;
            }
            end_ride("BMX physical chassis could not start: " + std::string(native_chassis::status().issue)); return;
        }
        r.chassis_started = true;r.mount_search.cancel();
        if(!r.rehearsal) {
            // Pending support may have taken several frames. Capture the live
            // standing root/orientation only after this body's acquisition.
            NativeFrame standing;
            auto body=native_chassis::observe(base,frame,&standing);
            // A second COMPLETE observation may sample a later native epoch;
            // each accepted rider/body pair must still be coherent internally.
            // Never retry ownership/configuration/read failures or reseed here.
            if(!body.available&&(body.snapshot_issue==snapshot_retry::Issue::clock_changed||
               (body.snapshot_retry_exhausted&&(body.snapshot_issue==snapshot_retry::Issue::ring_header_changed||
                                                body.snapshot_issue==snapshot_retry::Issue::ring_content_changed)))) {
                body=native_chassis::observe(base,frame,&standing);
            }
            mount_transition::Quat rotation;
            const mount_transition::BodyIdentity acquired{{standing.client,standing.entity,standing.core,standing.context,standing.rig},
                body.world,body.native_state,body.body_index,body.body_generation,body.token};
            if(!standing.available||!standing.offboard||!standing.standable||
               EquipmentOwner{standing.client,standing.entity,standing.core,standing.context,standing.rig}!=
               EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}||
               !body.available||!body.active||!body.token||body.serial!=standing.serial||
               body.simulation_time!=standing.simulation_time||body.step_seconds!=standing.seconds||
               (selected_seed&&body.world!=selected_seed->request.world)||
               (r.mount_from_parked&&!mount_transition::remount_matches(predecessor,acquired))||
               !mount_transition::native_rotation(standing.transform,rotation)||!r.mount_entry.capture(standing.position,rotation)) {
                logging::log(logging::Level::warning,logging::Channel::skater,
                    "BMX mount capture refused: rider available/offboard/standable={}/{}/{}; body available/active/token={}/{}/{}; rider epoch={}/{}; body epoch={}/{}; step={}/{}; body issue={}; snapshot issue={}; retained={}; owner matches={}; seed world matches={}; predecessor matches={}.",
                    standing.available,standing.offboard,standing.standable,body.available,body.active,body.token,
                    standing.serial,standing.simulation_time,body.serial,body.simulation_time,standing.seconds,body.step_seconds,
                    body.issue,static_cast<unsigned>(body.snapshot_issue),r.mount_from_parked,
                    EquipmentOwner{standing.client,standing.entity,standing.core,standing.context,standing.rig}==
                        EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig},
                    !selected_seed||body.world==selected_seed->request.world,
                    !r.mount_from_parked||mount_transition::remount_matches(predecessor,acquired));
                end_ride("BMX mount canceled because its acquired rider or body changed.");return;
            }
            r.mount_entry_token=body.token;
            r.mount_entry_began=r.mount_entry_completed=false;
            frame=std::move(standing);
        }
        begin_ride_trace();
        r.mount_from_parked=r.bike_parked=r.bike_carried=false;
        r.parked_ground_verified=false;
        DingoSDKOverlaySetBmxInteractionCapture(false);
        r.clock.reset();
    }
    bool resumed_test{};
    if (r.chassis_pause_requested) {
        if(r.mount_blend<1&&!r.rehearsal) {end_ride("BMX mount canceled by the native pause request.");return;}
        r.chassis_pause_requested = false;
        r.chassis_pause_frame = native_chassis::observe(base,frame);
        r.chassis_pause_status = native_chassis::status();
        r.chassis_pause_position_error = r.chassis_pause_rotation_error = 0;
        r.chassis_pause_started = GetTickCount64(); r.chassis_pause_until = frame.simulation_time + .25;
        r.chassis_pause_active = true;
        if (!r.chassis_pause_frame.available || !r.chassis_pause_frame.active) {
            finish_chassis_pause_test(false,"initial body unavailable"); end_ride("BMX pause test could not observe its body."); return;
        }
        chassis_ride::pause(base,frame); release_native_motion();
        DingoSDKOverlaySetBmxInputCapture(false); r.clock.reset();
        message("BMX chassis pause test: holding the owned body for a quarter native second.",true);
    }
    if (r.chassis_pause_active) {
        const auto parked = native_chassis::observe(base,frame);
        const auto status = native_chassis::status();
        double position_error = 0, same_q_error = 0, opposite_q_error = 0;
        for (unsigned i=0;i<3;++i) position_error += std::pow(parked.position[i]-r.chassis_pause_frame.position[i],2);
        for (unsigned i=0;i<4;++i) {
            same_q_error += std::pow(parked.orientation[i]-r.chassis_pause_frame.orientation[i],2);
            opposite_q_error += std::pow(parked.orientation[i]+r.chassis_pause_frame.orientation[i],2);
        }
        r.chassis_pause_position_error = std::max(r.chassis_pause_position_error,std::sqrt(position_error));
        r.chassis_pause_rotation_error = std::max(r.chassis_pause_rotation_error,std::sqrt(std::min(same_q_error,opposite_q_error)));
        if (!parked.available || parked.active || !status.parked || parked.token != r.chassis_pause_frame.token ||
            status.allocation_attempts != r.chassis_pause_status.allocation_attempts || status.mounts != r.chassis_pause_status.mounts ||
            r.chassis_pause_position_error > .001 || r.chassis_pause_rotation_error > .001 ||
            GetTickCount64()-r.chassis_pause_started > 5000) {
            finish_chassis_pause_test(false,"parked pose, owner or pool changed"); end_ride("BMX pause test rejected a changed body."); return;
        }
        if (frame.simulation_time < r.chassis_pause_until) {
            r.clock.reset(); r.world_frame = r.chassis_step.frame; visual::publish(r.world_frame); return;
        }
        resumed_test = true; r.clock.reset();
    }
    const double seconds = r.clock.sample_time(frame.serial, frame.simulation_time);
    if (r.clock.discontinuity()) {
        end_ride("BMX physical chassis stopped after a native simulation discontinuity."); return;
    }
    const auto sampled_raw=raw;
    if(private_switch_gate)raw=automated_switch::neutral_input();
    const auto input_source=!r.rehearsal?(private_switch_gate?ride_trace::InputSource::private_switch_neutral:
        ride_trace::InputSource::adapter_sample):
        r.ramp_rehearsal?ride_trace::InputSource::ramp_rehearsal:
        r.catalog_rehearsal?ride_trace::InputSource::catalog_rehearsal:
        r.trick_rehearsal?ride_trace::InputSource::trick_rehearsal:ride_trace::InputSource::flat_rehearsal;
    if (r.rehearsal) raw = r.ramp_rehearsal ? r.ramp_driver.controls(r.rehearsal_seconds + seconds) :
        r.catalog_rehearsal ? selftest::catalog_controls(r.rehearsal_seconds + seconds) :
        r.trick_rehearsal ? selftest::chassis_trick_controls(r.rehearsal_seconds + seconds) :
            selftest::controls(r.rehearsal_seconds + seconds);
    const bool mount_impact_gate=(!r.rehearsal&&r.mount_blend<1)||impact_test::variant();
    const bool input_neutralized=mount_impact_gate||(private_switch_gate&&!r.rehearsal);
    if(mount_impact_gate) {
        const auto device=raw.device;const bool available=raw.available,focused=raw.focused;
        raw={};raw.device=device;raw.available=available;raw.focused=focused;
    }
    const auto bridge_started = std::chrono::steady_clock::now();
    auto step = chassis_ride::update(base, frame, raw, frame.seconds);
    if (r.rehearsal) {
        const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bridge_started).count();
        r.chassis_bridge_ms += ms; ++r.chassis_bridge_calls;
        r.chassis_bridge_peak_ms = std::max(r.chassis_bridge_peak_ms,ms);
    }
    if (!step.ready) {
        if (resumed_test) finish_chassis_pause_test(false,step.issue);
        end_ride("BMX physical chassis stopped: " + std::string(step.issue)); return;
    }
    // One SDK status snapshot in the existing trace observation phase. Keep
    // it with full bounded rehearsal rows so early landing witnesses are not
    // overwritten by the ordinary240-sample trailing ride history.
    const auto producer_observation=(!r.ride_trace_root.empty()||
        (r.rehearsal&&r.chassis_samples.size()<900))?native_chassis::status():native_chassis::Status{};
    if(!r.ride_trace_root.empty())(void)r.ride_history.record({step,raw,producer_observation,
        frame.position,frame.offboard,frame.standable,frame.motion_supported,sampled_raw,input_source,input_neutralized,private_switch_gate});
    if(step.bail.bail) {
        skeleton_phase_probe::arm(skeleton_phase_probe::Window::first_bail);
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX impact bail: reason={}; region={}; serial={}; alignment={}; closing={}m/s; residual={}Ns; guard rays={}.",
            static_cast<unsigned>(step.bail.reason),static_cast<unsigned>(step.bail.region),step.serial,
            step.bail.alignment,step.bail.closing_speed,step.impact_sample.residual_impulse_ns,step.body_guard_rays);
        if(!detach_crashed_bike(base,frame,step))end_ride("BMX impact: bike control released; detached body unavailable.");
        const auto previous_request_id=bail::status().request_id;
        const bool requested=bail::request(base,frame);
        const auto request_status=bail::status();
        const bool fresh_request_status=request_status.request_id>previous_request_id&&request_status.entity==frame.entity;
        if(r.bike_crashed)r.crash_interaction.begin(r.crash_identity,
            fresh_request_status?impact_receipt::capture(requested,request_status,frame.entity):impact_receipt::Receipt{},
            r.crash_serial,r.crash_native_time);
        begin_fall_lifecycle(base,frame,requested,request_status,fresh_request_status);
        if(r.ride_trace_pending&&r.pending_ride_history.token()==step.body_token&&r.ride_trace_entity==frame.entity){
            r.ride_trace_has_fall_request=true;r.ride_trace_fall_accepted=requested;r.ride_trace_fall_status=request_status;
            r.ride_trace_fall_status_fresh=fresh_request_status;
        }
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX fall handoff: accepted={}; request={}; generation={}; fresh status={}; status available={}; owner matches={}; protected={}; ready={}; flags={}; issue={}",
            requested,request_status.request_id,request_status.intent_generation,fresh_request_status,request_status.intent_status_available,
            request_status.entity==frame.entity,request_status.protected_owner,request_status.ready,request_status.native_offboard_flags,request_status.issue);
        if(impact_test::variant()) {
            r.impact_pending=true;r.impact_started=GetTickCount64();r.impact_step=step;
            r.impact_receipt=impact_receipt::capture(requested,request_status,frame.entity);
        }
        if(requested)message(request_status.intent_armed?"BMX crash: bike released; recording the native fall handoff.":
            "BMX crash: native rider fall requested.",true);
        else if(fresh_request_status&&request_status.protected_owner)
            message("BMX crash: rider fall blocked by No Bail protection. Disable No Bail for native falls.",true);
        else message("BMX crash: bike released; rider fall unavailable. Check No Bail and flight protection.",true);
        return;
    }
    if (resumed_test) {
        const auto status = native_chassis::status();
        const bool passed = status.active && !status.parked && step.body_token == r.chassis_pause_frame.token &&
            status.allocation_attempts == r.chassis_pause_status.allocation_attempts && status.mounts == r.chassis_pause_status.mounts;
        finish_chassis_pause_test(passed,passed ? "same pool resumed" : "resumed pool changed");
        if (!passed) { end_ride("BMX pause test could not resume the same body."); return; }
    }
    auto rider_target=step.target_rider_root,rider_velocity=step.velocity;
    auto rider_rotation=step.observed_quaternion;
    mount_transition::Sample mount_sample;
    if(!r.rehearsal) {
        if(step.body_token!=r.mount_entry_token) {end_ride("BMX mount body ownership changed.");return;}
        mount_sample=r.mount_entry.sample(step.target_rider_root,step.observed_quaternion,seconds);
        if(!mount_sample.ready) {end_ride("BMX mount canceled. Stand close beside the bike on steady ground before getting on.");return;}
        r.mount_blend=mount_sample.progress;
        rider_target=mount_sample.root;rider_velocity=mount_sample.velocity;rider_rotation=mount_sample.rotation;
        step.frame.rider_blend=mount_sample.blend;
        if(mount_sample.progress<1) {
            step.frame.mount_token=r.mount_entry_token;step.frame.mount_progress=mount_sample.progress;
            step.frame.mount_from_left=mount_sample.from_left;
        }
    }
    if (!submit_native_chassis_motion(frame, rider_velocity, rider_target, rider_rotation)) {
        end_ride("BMX rider could not follow the physical chassis."); return;
    }
    if(mount_sample.ready&&!r.mount_entry_began) {
        r.mount_entry_began=true;
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX natural mount begin: token={}; duration={}s; root distance={}m; rotation delta={}rad.",
            r.mount_entry_token,mount_sample.duration,mount_sample.initial_distance,mount_sample.initial_angle);
    }
    if(mount_sample.ready&&mount_sample.progress==1&&!r.mount_entry_completed) {
        r.mount_entry_completed=true;
        logging::log(logging::Level::info,logging::Channel::skater,
            "BMX natural mount complete: token={}; duration={}s; serial={}; native time={}; observed root error={}m.",
            r.mount_entry_token,mount_sample.duration,step.serial,step.native_simulation_seconds,
            mount_transition::length(mount_transition::sub(frame.position,mount_sample.root)));
        message(std::string(mounted_controls),true);
    }
    r.chassis_step = step;
    if (!r.chassis_com_known) {
        r.chassis_initial_com = step.observed_com; r.chassis_pre_hop_y = step.observed_com[1]; r.chassis_com_known = true;
    }
    DingoSDKOverlaySetBmxInputCapture(true); r.observed_capture.store(true);
    r.world_frame = step.frame; visual::publish(step.frame); r.velocity = step.velocity;
    if (seconds <= 0) return;
    double error_squared = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const double error = step.target_rider_root[i] - frame.position[i];
        error_squared += error * error;
    }
    r.chassis_tracking_error = std::max(r.chassis_tracking_error, std::sqrt(error_squared));
    if (r.rehearsal) {
        r.rehearsal_seconds += seconds;
        const auto elapsed = r.rehearsal_seconds;
        if (r.ramp_rehearsal) r.ramp_driver.observe({step.ready,step.grounded,elapsed,seconds,
            step.frame.position,step.observed_com,step.velocity,step.landing_body_alignment,
            step.landing_support_alignment,step.serial,step.body_token,step.actual_wheel_support,step.force,step.torque,
            step.air_damping_torque,step.angular_velocity,
            {step.wheel_contacts_sampled&&!step.contact_hit[0],step.wheel_contacts_sampled&&!step.contact_hit[1]},
            step.neutral_air_damping_active,step.air_rotation_reserved,step.hop_applied,
            step.air_control_torque,step.motor_torque,step.contact_torque,step.balance_torque});
        const auto progress_now = GetTickCount64();
        if(r.catalog_rehearsal) {
            const auto rendered=rider::metrics();
            // Count only distinct completed native render writes, never the
            // published target or a count delta whose intervening poses are unknown.
            if(rendered.render_count>r.catalog_last_render_count) {
                r.catalog_last_render_count=rendered.render_count;
                if(rendered.render_ready && rendered.render_entity==r.entity &&
                    rendered.rider_mode==visual::RiderMode::riding &&
                    rendered.movement_id==catalog::TrickId::superman &&
                    progress_now>=rendered.render_observed_ms &&
                    progress_now-rendered.render_observed_ms<=250) {
                    r.catalog_max_render_phase=std::max(r.catalog_max_render_phase,rendered.movement_phase);
                    if(rendered.movement_phase>=.9f)++r.catalog_peak_render_poses;
                    if(rendered.observed_requested)++r.catalog_observed_requested_writes;
                    if(rendered.observed_applied) {
                        if(rendered.observed_requested&&std::isfinite(rendered.observed_strength)&&
                            rendered.observed_strength>0&&rendered.observed_strength<=1) {
                            if(!r.catalog_observed_applied_writes)
                                r.catalog_observed_min_strength=r.catalog_observed_max_strength=rendered.observed_strength;
                            else {
                                r.catalog_observed_min_strength=std::min(r.catalog_observed_min_strength,rendered.observed_strength);
                                r.catalog_observed_max_strength=std::max(r.catalog_observed_max_strength,rendered.observed_strength);
                            }
                            ++r.catalog_observed_applied_writes;
                        } else ++r.catalog_observed_invalid_writes;
                    }
                }
            }
        }
        if (progress_now >= r.chassis_next_progress) {
            r.chassis_next_progress = progress_now+2000;
            logging::log(logging::Level::info,logging::Channel::skater,
                "BMX physical rehearsal progress: native={}s; wall={}ms; serial={}; applications={}; grounded={}; contact scale={}; bridge peak={}ms; world peak={}ms.",
                elapsed,progress_now-r.rehearsal_started,step.serial,step.native_applications,step.grounded,
                step.contact_force_scale,r.chassis_bridge_peak_ms,r.chassis_world_peak_ms);
        }
        r.chassis_min_contact_scale = std::min(r.chassis_min_contact_scale,static_cast<double>(step.contact_force_scale));
        r.trick_bar_release = std::max(r.trick_bar_release,step.tricks.bar_release);
        r.trick_tail_release = std::max(r.trick_tail_release,step.tricks.tail_release);
        if (step.trick_events.bar_caught) ++r.trick_bar_catches;
        if (step.trick_events.tail_caught) ++r.trick_tail_catches;
        r.trick_pitch_command = std::max(r.trick_pitch_command,step.actions.air_pitch);
        if (!step.grounded && elapsed >= 6.65) {
            const auto axis = chassis::rotate_unit(step.observed_quaternion,{1,0,0});
            double torque = 0,rate = 0;
            for (unsigned i=0;i<3;++i) { torque += axis[i]*step.torque[i]; rate += axis[i]*step.angular_velocity[i]; }
            r.trick_pitch_torque = std::max(r.trick_pitch_torque,torque); r.trick_pitch_rate = std::max(r.trick_pitch_rate,rate);
            if (!r.trick_pitch_serial && step.actions.air_pitch > .1) {
                r.trick_pitch_serial = step.serial; r.trick_pitch_axis = axis; r.trick_pitch_before_rate = rate;
            } else if (r.trick_pitch_serial && step.serial == r.trick_pitch_serial+1) {
                double observed_rate = 0;
                for (unsigned i=0;i<3;++i) observed_rate += r.trick_pitch_axis[i]*step.angular_velocity[i];
                r.trick_pitch_response = observed_rate-r.trick_pitch_before_rate;
                r.trick_pitch_response_serial = step.serial;
            }
        }
        r.chassis_distance = std::max(r.chassis_distance, static_cast<double>(std::hypot(
            step.observed_com[0]-r.chassis_initial_com[0],step.observed_com[2]-r.chassis_initial_com[2])));
        const double com_speed = std::hypot(step.velocity[0],step.velocity[2]);
        if (elapsed >= 4.5 && elapsed < 5) { r.chassis_before_brake = std::max(r.chassis_before_brake,com_speed); r.chassis_before_measured = true; }
        if (elapsed >= 5.8 && elapsed < 6) { r.chassis_after_brake = std::min(r.chassis_after_brake,com_speed); r.chassis_after_measured = true; }
        if (elapsed >= 8.7) { r.chassis_final_speed = com_speed; r.chassis_final_measured = true; }
        if (elapsed < 6.3 && step.grounded) r.chassis_pre_hop_y = step.observed_com[1];
        if (elapsed >= 6.35 && step.hop_events) {
            r.chassis_hop_rise = std::max(r.chassis_hop_rise,step.observed_com[1]-r.chassis_pre_hop_y);
            r.chassis_takeoff_speed = std::max(r.chassis_takeoff_speed,static_cast<double>(step.velocity[1]));
        }
        const double native_speed = std::hypot(frame.position[0] - r.previous_position[0], frame.position[2] - r.previous_position[2]) / seconds;
        r.previous_position = frame.position;
        if (elapsed >= 4.5 && elapsed < 5) {
            r.before_brake_speed = std::max(r.before_brake_speed, native_speed); r.before_brake_measured = true;
        }
        if (elapsed >= 5.8 && elapsed < 6) {
            r.after_brake_speed = std::min(r.after_brake_speed, native_speed); r.after_brake_measured = true;
        }
        if (elapsed >= 8.7) { r.final_speed = native_speed; r.final_speed_measured = true; }
        if (!input_focused) { r.background_input_used = true; r.background_seconds += seconds; }
        r.max_distance = std::max(r.max_distance, static_cast<double>(std::hypot(frame.position[0] - r.initial_position[0], frame.position[2] - r.initial_position[2])));
        if (step.grounded && step.has_ground) { r.chassis_ground_height = step.ground_height; r.chassis_ground_known = true; }
        // The flat-ground rehearsal records height above its last verified tire
        // support. This measurement does not feed a force or correct bike height.
        if (r.chassis_ground_known) r.max_height = std::max(r.max_height, static_cast<double>(step.frame.position[1] - r.chassis_ground_height));
        const double yaw = std::atan2(frame.transform[8], frame.transform[10]);
        r.max_turn = std::max(r.max_turn, std::abs(std::remainder(yaw - r.initial_yaw, tau)));
        r.right_turn.observe(elapsed, yaw);
        r.observed_bike_turn.observe(elapsed,step.serial,step.observed_quaternion);
        if (r.chassis_samples.size() < 900) r.chassis_samples.emplace_back(Json{
            {"elapsed",elapsed}, {"serial",step.serial}, {"native_time",step.native_simulation_seconds},
            {"body_token",step.body_token},{"producer_world",producer_observation.world},
            {"producer_observation_phase","after_chassis_update_before_rider_motion"},
            {"producer_serial",producer_observation.last_serial},{"producer_applications",producer_observation.applications},
            {"producer_last_force",producer_observation.last_force},{"producer_last_torque",producer_observation.last_torque},
            {"producer_failed",producer_observation.failed},{"producer_rejected",producer_observation.rejected},
            {"producer_duplicate_steps",producer_observation.duplicate_steps},
            {"com",step.observed_com}, {"quaternion",step.observed_quaternion},
            {"bike_root",step.frame.position}, {"rider_root",frame.position}, {"rider_target",step.target_rider_root},
            {"velocity",step.velocity}, {"angular_velocity",step.angular_velocity},
            {"force",step.force}, {"torque",step.torque}, {"hop_impulse",step.hop_impulse},
            {"grounded",step.grounded}, {"wheel_separation",step.wheel_separation},
            {"normal_load",step.normal_load}, {"contact_hit",step.contact_hit},
            {"contact_points",step.contact_points}, {"contact_normals",step.contact_normals},
            {"query_geometry_fresh",step.query_geometry_fresh},{"approach_selected",step.approach_selected},
            {"approach_microseconds",step.approach_microseconds},
            {"approach_normal_selected",step.approach_normal_selected},
            {"contact_compliance_selected",step.contact_compliance_selected},{"contact_compliance_revoked",step.contact_compliance_revoked},
            {"landing_contact_compliance_selected",step.landing_contact_compliance_selected},{"landing_contact_compliance_revoked",step.landing_contact_compliance_revoked},
            {"landing_contact_compliance",landing_contact_compliance_json(step.landing_compliance)},
            {"compliant_normal_mask",step.compliant_normal_mask},{"compliance_attempted_mask",step.compliance_attempted_mask},
            {"compliance_budget_fallback",step.compliance_budget_fallback},
            {"compliant_contacts",Json::array({contact_compliance_json((step.compliance_attempted_mask&1u)!=0,step.compliance[0],step.compliant_predicted_gap[0],step.compliant_deflection_satisfied[0]),
                contact_compliance_json((step.compliance_attempted_mask&2u)!=0,step.compliance[1],step.compliant_predicted_gap[1],step.compliant_deflection_satisfied[1])})},{"approach_normal_attempted",step.approach_normal_attempted},
            {"approach_normal_adopted",step.approach_normal_adopted},{"approach_normal_issue",step.approach_normal_issue},
            {"speculative_normal_mask",step.speculative_normal_mask},{"speculative_issues",step.speculative_issues},
            {"speculative_normal_impulse",step.speculative_normal_impulse},{"speculative_normal_force",step.speculative_normal_force},
            {"speculative_linear_impulse",step.speculative_linear_impulse},{"speculative_angular_impulse",step.speculative_angular_impulse},
            {"queried_surfaces",Json::array({queried_surface_json(step.queried_surfaces[0]),queried_surface_json(step.queried_surfaces[1])})},
            {"approach_witnesses",Json::array({approach_geometry_json(step.approach[0]),approach_geometry_json(step.approach[1])})},
            {"pre_contact_prediction",precontact_prediction_json(step.pre_contact_prediction_available,step.pre_contact_prediction)},
            {"contact_balance_normals",step.contact_balance_normals},{"contact_balance_normal_available",step.contact_balance_normal_available},
            {"balance_reference_up",step.balance_reference_up},{"desired_balance_up",step.desired_balance_up},
            {"balance_torque",step.balance_torque},{"air_control_torque",step.air_control_torque},
            {"air_damping_torque",step.air_damping_torque},{"air_rotation_reserved",step.air_rotation_reserved},
            {"neutral_air_damping_active",step.neutral_air_damping_active},
            {"pump_force",step.pump_force},{"pump_applied",step.pump.applied},
            {"pump_status",ground_pump::name(step.pump.status)},{"pump_acceleration",step.pump.down_acceleration},
            {"pump_automatic_armed",step.pump.automatic_armed},{"pump_direct_seconds",step.pump.direct_seconds},
            {"pump_automatic_remaining",step.pump.automatic_remaining},{"pump_air_seconds",step.pump.air_seconds},
            {"motor_torque",step.motor_torque},{"contact_torque",step.contact_torque},
            {"requested_front_steer",step.requested_front_steer_angle},{"turning_lean",step.turning_lean_angle},
            {"steering_limited",step.steering_limited},
            {"raw_left_stick",std::array<double,2>{raw.left_x,raw.left_y}},
            {"raw_right_stick",std::array<double,2>{raw.right_x,raw.right_y}},
            {"rider_tracking_error",std::sqrt(error_squared)}, {"applications",step.native_applications},
            {"command_submitted",step.command_submitted}, {"hop_events",step.hop_events}});
        if (!r.chassis_samples.empty()) {
            auto& sample = r.chassis_samples.back();
            sample["air_pitch_command"] = step.actions.air_pitch;
            sample["contact_force_scale"] = step.contact_force_scale;
            sample["wheel_opening_speed_mps"] = step.wheel_opening_speed;
            sample["wheel_recovery_velocity_mps"] = step.wheel_recovery_velocity;
            sample["landing_compression"] = step.frame.landing_compression;
            sample["air_extension_blend"] = step.frame.air_extension_blend;
            sample["air_extension_available"] = step.frame.air_extension_available;
            sample["rider_posture_available"] = step.frame.rider_posture_available;
            sample["rider_uphill_blend"] = step.frame.rider_uphill_blend;
            sample["rider_gaze_counterpitch_rad"] = step.frame.rider_gaze_counterpitch;
            sample["residual_available"] = step.impact_sample.residual_available;
            sample["residual_impulse_ns"] = step.impact_sample.residual_impulse_ns;
            sample["balance_support_mask"] = step.balance_support_mask;
            sample["balance_continuation_mask"] = step.balance_continuation_mask;
            sample["contact_rays"] = step.contact_rays;
            sample["contact_features"] = step.contact_features;
            sample["contact_edge_uncertainty_m"] = step.contact_edge_uncertainty;
            auto edge_diagnostics=Json::array();
            for(const auto& d:step.contact_edge_diagnostics) {
                edge_diagnostics.push_back(Json{{"candidate",d.candidate},{"bounded",d.bounded},
                    {"uncertainty_valid",d.uncertainty_valid},{"projection_beyond",d.projection_beyond},
                    {"radial_valid",d.radial_valid},{"witness_attempted",d.witness_attempted},
                    {"witness_hit",d.witness_hit},{"witness_near_boundary",d.witness_near_boundary},
                    {"witness_on_plane",d.witness_on_plane},{"witness_normal_valid",d.witness_normal_valid},
                    {"verified",d.verified},{"bisections",d.bisections},{"uncertainty_m",d.uncertainty_m},
                    {"boundary_error_m",d.boundary_error_m},{"boundary_allowance_m",d.boundary_allowance_m},
                    {"plane_error_m",d.plane_error_m},{"radial_up_alignment",d.radial_up_alignment},
                    {"witness_up_alignment",d.witness_up_alignment},{"inside_point",d.inside_point},
                    {"outside_point",d.outside_point},{"witness_point",d.witness_point},
                    {"radial_from",d.radial_from},{"radial_to",d.radial_to}});
                auto& entry=edge_diagnostics[edge_diagnostics.size()-1];
                entry["original_witness_hit"]=d.original_witness_hit;
                entry["retry_attempted"]=d.retry_attempted;
                entry["retry_interior_hit"]=d.retry_interior_hit;
                entry["retry_interior_on_plane"]=d.retry_interior_on_plane;
                entry["retry_radial_attempted"]=d.retry_radial_attempted;
                entry["retry_inset_m"]=d.retry_inset_m;
                entry["retry_uncertainty_m"]=d.retry_uncertainty_m;
                entry["retry_interior_point"]=d.retry_interior_point;
                entry["refinement_sampled"]=d.refinement_sampled;
                entry["refinement_hit"]=d.refinement_hit;
                entry["refinement_projection_valid"]=d.refinement_projection_valid;
                entry["refinement_rejected"]=d.refinement_rejected;
                entry["refinement_classification"]=d.refinement_classification;
                entry["refinement_signed_plane_error_m"]=d.refinement_signed_plane_error_m;
                entry["refinement_from"]=d.refinement_from;
                entry["refinement_to"]=d.refinement_to;
                entry["refinement_point"]=d.refinement_point;
                entry["refinement_projected_point"]=d.refinement_projected_point;
                entry["refinement_plane_point"]=d.refinement_plane_point;
                entry["refinement_plane_normal"]=d.refinement_plane_normal;
            }
            sample["contact_edge_diagnostics"]=std::move(edge_diagnostics);
            sample["modeled_wheel_angular_speed_rad_s"] = step.wheel_angular_speed;
            sample["desired_support_angular_velocity"] = step.desired_support_angular_velocity;
            sample["landing_body_alignment"] = step.landing_body_alignment;
            sample["landing_support_alignment"] = step.landing_support_alignment;
            sample["landing_closing_speed"] = step.landing_closing_speed;
            sample["actual_support"] = step.actual_support;
            sample["actual_wheel_support"] = step.actual_wheel_support;
            const auto& approach=step.landing_approach;
            sample["landing_approach"] = Json{{"valid",approach.valid},{"first_contact",approach.first_contact},
                {"previous_unsupported_available",approach.approach_available},{"post_available",approach.post_available},
                {"contact_serial",approach.contact_serial},{"contact_time",approach.contact_time},
                {"previous_age_at_contact_seconds",approach.approach_available?Json(approach.approach_age_seconds):Json{}},
                {"previous_sample_serial",approach.approach_available?Json(approach.approach.serial):Json{}},
                {"previous_sample_time",approach.approach_available?Json(approach.approach.simulation_time):Json{}},
                {"previous_com",approach.approach_available?Json(approach.approach.com):Json{}},
                {"previous_linear_velocity",approach.approach_available?Json(approach.approach.linear_velocity):Json{}},
                {"previous_angular_velocity",approach.approach_available?Json(approach.approach.angular_velocity):Json{}},
                {"previous_body_heading_available",approach.approach_available&&approach.approach.body_heading_available},
                {"previous_body_yaw",approach.approach_available&&approach.approach.body_heading_available?Json(approach.approach.body_yaw):Json{}},
                {"previous_point_closing_speed",approach.approach_available?Json(approach.observed_previous_closing_speed):Json{}},
                {"gravity_point_speed_estimate_available",approach.gravity_estimate_available},
                {"gravity_estimate_acceleration",approach.gravity_estimate_available?Json(approach.estimate_gravity):Json{}},
                {"gravity_estimated_closing_speed",approach.gravity_estimate_available?Json(approach.gravity_estimated_closing_speed):Json{}},
                {"first_contact_post_closing_speed",approach.post_available?Json(approach.first_contact_post_closing_speed):Json{}},
                {"post_closing_speed",approach.post_available?Json(approach.post_closing_speed):Json{}},
                {"scoring_closing_speed",approach.post_available?Json(approach.scoring_closing_speed):Json{}},
                {"previous_travel_heading_available",approach.approach_available&&approach.approach_travel_available},
                {"previous_travel_yaw",approach.approach_available&&approach.approach_travel_available?Json(approach.approach_travel_yaw):Json{}},
                {"post_travel_heading_available",approach.post_available&&approach.post_travel_available},
                {"post_travel_yaw",approach.post_available&&approach.post_travel_available?Json(approach.post_travel_yaw):Json{}}};
            sample["landing_pending"] = step.tricks.landing_pending;
            sample["landing_support_seconds"] = step.tricks.landing_support_seconds;
            sample["landing_gap_seconds"] = step.tricks.landing_gap_seconds;
            if (r.ramp_rehearsal) {
                sample["ramp_progress"] = r.ramp_driver.tally().progress;
                sample["ramp_ballistic_seconds"] = r.ramp_driver.tally().ballistic_seconds;
                sample["ramp_maximum_ballistic_seconds"] = r.ramp_driver.tally().maximum_ballistic_seconds;
                sample["ramp_natural_flight_seconds"] = r.ramp_driver.tally().natural_flight_seconds;
                sample["ramp_maximum_natural_flight_seconds"] = r.ramp_driver.tally().maximum_natural_flight_seconds;
                sample["ramp_passive_angular_intervals"] = r.ramp_driver.tally().passive_angular_intervals;
                sample["ramp_ballistic_velocity_error_mps"] = r.ramp_driver.tally().ballistic_velocity_error;
                sample["ramp_ascent"] = r.ramp_driver.tally().ascent;
                sample["ramp_flight"] = r.ramp_driver.tally().flight;
                sample["ramp_landed"] = r.ramp_driver.tally().landed;
            }
            sample["raw_left_x"] = raw.left_x; sample["raw_left_y"] = raw.left_y;
            sample["raw_right_x"] = raw.right_x; sample["raw_right_y"] = raw.right_y;
            sample["raw_pedal"] = raw.pedal; sample["raw_brake"] = raw.brake;
            sample["raw_left_trigger"]=raw.left_trigger;sample["raw_right_trigger"]=raw.right_trigger;
            sample["raw_left_shoulder"]=raw.left_shoulder;sample["raw_right_shoulder"]=raw.right_shoulder;
            sample["bar_release"] = step.tricks.bar_release; sample["tail_release"] = step.tricks.tail_release;
            sample["bar_angle"] = step.tricks.bar_angle; sample["tail_angle"] = step.tricks.tail_angle;
            sample["bar_caught_event"] = step.trick_events.bar_caught; sample["tail_caught_event"] = step.trick_events.tail_caught;
            sample["combo_landed_event"] = step.trick_events.combo_landed;
            sample["landed_barspins"] = step.tricks.tally.landed_barspins;
            sample["landed_tailwhips"] = step.tricks.tally.landed_tailwhips;
            sample["landed_combos"] = step.tricks.tally.landed_combos;
            sample["failed_combos"] = step.tricks.tally.failed_combos;
            sample["canceled_combos"] = step.tricks.tally.canceled_combos;
            sample["landing_hold_active"] = step.tricks.landing_hold_active;
            sample["landing_hold_elapsed_seconds"] = step.tricks.landing_hold_elapsed_seconds;
            sample["movement_id"] = static_cast<unsigned>(step.tricks.movement_id);
            sample["movement_phase"] = step.tricks.movement_phase;
            sample["movement_progress"] = step.tricks.movement_progress;
            sample["movement_caught_event"] = step.trick_events.movement_caught;
            sample["pending_movements"] = step.tricks.pending_movements;
            sample["landed_movements"] = step.tricks.tally.landed_movements;
            sample["flip_radians"] = step.tricks.flip_radians;
            sample["roll_radians"] = step.tricks.roll_radians;
            sample["landed_front_flips"] = step.tricks.tally.landed_front_flips;
            sample["landed_back_flips"] = step.tricks.tally.landed_back_flips;
            sample["landed_barrel_rolls"] = step.tricks.tally.landed_barrel_rolls;
            sample["trick_failure"] = std::string(trick_failure_name(step.tricks.failure));
        }
        if (elapsed >= selftest::simulation_duration) {
            finish_rehearsal(true);
            end_ride("BMX physical chassis rehearsal finished. See bmx-selftest.json in the isolated data folder."); return;
        }
    }
    if (r.mount_blend < 1) {
        message("BMX getting on the bike.");
        return;
    }
    std::ostringstream info; info.precision(2);
    info << std::fixed << "BMX mounted: " << std::abs(step.speed) * 3.6 << " km/h; "
        << (step.grounded ? "grounded" : "airborne") << ". Physical chassis; left stick handlebars, right stick rear tricks.";
    message(info.str());
}
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "bmx_native_coupled_lab.inc"
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
#include "bmx_native_coupled_force_lab.inc"
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED) && defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
#include "bmx_coupled_avatar_entry.inc"
#include "bmx_coupled_ride_private.inc"
#endif
}
bool request_quick_switch_test() noexcept {
    if(!quick_switch_rehearsal::enabled())return false;
    runtime().request.store(11);return true;
}
void request_mount(bool mount) noexcept { runtime().request.store(mount ? 1 : -1); }
void request_toggle() noexcept { runtime().request.store(4); }
void request_interaction(bool carry) noexcept {runtime().request.store(carry?8:7);}
void request_native_fall_observation() noexcept {runtime().request.store(9);}
bool request_getup_test() noexcept {
    if(!impact_test::getup_enabled())return false;
    runtime().request.store(10);return true;
}
bool request_selftest(bool unattended) noexcept {
    try {
        if (!user_data::development_requested() || !user_data::override_root()) return false;
        runtime().request.store(unattended ? 3 : 2); return true;
    } catch (...) { return false; }
}
bool request_chassis_pause_test() noexcept {
    if (!chassis_ride::enabled() || !runtime().active.load()) return false;
    runtime().request.store(5); return true;
}
bool mounted() noexcept { return runtime().active.load(); }
std::string status() {
    auto& r = runtime();
    const auto world_model = world::snapshot();
    const auto physical = native_chassis::status();
    const auto basis=native_motion_basis_status();
    std::lock_guard lock(r.status_mutex);
    return r.message + " Native velocity updates: " + std::to_string(native_motion_updates()) +
        "; native bike parts: " + std::to_string(world_model.live_parts) + "/" + std::to_string(world::part_count) + "; placements: " +
        std::to_string(world_model.entity_placements) + "; " + world_model.issue +
        "; native physics state: " + (r.observed_physics_valid.load() ? std::to_string(r.observed_physics.load()) : "unavailable") +
        "; BMX capture: " + (r.observed_capture.load() ? "held" : "released") +
        "; chassis mounts=" + std::to_string(physical.mounts) + "; allocations=" + std::to_string(physical.allocation_attempts) +
        "; active=" + (physical.active ? "true" : "false") + "; parked=" + (physical.parked ? "true" : "false") +
        "; failed=" + (physical.failed ? "true" : "false") + "; blocked=" + (physical.blocked ? "true" : "false") +
        "; observation suspended=" + (physical.observation_suspended ? "true" : "false") +
        "; deferred park=" + (physical.deferred_park ? "true" : "false") + "; chassis issue=" + physical.issue +
        "; force applications=" + std::to_string(physical.applications) +
        "; body world=" + std::to_string(physical.world) +
        "; rider pose updates=" + std::to_string(rider::updates()) + r.interaction_status + ". " + rider::status()+
        "; motion basis="+basis.mode+", basis scope="+(basis.qualified_scope?"true":"false")+
        ", basis refused="+(basis.refused?"true":"false")+", selected full rotation targets="+std::to_string(basis.selected_full_rotation_targets)+
        ", preserved supplied bases="+std::to_string(basis.preserved_supplied_bases)+
        (overlay::detail::human_native_handoff_requested()?"; human native switch="+overlay::detail::human_native_handoff_status():"");
}
std::string source_status() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto client = runtime().observed_client.load();
    const auto inventory = skater::read_client_skater_sources(client, skater::SourceInventoryLayout{
        base + addr::engine::client_vtable, base + addr::engine::context_partition_offset,
        base + addr::engine::source_managers, base + addr::engine::source_manager_vtable,
        base + addr::engine::client_skater_source_vtable, base + addr::engine::skater_entity_vtable},
        [](std::uintptr_t address, void* target, std::size_t size) { return memory::peek_bytes(address, target, size); });
    std::ostringstream out;
    out << "Native source inventory: " << (inventory.available ? "available" : "unavailable") << "; " << inventory.issue
        << "; managers=" << inventory.managers << "; entries=" << inventory.entries
        << "; sources=" << inventory.sources.size();
    unsigned shown{};
    for (const auto& source : inventory.sources) {
        if (++shown > 16) break;
        out << "\n  " << source.blueprint_name << "; entity=" << (source.entity ? "present" : "missing")
            << "; source_local_id=" << static_cast<unsigned>(source.source_local_id) << "; context=" << source.context_matches
            << "; parent_context=" << source.parent_context_matches << "; data flags=";
        for (const auto flag : source.data_flags_e0) out << static_cast<unsigned>(flag) << ',';
    }
    out << '\n' << ecs_startup_diagnostics(base, client);
    return out.str();
}
void stop() noexcept {
#if defined(DINGOSDK_BMX_NATIVE_COUPLED) && defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    if(coupled_ride_private::selected())coupled_ride_private::request_stop();
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
    if(coupled_force_lab::selected())coupled_force_lab::request_stop();
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
    if(coupled_lab::selected())coupled_lab::request_stop();
#endif
    stop_player_startup();
    try {
        queue_fall_lifecycle(fall_lifecycle::Reason::runtime_stopped);
        intent_observer::cancel_getup_observation();
        end_ride("BMX dismounted.");
    } catch (...) { release_native_motion(); DingoSDKOverlaySetBmxInputCapture(false); }
}
void poll_private_native_input_handoff(std::uintptr_t base,std::uintptr_t client) noexcept {
    auto& r=runtime();if(!r.private_native_input_started)return;
    using namespace native_input_handoff;
    const auto phase=overlay::detail::private_bmx_native_input_handoff_phase();
    if(phase==Phase::complete||phase==Phase::aborted) {
        r.quick_test.finish_private_native_input_prefix(phase==Phase::complete,
            phase==Phase::complete?"private_native_input_prefix_103_100_dwell_not_visual_quality":"private_native_input_handoff_aborted");
        return;
    }
    const auto frame=read_native_frame(base,client);const auto body=native_chassis::released_ownership_status();
    const auto physical=native_chassis::status();const auto publication=rider::private_handoff_publication();
    const auto motion=native_motion_lease_status();const auto mp=multiplayer::model();
    Sample s;s.now=GetTickCount64();s.sampled_ms=r.observed_input.sampled_ms;
    s.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
    s.body={body.world,body.native_state,body.body_index,body.body_generation,body.token};
    s.counters={native_motion_updates(),publication.poses,publication.renders,publication.board_hides,
        physical.applications,physical.allocation_attempts,physical.mounts};
    s.serial=frame.serial;s.native_time=frame.simulation_time;s.offboard=frame.offboard;s.standable=frame.standable;s.motion_supported=frame.motion_supported;
    s.configuration=quick_switch_rehearsal::enabled()&&board_handoff_trace::selected();
    const auto& input=r.observed_input;
    s.gates=r.native_board_control_input_allowed&&r.owner_gameplay_ready&&!mp.active&&!mp.lobby_joining&&input.available&&input.focused&&input.overlay_ready&&
        !input.menu_blocked&&!input.menu_requested&&!input.capture_blocked;
    s.sdk_device=input.keyboard?0:input.device;
    s.sdk_quiet=!input.buttons&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&!input.quick_hop&&
        std::abs(input.left_x)<=.05f&&std::abs(input.left_y)<=.05f&&std::abs(input.right_x)<=.05f&&std::abs(input.right_y)<=.05f&&
        input.pedal<=.05f&&input.brake<=.05f&&input.left_trigger<=.05f&&input.right_trigger<=.05f;
    s.gesture_released=!input.dismount_requested&&!input.left_shoulder;
    s.sdk_idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_carried&&!r.bike_crashed&&
        !r.chassis_started&&!r.step_off_board.active()&&!r.board_switch.active()&&!r.setdown_search.active()&&!r.rehearsal;
    s.publication_inactive=publication.inactive;s.parts_zero=world::snapshot().live_parts==0;
    s.capture_released=!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&!input.capturing;
    s.motion_released=motion.available&&!motion.active;
    s.pool_released=body.available&&body.ready&&!body.active&&!body.detached&&!body.lease_active&&!body.failed&&
        physical.parked&&!physical.active&&!physical.failed&&!physical.blocked&&!physical.observation_suspended&&
        !physical.deferred_park&&!physical.owner_quarantined&&
        native_chassis::parked_owner_matches(base,frame,r.private_native_input_ticket.body.world,r.private_native_input_ticket.body.token,false);
    NoBailSkater canonical;bool protected_now{};
    s.canonical=frame.available&&inspect_no_bail_owner(client,frame.entity,canonical,protected_now)&&
        canonical.base==base&&canonical.entity==frame.entity&&canonical.core==frame.core&&canonical.context==frame.context&&canonical.rig==frame.rig;
    s.protected_owner=protected_now;
    std::uint64_t serial{};double time{};
    s.clock_coherent=frame.available&&frame.timing_available&&read_native_clock_try(frame.entity,frame.core,serial,time)&&
        serial==frame.serial&&time==frame.simulation_time&&memory::peek(frame.context+0x1414,s.physics);
    overlay::detail::update_private_bmx_native_input_handoff(s);
    if(phase==Phase::observing&&overlay::detail::private_bmx_native_input_handoff_phase()==Phase::complete&&
       !r.private_native_input_terminal.requested) {
        // update takes Sample by value and never changes these tuple fields.
        // This is the exact current Sample frozen by Session::observe, never a
        // replacement clock borrowed from a different retained trace row.
        r.private_native_input_terminal={true,s.owner,{s.serial,s.native_time},s.physics,s.now,r.private_native_input_ticket.deadline_ms};
    }
}
void poll_human_native_input_handoff(std::uintptr_t base,std::uintptr_t client) noexcept {
    auto& r=runtime();if(!r.human_native_input_pending)return;
    using namespace native_input_handoff;
    if(!overlay::detail::human_native_handoff_active()) {
        r.human_native_input_pending=false;
        if(r.step_off_board.active())cancel_step_off_board("Native switch canceled; bike remains beside you.");
        try {
            const auto status=overlay::detail::human_native_handoff_status();
            logging::write(logging::Level::info,logging::Channel::skater,status);
            const auto root=user_data::override_root();
            if(root&&r.human_native_reports<16) {
                ++r.human_native_reports; // Bound attempts, including failed writes.
                const auto detail=Json::parse(status);const auto generation=detail.at("generation").get<std::uint64_t>();
                const auto name="bmx-human-native-switch-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(generation)+".json";
                std::ofstream output(*root/name,std::ios::trunc);
                output<<Json{{"schema",1},{"process_id",GetCurrentProcessId()},{"completed",true},
                    {"native_handoff_complete",overlay::detail::human_native_handoff_complete()},
                    {"report_drops",r.human_native_report_drops},{"handoff",detail}}.dump(2)<<'\n';output.flush();
                if(!output.good())++r.human_native_report_drops;
            }else ++r.human_native_report_drops;
        }catch(...){++r.human_native_report_drops;}
        message(overlay::detail::human_native_handoff_complete()?"Native skateboard equipped. LB/L1 + Y/Triangle: bike.":
            "Native skateboard switch canceled. On foot; no automatic retry. See BMX status.",true);return;
    }
    const auto frame=read_native_frame(base,client);const auto body=native_chassis::released_ownership_status();
    const auto physical=native_chassis::status();const auto publication=rider::private_handoff_publication();
    const auto motion=native_motion_lease_status();const auto mp=multiplayer::model();
    Sample s;s.now=GetTickCount64();s.sampled_ms=r.observed_input.sampled_ms;
    s.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
    s.body={body.world,body.native_state,body.body_index,body.body_generation,body.token};
    s.counters={native_motion_updates(),publication.poses,publication.renders,publication.board_hides,
        physical.applications,physical.allocation_attempts,physical.mounts};
    s.serial=frame.serial;s.native_time=frame.simulation_time;s.offboard=frame.offboard;s.standable=frame.standable;s.motion_supported=frame.motion_supported;
    s.configuration=overlay::detail::human_native_handoff_requested();
    const auto& input=r.observed_input;
    s.gates=r.owner_gameplay_ready&&!mp.active&&!mp.lobby_joining&&input.available&&input.focused&&input.overlay_ready&&
        !input.menu_blocked&&!input.menu_requested&&!input.capture_blocked;
    s.sdk_device=input.keyboard?0:input.device;
    s.sdk_quiet=!input.buttons&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&!input.quick_hop&&
        std::abs(input.left_x)<=.05f&&std::abs(input.left_y)<=.05f&&std::abs(input.right_x)<=.05f&&std::abs(input.right_y)<=.05f&&
        input.pedal<=.05f&&input.brake<=.05f&&input.left_trigger<=.05f&&input.right_trigger<=.05f;
    s.gesture_released=!input.dismount_requested&&!input.left_shoulder;
    s.sdk_idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_carried&&!r.bike_crashed&&
        !r.chassis_started&&!r.step_off_board.active()&&!r.board_switch.active()&&!r.setdown_search.active()&&!r.rehearsal;
    s.publication_inactive=publication.inactive;s.parts_zero=world::snapshot().live_parts==0;
    s.capture_released=!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&!input.capturing;
    s.motion_released=motion.available&&!motion.active;
    s.pool_released=body.available&&body.ready&&!body.active&&!body.detached&&!body.lease_active&&!body.failed&&
        physical.parked&&!physical.active&&!physical.failed&&!physical.blocked&&!physical.observation_suspended&&
        !physical.deferred_park&&!physical.owner_quarantined&&
        native_chassis::parked_owner_matches(base,frame,r.human_native_input_ticket.body.world,r.human_native_input_ticket.body.token,false);
    NoBailSkater canonical;bool protected_now{};
    s.canonical=frame.available&&inspect_no_bail_owner(client,frame.entity,canonical,protected_now)&&
        canonical.base==base&&canonical.entity==frame.entity&&canonical.core==frame.core&&canonical.context==frame.context&&canonical.rig==frame.rig;
    s.protected_owner=protected_now;
    std::uint64_t serial{};double time{};
    s.clock_coherent=frame.available&&frame.timing_available&&read_native_clock_try(frame.entity,frame.core,serial,time)&&
        serial==frame.serial&&time==frame.simulation_time&&memory::peek(frame.context+0x1414,s.physics);
    human_native_handoff::Guard guard{s.now,s.sampled_ms,s.owner,s.sdk_device,s.gates&&s.configuration&&s.canonical&&!s.protected_owner&&s.clock_coherent,
        (input.buttons&~0x8100u)==0&&std::abs(input.left_x)<=.05f&&std::abs(input.left_y)<=.05f&&
        std::abs(input.right_x)<=.05f&&std::abs(input.right_y)<=.05f&&input.pedal<=.05f&&input.brake<=.05f&&
        input.left_trigger<=.05f&&input.right_trigger<=.05f,s.gesture_released};
    if(!r.human_native_input_ticket.serial) {
        if(!r.step_off_board.active()||!r.bike_parked)overlay::detail::cancel_human_native_handoff("prepared_step_off_no_longer_active");
        else overlay::detail::guard_human_native_handoff(guard);
    }
    else overlay::detail::update_human_native_handoff(s,guard);
}

native_input_handoff::Sample human_native_return_sample(std::uintptr_t base,std::uintptr_t client,const NativeFrame& frame) noexcept {
    using namespace native_input_handoff;
    auto& r=runtime();const auto body=native_chassis::released_ownership_status();
    const auto physical=native_chassis::status();const auto publication=rider::private_handoff_publication();
    const auto motion=native_motion_lease_status();const auto mp=multiplayer::model();
    Sample s;s.now=GetTickCount64();s.sampled_ms=r.observed_input.sampled_ms;
    s.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
    s.body={body.world,body.native_state,body.body_index,body.body_generation,body.token};
    s.counters={native_motion_updates(),publication.poses,publication.renders,publication.board_hides,
        physical.applications,physical.allocation_attempts,physical.mounts};
    s.serial=frame.serial;s.native_time=frame.simulation_time;s.offboard=frame.offboard;s.standable=frame.standable;s.motion_supported=frame.motion_supported;
    s.configuration=overlay::detail::human_native_handoff_requested();
    const auto& input=r.observed_input;
    s.gates=r.owner_gameplay_ready&&!mp.active&&!mp.lobby_joining&&input.available&&input.focused&&input.overlay_ready&&
        !input.menu_blocked&&!input.menu_requested&&!input.capture_blocked;
    s.sdk_device=input.keyboard?0:input.device;
    s.sdk_quiet=!input.buttons&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&!input.quick_hop&&
        std::abs(input.left_x)<=.05f&&std::abs(input.left_y)<=.05f&&std::abs(input.right_x)<=.05f&&std::abs(input.right_y)<=.05f&&
        input.pedal<=.05f&&input.brake<=.05f&&input.left_trigger<=.05f&&input.right_trigger<=.05f;
    s.gesture_released=!input.dismount_requested&&!input.left_shoulder;
    s.sdk_idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_carried&&!r.bike_crashed&&
        !r.chassis_started&&!r.step_off_board.active()&&!r.board_switch.active()&&!r.setdown_search.active()&&!r.rehearsal;
    s.publication_inactive=publication.inactive;s.parts_zero=world::snapshot().live_parts==0;
    s.capture_released=!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&!input.capturing;
    s.motion_released=motion.available&&!motion.active;
    s.pool_released=body.available&&body.ready&&!body.active&&!body.detached&&!body.lease_active&&!body.failed&&
        physical.parked&&!physical.active&&!physical.failed&&!physical.blocked&&!physical.observation_suspended&&
        !physical.deferred_park&&!physical.owner_quarantined&&
        native_chassis::parked_owner_matches(base,frame,body.world,body.token,false);
    NoBailSkater canonical;bool protected_now{};
    s.canonical=frame.available&&inspect_no_bail_owner(client,frame.entity,canonical,protected_now)&&
        canonical.base==base&&canonical.entity==frame.entity&&canonical.core==frame.core&&canonical.context==frame.context&&canonical.rig==frame.rig;
    s.protected_owner=protected_now;
    std::uint64_t serial{};double time{};
    s.clock_coherent=frame.available&&frame.timing_available&&memory::peek(frame.context+0x1414,s.physics)&&
        read_native_clock_try(frame.entity,frame.core,serial,time)&&serial==frame.serial&&time==frame.simulation_time;
    return s;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept {
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
    if(coupled_lab::selected()){coupled_lab::tick(base,client,gameplay_ready);return;}
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED)
    if(coupled_force_lab::selected()){coupled_force_lab::tick(base,client,gameplay_ready);return;}
#endif
#if defined(DINGOSDK_BMX_NATIVE_COUPLED) && defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    if(coupled_ride_private::selected()){coupled_ride_private::tick(base,client,gameplay_ready);return;}
#endif
    auto& r = runtime();
    r.human_native_return_tick_visited=false;
    r.quick_test_tick_visited=false;
    r.native_board_control_input_allowed=false;
    r.owner_unwind.begin_tick();r.owner_gameplay_ready=gameplay_ready;
    r.world_frame = {};
    visual::begin_client_tick();
    struct WorldTick {
        std::uintptr_t base{}, client{};
        Runtime& state;
        ~WorldTick() {
            if(state.human_native_return_pending&&!state.human_native_return_tick_visited)
                overlay::detail::cancel_human_native_return("client_tick_ended_before_reverse_source_gate");
            if(state.quick_preflight.started()&&!state.quick_test.terminal()&&!state.quick_test_tick_visited) {
                state.quick_preflight.fail("preflight_client_tick_ended_before_native_source_gate");
                state.quick_test.fail("client_tick_ended_before_native_source_gate");
                retain_quick_test_rejection(state,"client_tick_ended_before_native_source_gate",false);
            }
            report_quick_test();
            const auto started = std::chrono::steady_clock::now();
            const auto publication=visual::pending_placement();
            const auto placed=world::tick(base,client,publication.frame,publication.revision);
            (void)visual::complete_placement(placed.revision,placed.entity,placed.complete);
            poll_private_native_input_handoff(base,client);
            poll_human_native_input_handoff(base,client);
            if(board_handoff_trace::initialize_client(base)||board_handoff_trace::store.enabled.load(std::memory_order_acquire)) {
                board_handoff_trace::ClientState observed;
                observed.sampled_ms=state.observed_input.sampled_ms;
                observed.native_control_input_allowed=state.native_board_control_input_allowed;
                observed.motion_updates=native_motion_updates();observed.pose_updates=rider::updates();observed.render_updates=rider::render_updates();
                observed.parts=world::snapshot().live_parts;observed.gameplay=state.owner_gameplay_ready;
                const auto session=multiplayer::model();observed.solo=!session.active&&!session.lobby_joining;
                observed.focused=state.observed_input.available&&state.observed_input.focused;
                observed.menu=state.observed_input.menu_blocked||state.observed_input.menu_requested;
                observed.capture=overlay::detail::bmx_input_capture_active();observed.active=state.active.load();observed.pending=state.pending;
                observed.stepping_off=state.stepping_off;observed.board_pending=state.board_switch.active();
                observed.private_terminal=state.private_native_input_terminal;
                rider::poll_private_board_handoff(base,!state.active.load()&&!state.pending&&!state.stepping_off,observed);
            }
            const auto bike_mode=state.active.load()?
                (state.native_chassis_mode&&(!state.chassis_started||state.mount_blend<1)?"mounting":"mounted"):
                state.stepping_off?"stepping_off":
                state.bike_carried?"carrying":state.bike_crashed?"crashed":state.bike_parked?"parked":"none";
            const auto rider_mode=!state.world_frame.active?"none":
                state.world_frame.rider_mode==visual::RiderMode::carrying?"carrying":
                state.world_frame.rider_mode==visual::RiderMode::riding?"riding":"none";
            {
                std::lock_guard lock(state.status_mutex);
                state.interaction_status=std::string("; bike mode=")+bike_mode+"; rider mode="+rider_mode+
                    "; setdown verified="+(state.parked_ground_verified?"true":"false")+
                    "; body token="+std::to_string(state.bike_parked||state.bike_crashed?state.parked_token:
                        state.active.load()?state.chassis_step.body_token:0);
            }
            if (state.rehearsal && state.native_chassis_mode) {
                const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
                state.chassis_world_ms += ms; ++state.chassis_world_calls;
                state.chassis_world_peak_ms = std::max(state.chassis_world_peak_ms,ms);
            }
        }
    } world_tick{base, client, r};
    r.observed_client.store(client);
    tick_player_startup(base, client, gameplay_ready);
    skeleton_phase_probe::poll(!r.active.load()&&!r.pending&&!r.stepping_off);
    try {
        // The original request deadline also covers active admission searches,
        // before any suspension, menu or focus path can postpone their expiry.
        if ((r.pending||(r.native_chassis_mode&&r.active.load()&&!r.chassis_started))&&
            GetTickCount64()>=r.pending_until) {
            std::string last_guard;
            { std::lock_guard lock(r.status_mutex); last_guard = r.message; }
            stop_mount_admission("BMX mount timed out. Last readiness check: " + last_guard); return;
        }
        if(service_retained_owner(base,client))return;
        const auto suspended=native_chassis::poll_suspension(base,client);
        if(suspended==native_chassis::SuspensionPoll::waiting) {
            // SDK-only BMX motion/physics revocation. WorldTick may perform
            // its existing separately guarded native presentation cleanup.
            withdraw_unavailable_bike_observation();return;
        }
        if(suspended==native_chassis::SuspensionPoll::invalidated) {
            (void)owner_observation::reset_interactions(r.request,r.interaction_button,r.toggle_key,r.interact_key,r.carry_key,&r.quick_switch_button);
            DingoSDKOverlaySetBmxQuickSwitchCapture(false);
            r.observation_input_rearm=false;
            end_ride("BMX stopped because suspended physics ownership changed.");return;
        }
        if(suspended==native_chassis::SuspensionPoll::resumed||r.observation_input_rearm) {
            // Also drain a console request arriving after the final waiting
            // drain but before this fresh proof. Spend one fresh quiescent tick
            // without reinstating suspension or admitting an interaction.
            (void)owner_observation::reset_interactions(r.request,r.interaction_button,r.toggle_key,r.interact_key,r.carry_key,&r.quick_switch_button);
            DingoSDKOverlaySetBmxQuickSwitchCapture(false);
            r.observation_input_rearm=false;r.world_frame={};visual::publish({});return;
        }
        bail::poll_observation();
        flush_ride_trace();
        flush_fall_lifecycle(base,client);
        poll_getup_lab_report();
        if(r.impact_pending) {
            const auto fall=bail::status();const auto elapsed=GetTickCount64()-r.impact_started;
            const bool pulse_complete=fall.intent_status_available&&fall.activation_enabled&&fall.intent_prior_false&&fall.intent_written&&
                fall.intent_true_returned&&fall.intent_true_readback&&fall.intent_reset&&fall.intent_reset_readback&&
                !fall.intent_reset_pending&&!fall.intent_owner_expired&&fall.intent_true_calls==1&&
                fall.intent_reset_serial>fall.intent_true_serial&&fall.intent_true_serial>0&&
                ((fall.intent_reset_calls==1&&fall.intent_native_calls==2)||
                 (fall.intent_reset_calls==0&&fall.intent_native_cleared&&fall.intent_native_calls==1));
            const bool same_request=impact_receipt::matches(r.impact_receipt,fall);
            const bool handoff_complete=same_request&&(fall.activation_enabled?pulse_complete:fall.native_calls==1);
            if((fall.ragdoll_observed&&handoff_complete)||fall.expired||fall.owner_changed||fall.intent_owner_expired||elapsed>=8000) {
                const auto root=user_data::override_root();
                if(root&&impact_test::variant()) {
                    const auto& hit=r.impact_step;
                    const auto chassis_diagnostics=native_chassis::status();
                    std::ofstream report(*root/L"bmx-impact-test.json",std::ios::trunc);
                    report<<Json{{"process_id",GetCurrentProcessId()},{"completed",true},
                        {"accepted_request",r.impact_receipt.accepted},{"request_matches",same_request},
                        {"request_id",fall.request_id},{"expected_request_id",r.impact_receipt.request_id},
                        {"expected_generation",r.impact_receipt.generation},
                        {"variant",impact_test::variant()},{"bail",hit.bail.bail},
                        {"reason",static_cast<unsigned>(hit.bail.reason)},{"region",static_cast<unsigned>(hit.bail.region)},
                        {"body_token",hit.body_token},{"serial",hit.serial},{"com",hit.observed_com},
                        {"quaternion",hit.observed_quaternion},{"velocity",hit.velocity},
                        {"body_alignment",hit.bail.alignment},{"closing_speed",hit.bail.closing_speed},
                        {"guard_rays",hit.body_guard_rays},{"native_fall_consumed",fall.consumed},
                        {"native_ragdoll_observed",fall.ragdoll_observed},{"native_calls",fall.native_calls},
                        {"native_ragdoll_mode",fall.native_ragdoll_mode},{"native_offboard_flags",fall.native_offboard_flags},
                        {"intent_armed",fall.intent_armed},{"intent_written",fall.intent_written},{"intent_reset",fall.intent_reset},
                        {"intent_context_verified",fall.context_verified},{"intent_callbacks",fall.intent_callbacks},
                        {"intent_native_calls",fall.intent_native_calls},
                        {"intent_generation",fall.intent_generation},{"intent_status_available",fall.intent_status_available},
                        {"activation_enabled",fall.activation_enabled},{"intent_reset_pending",fall.intent_reset_pending},
                        {"intent_prior_false",fall.intent_prior_false},{"intent_true_returned",fall.intent_true_returned},
                        {"intent_true_readback",fall.intent_true_readback},{"intent_reset_readback",fall.intent_reset_readback},
                        {"intent_native_cleared",fall.intent_native_cleared},{"intent_owner_expired",fall.intent_owner_expired},
                        {"intent_true_serial",fall.intent_true_serial},{"intent_reset_serial",fall.intent_reset_serial},
                        {"intent_true_calls",fall.intent_true_calls},{"intent_reset_calls",fall.intent_reset_calls},
                        {"issue",fall.issue},{"chassis_active",native_chassis::status().active},
                        {"bike_detached",native_chassis::status().detached},{"bike_parts",world::snapshot().live_parts},
                        {"snapshot_reads",chassis_diagnostics.snapshot_reads},{"snapshot_retries",chassis_diagnostics.snapshot_retries},
                        {"snapshot_recoveries",chassis_diagnostics.snapshot_recoveries},
                        {"snapshot_retry_exhaustions",chassis_diagnostics.snapshot_retry_exhaustions},
                        {"last_snapshot_issue",static_cast<unsigned>(chassis_diagnostics.last_snapshot_issue)},
                        {"crash_samples",Json(r.crash_samples)},
                        {"elapsed_ms",elapsed},{"passed",hit.bail.bail&&fall.consumed&&fall.ragdoll_observed&&handoff_complete}}.dump(2)<<'\n';
                }
                r.impact_pending=false;
            }
        }
        // One source sample also supplies the gates for F6 while BMX is idle.
        // The hotkey uses the existing bypass only for one keyboard query; it
        // never polls the gamepad a second time or takes input from a menu.
        const auto ui = multiplayer::read_game_ui_state(base);
        const auto input = overlay::read_bmx_input();
        r.observed_input=input;
        const auto mp = multiplayer::model();
        r.observed_capture.store(input.capturing);
        const bool normal_input = gameplay_ready && !mp.active && !mp.lobby_joining &&
            selftest::input_allowed(selftest::Mode::focused, false,
                {ui.available, ui.in_menu, input.overlay_ready, input.focused, input.menu_blocked,
                 input.capture_blocked, input.menu_requested, input.available});
        r.native_board_control_input_allowed=normal_input&&board_handoff_policy::native_board_input_neutral(
            {input.left_x,input.left_y,input.right_x,input.right_y,input.pedal,input.brake,input.left_trigger,input.right_trigger},
            input.buttons,input.quick_hop,input.dismount_requested,input.left_shoulder,input.right_shoulder);
        if(board_handoff_trace::native_only_control_process()) {
            // Every initialized NativeOnly process bypasses BMX lifecycle and
            // capture, including passive physical-input comparisons. Revocation
            // seals observation while this process stays isolated. The optional
            // software request remains exclusive to its guarded observer.
            if(!board_handoff_trace::selected())board_handoff_trace::seal("private_configuration_changed");
            if(r.request.exchange(0))board_handoff_trace::seal("native_board_control_external_BMX_request");
            return;
        }
        // The private ramp process teleports before acquiring its one chassis
        // pool. A manual mount beforehand would bind that pool to the rider
        // which the native teleport replaces, invalidating the experiment.
        // Ordinary Ride profiles never enable this diagnostic gate.
        const bool ramp_process = ramp_test::enabled();
        bool toggle_down{},interact_down{},carry_down{};
        const bool recovery_process=impact_test::recovery_enabled();
        const bool getup_process=impact_test::getup_enabled();
        const bool quick_test_process=quick_switch_rehearsal::enabled();
        const bool switch_process=automated_switch::enabled()||recovery_process||getup_process;
        if (normal_input && !r.rehearsal && !ramp_process && !switch_process) {
            overlay::begin_input_access();
            toggle_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            interact_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            carry_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            overlay::end_input_access();
        }
        auto request = r.request.exchange(0);
        if(request&&r.human_native_return_pending) {
            overlay::detail::cancel_human_native_return("external_lifecycle_request_during_native_return");request=0;
        }
        if(request&&r.human_native_return_mount) {
            end_ride("Native return mount canceled by an external lifecycle action.");return;
        }
        if(request&&overlay::detail::human_native_admission_waiting()) {
            overlay::detail::cancel_human_native_admission("external_request_during_original_poll_wait");request=0;
        }
        if(r.private_native_input_started&&request) {
            overlay::detail::cancel_private_bmx_native_input_handoff("external_lifecycle_request_during_private_handoff");
            r.quick_test.finish_private_native_input_prefix(false,"external_lifecycle_request_during_private_handoff");
            request=0;
        }
        bool switch_lease_active{};
        if(switch_process) {
            // Only explicit console lifecycle commands may start this window.
            // Manual request generation remains disabled even after expiry.
            if(request && request!=-1 && request!=1 && request!=4 && request!=5 && request!=7 && request!=8 &&
               !((recovery_process||getup_process)&&request==3)&&!(getup_process&&request==10)&&!(quick_test_process&&request==11)) {
                message("Private switch automation accepts only equipment, interaction and pause-test commands.",true);request=0;
            }
            if(request)r.switch_test_lease.begin(GetTickCount64());
            switch_lease_active=r.switch_test_lease.active(GetTickCount64());
            if(r.switch_test_lease.expired()) {
                if(!r.switch_test_expiry_reported) {
                    r.switch_test_expiry_reported=true;
                    end_ride("Private switch automation expired after 200 wall seconds; bike control released.");
                }
                return;
            }
        }
        const bool switch_input= switch_process && automated_switch::lifecycle_allowed(switch_lease_active,
            gameplay_ready,!mp.active&&!mp.lobby_joining,
            selftest::input_allowed(selftest::Mode::unattended,true,
                {ui.available,ui.in_menu,input.overlay_ready,input.focused,input.menu_blocked,
                 input.capture_blocked,input.menu_requested,input.available}),chassis_ride::enabled());
        if(quick_test_process&&request==11) {
            request=0;
            if(!r.quick_preflight.begin(GetTickCount64()))message("Private quick-switch test is one-shot for this process.",true);
        }
        // This diagnostic substitutes availability/device only. Actual focus,
        // UI and all existing lifecycle guards remain authoritative.
        const bool lifecycle_input=quick_test_process?(switch_input&&input.focused):(switch_process?switch_input:normal_input);
        const bool switch_had_crashed_bike=r.bike_crashed;
        (void)tick_crashed_bike(base,client,lifecycle_input,gameplay_ready&&!mp.active&&!mp.lobby_joining);
        if(r.owner_unwind.stopped())return;
        if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return;}
        if(r.step_off_board.active()&&request) {
            cancel_step_off_board("Skateboard switch canceled; the bike remains beside you. Repeat the bike action to continue.");return;
        }
        if(request==10) {begin_getup_lab(base,client,lifecycle_input,gameplay_ready&&!mp.active&&!mp.lobby_joining);return;}
        if(request==9) {
            const auto owner=read_native_frame(base,client);
            std::uint32_t physics{};NoBailSkater canonical;bool protected_now{};
            const bool physics_valid=owner.available&&memory::peek(owner.context+0x1414,physics);
            const bool owner_valid=owner.available&&inspect_no_bail_owner(client,owner.entity,canonical,protected_now)&&
                canonical.base==base&&canonical.core==owner.core&&canonical.context==owner.context&&canonical.rig==owner.rig;
            const bool eligible=gameplay_ready&&!mp.active&&!mp.lobby_joining&&ui.available&&!ui.in_menu&&
                !input.menu_blocked&&!input.capture_blocked&&!input.menu_requested&&
                !r.active.load()&&!r.pending&&!r.bike_parked&&!r.rehearsal&&owner.available&&
                physics_valid&&physics==100&&owner_valid&&!protected_now;
            if(!eligible||!bail::begin_observation(base,owner)) {
                logging::log(logging::Level::info,logging::Channel::skater,
                    "Native fall observation guard: eligible={}; frame={}; frame issue={}; physics valid={}; physics={}; canonical={}; protected={}; adapter={}; observation={}",
                    eligible,owner.available,owner.issue,physics_valid,physics,owner_valid,protected_now,
                    bail::status().issue,bail::poll_observation().issue);
                message("Native fall observation requires the opted-in private profile and an unprotected grounded skateboard rider.",true);return;
            }
            watch_physics_state(client,owner.entity);
            no_bail_request_wipeout(2.5f);
            message("Private native skateboard fall requested; recording the game's animation transition.",true);
            return;
        }
        if (ramp_process && request > 0 && request != 6) {
            message("Manual BMX mounting and equipment tests are disabled in the private ramp test process. Use the ordinary Ride profile for controller riding.", true);
            request = 0;
        }
        if (!r.ramp_request_consumed && !request && !r.active.load() && !r.pending && ramp_process) {
            const auto route = ramp_test::route();
            const auto prepared = ramp_test::snapshot();
            if (route.ready && prepared.completed && prepared.prepared && !prepared.failed) {
                r.ramp_request_consumed = true; request = 6;
            }
        }
        const bool manual_interaction=!ramp_process&&!automated_ride::enabled()&&!switch_process;
        if (r.toggle_key.update(toggle_down, normal_input && !r.rehearsal && manual_interaction) && !request) request = 4;
        auto quick_sample=input;NativeFrame lab_owner_frame;
        quick_switch_rehearsal::Output lab_output;
        if(quick_test_process&&r.quick_preflight.started()) {
            r.quick_test_tick_visited=true;
            const bool guarded_setdown_followup=r.quick_test.started()&&request==7&&r.quick_test_setdown_mount_queued;
            r.quick_test_setdown_mount_queued=false;
            const int internal_request=guarded_setdown_followup?request:0;
            if(request&&!guarded_setdown_followup) {
                r.quick_preflight.fail("preflight_external_lifecycle_request");
                r.quick_test.fail("external_lifecycle_request_during_quick_test");
                retain_quick_test_rejection(r,"external_lifecycle_request_during_quick_test",false);
            }
            const bool gates=lifecycle_input&&!r.rehearsal;
            const bool physical_quiet=std::abs(input.left_x)<=.18f&&std::abs(input.left_y)<=.18f&&
                std::abs(input.right_x)<=.18f&&std::abs(input.right_y)<=.18f&&input.pedal<=.12f&&input.brake<=.12f&&
                input.left_trigger<=.12f&&input.right_trigger<=.12f&&!input.buttons&&!input.quick_hop&&
                !input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder;
            if(!r.quick_test.terminal()) {
                const bool motion_ready=gates&&physical_quiet&&enable_native_motion(base);
                if(motion_ready)lab_owner_frame=read_native_frame(base,client);
                const auto physical=native_chassis::status();
                quick_switch_rehearsal::Frame observed;
                observed.owner={lab_owner_frame.client,lab_owner_frame.entity,lab_owner_frame.core,lab_owner_frame.context,lab_owner_frame.rig};
                observed.available=lab_owner_frame.available;observed.timing_available=lab_owner_frame.timing_available;
                observed.serial=lab_owner_frame.serial;observed.native_time=lab_owner_frame.simulation_time;
                observed.offboard=lab_owner_frame.offboard;observed.standable=lab_owner_frame.standable;
                observed.motion_supported=lab_owner_frame.motion_supported;
                observed.physics_available=lab_owner_frame.available&&memory::peek(lab_owner_frame.context+0x1414,observed.physics);
                observed.active=r.active.load();observed.pending=r.pending;
                observed.mounting=r.active.load()&&(!r.chassis_started||r.mount_blend<1);
                observed.parked=r.bike_parked;observed.carried=r.bike_carried;observed.crashed=r.bike_crashed;
                observed.pool_active=physical.active;observed.pool_parked=physical.parked;
                observed.pool_failed=physical.failed||physical.blocked||physical.owner_quarantined;
                observed.pool_suspended=physical.observation_suspended;
                observed.capture=input.capturing;observed.world=physical.world;
                observed.allocations=physical.allocation_attempts;observed.mounts=physical.mounts;
                observed.applications=physical.applications;observed.parts=world::snapshot().live_parts;
                observed.token=r.bike_parked||r.bike_crashed?r.parked_token:r.active.load()?r.chassis_step.body_token:0;
                observed.strict_setdowns=r.quick_test_strict_setdowns;
                QuickTestObservation sample;
                sample.present=true;sample.stage=r.quick_test.started()?"scenario":"preflight";
                sample.phase=quick_switch_rehearsal::phase_name(r.quick_test.phase());sample.wall_ms=GetTickCount64();
                sample.motion_enabled=sample.native_read_attempted=motion_ready;sample.native_issue=lab_owner_frame.issue;
                sample.lifecycle_allowed=gates;sample.physical_quiet=physical_quiet;sample.focused=input.focused;
                sample.ui_available=ui.available;sample.ui_menu=ui.in_menu;sample.overlay_ready=input.overlay_ready;
                sample.overlay_menu=input.menu_blocked;sample.capture_blocked=input.capture_blocked;sample.menu_requested=input.menu_requested;
                sample.pool_ready=physical.ready;sample.pool_failed=physical.failed;sample.pool_blocked=physical.blocked;
                sample.pool_quarantined=physical.owner_quarantined;sample.pool_suspended=physical.observation_suspended;
                sample.pool_deferred=physical.deferred_park;sample.pool_detached=physical.detached;sample.pool_issue=physical.issue;
                auto& board=observed.board_transition;board=r.quick_board_request;
                board.observed_wall_ms=sample.wall_ms;board.pending_deadline_ms=r.board_switch.pending_deadline_ms();
                board.pending_for_owner=r.board_switch.pending_for(sample.wall_ms,
                    {lab_owner_frame.client,lab_owner_frame.entity,lab_owner_frame.core,lab_owner_frame.context,lab_owner_frame.rig});
                if(r.quick_test.awaiting_native_board()&&(observed.physics==200||observed.physics==201)&&
                    motion_ready&&observed.available&&observed.timing_available&&observed.physics_available&&
                    board.pending_for_owner&&board.accepted_requests&&board.request_owner==observed.owner&&board.world&&board.token) {
                    board.parked_pool_proven=native_chassis::parked_owner_matches(base,lab_owner_frame,board.world,board.token,false);
                    std::uint64_t after_serial{};double after_time{};std::uint32_t after_physics{};
                    board.clock_unchanged=read_native_clock(lab_owner_frame.entity,lab_owner_frame.core,after_serial,after_time)&&
                        after_serial==observed.serial&&after_time==observed.native_time;
                    board.physics_unchanged=memory::peek(lab_owner_frame.context+0x1414,after_physics)&&after_physics==observed.physics;
                }
                sample.frame=observed;r.quick_last_attempt=std::move(sample);
                if(!r.quick_first_attempt.present)r.quick_first_attempt=r.quick_last_attempt;
                if(!r.quick_test.started()) {
                    const auto ready=r.quick_preflight.step(observed,motion_ready,gates,physical_quiet,r.quick_last_attempt.wall_ms);
                    if(ready==quick_switch_rehearsal::PreflightResult::rejected)r.quick_test.fail(r.quick_preflight.issue());
                    else if(ready==quick_switch_rehearsal::PreflightResult::qualified) {
                        // Preserve the original command wall origin; readiness cannot
                        // renew either the scenario190s bound or existing switch lease.
                        if(board_handoff_trace::selected()&&board_handoff_trace::store.mode==board_handoff_trace::Mode::quick_switch)
                            r.quick_test.observe_board_dwell_for_private_trace(board_handoff_trace::driver_board_dwell_seconds);
                        if(!r.quick_test.start(r.quick_preflight.wall_started()))r.quick_test.fail("scenario_start_rejected_after_preflight");
                    }
                }
                if(r.quick_test.started()&&!r.quick_test.terminal()&&
                    !(overlay::detail::private_bmx_native_input_handoff_requested()&&r.quick_test.awaiting_native_board()))
                    lab_output=r.quick_test.step(observed,gates,physical_quiet,r.quick_last_attempt.wall_ms);
                if(r.quick_test.terminal()&&!r.quick_test.passed())retain_quick_test_rejection(r,r.quick_test.issue(),true);
            }
            // Pending readiness and every terminal result remain neutral. The
            // original hardware observation stays separate from this fixture.
            quick_sample.keyboard=false;quick_sample.available=true;
            quick_sample.device=quick_switch_rehearsal::virtual_device;
            quick_sample.left_x=quick_sample.left_y=quick_sample.right_x=quick_sample.right_y=0;
            quick_sample.pedal=quick_sample.brake=quick_sample.left_trigger=quick_sample.right_trigger=0;
            quick_sample.buttons=0;quick_sample.quick_hop=false;quick_sample.right_shoulder=false;
            quick_sample.dismount_requested=lab_output.north;quick_sample.left_shoulder=lab_output.shoulder;
            quick_sample.sampled_ms=GetTickCount64();
            request=r.quick_test.terminal()?0:lab_output.setup_request?lab_output.setup_request:internal_request;
        }
        const bool lab_interaction=quick_test_process&&r.quick_test.started()&&!r.quick_test.terminal()&&lifecycle_input;
        const bool interact_allowed=(normal_input&&!r.rehearsal&&manual_interaction)||lab_interaction;
        // North-only equipment capture is independent of nearby interaction:
        // park/stepoff cleanup cannot release an accepted chord while held.
        const bool quick_allowed=interact_allowed&&!quick_sample.keyboard&&quick_sample.device!=0;
        const bool quick_owner_needed=quick_allowed&&
            (r.quick_switch_button.owner_sample_needed(quick_sample.dismount_requested,quick_sample.left_shoulder)||
                r.human_native_return_pending);
        NativeFrame quick_owner_frame;
        if(quick_owner_needed) {
            if(lab_interaction)quick_owner_frame=lab_owner_frame;
            else if(enable_native_motion(base))quick_owner_frame=read_native_frame(base,client);
        }
        const auto quick=r.quick_switch_button.update(quick_sample.dismount_requested,quick_sample.left_shoulder,
            quick_allowed,quick_sample.device,quick_owner_needed,
            quick_owner_frame.available?BikeQuickSwitchOwner{quick_owner_frame.client,quick_owner_frame.entity,
                quick_owner_frame.core,quick_owner_frame.context,quick_owner_frame.rig}:BikeQuickSwitchOwner{},quick_sample.sampled_ms);
        DingoSDKOverlaySetBmxQuickSwitchCapture(quick.capture_north);
        unsigned quick_routed_request{};bool human_admission_resumed{},human_wait_request_consumed{};
        const auto admission_context=[&](const NativeFrame& frame,std::uint64_t now) {
            human_native_handoff::AdmissionContext c;
            c.now=now;c.sampled_ms=quick_sample.sampled_ms;c.device=quick_sample.device;
            c.owner={base,frame.client,frame.entity,frame.core,frame.context,frame.rig};
            c.serial=frame.serial;c.native_time=frame.simulation_time;
            c.allowed=normal_input&&quick_allowed&&frame.available&&frame.timing_available&&frame.serial&&
                std::isfinite(frame.simulation_time)&&frame.simulation_time>=0&&r.active.load()&&
                r.native_chassis_mode&&r.chassis_started&&r.chassis_step.ready&&!r.rehearsal&&r.mount_blend>=1&&
                !r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_crashed&&!r.bike_carried&&!r.step_off_board.active();
            c.sdk_chord_held=quick_sample.dismount_requested&&quick_sample.left_shoulder&&!quick_sample.right_shoulder&&!quick_sample.quick_hop&&
                (quick_sample.buttons&~0x8100u)==0&&std::abs(quick_sample.left_x)<=.05f&&std::abs(quick_sample.left_y)<=.05f&&
                std::abs(quick_sample.right_x)<=.05f&&std::abs(quick_sample.right_y)<=.05f&&quick_sample.pedal<=.05f&&quick_sample.brake<=.05f&&
                quick_sample.left_trigger<=.05f&&quick_sample.right_trigger<=.05f;
            return c;
        };
        // A new lifecycle action cancels this one held gesture; it never routes
        // through the legacy software helper while awaiting the original poll.
        if(overlay::detail::human_native_admission_waiting()) {
            if(request||quick.pressed||toggle_down||interact_down||carry_down) {
                overlay::detail::cancel_human_native_admission("new_request_during_original_poll_wait");request=0;human_wait_request_consumed=true;
            } else {
                const auto pending=overlay::detail::poll_human_native_admission(admission_context(quick_owner_frame,GetTickCount64()));
                if(pending==human_native_handoff::AdmissionWaitResult::ready) {
                    request=4;quick_routed_request=4;human_admission_resumed=true;
                }
            }
        }
        if(quick.pressed&&!request&&!human_admission_resumed&&!human_wait_request_consumed) {
            switch(quick_bike_action(r.active.load(),r.pending,r.active.load()&&r.mount_blend<1,r.bike_parked,r.bike_crashed)) {
            case QuickBikeAction::skateboard:request=4;break;
            case QuickBikeAction::nearby_bike:request=7;break;
            case QuickBikeAction::equip_bike:request=1;break;
            default:message("Finish getting on the bike before switching equipment.",true);break;
            }
            quick_routed_request=static_cast<unsigned>(request);
        }
        const auto interaction=r.interaction_button.update(quick_sample.dismount_requested,
            !r.human_native_input_pending&&!r.human_native_return_pending&&!overlay::detail::human_native_admission_waiting()&&!quick.suppress_north&&interact_allowed&&(r.active.load()||(r.native_chassis_mode&&r.pending)||r.bike_parked||
                (r.bike_crashed&&r.crash_interaction_ready)),GetTickCount64());
        if(r.interact_key.update(interact_down,interact_allowed)&&!request)request=7;
        if(r.carry_key.update(carry_down,interact_allowed)&&!request)request=8;
        if(interaction!=BikeInteraction::none&&!request)request=interaction==BikeInteraction::hold?8:7;
        if(human_wait_request_consumed)request=0;
        if(quick_test_process&&r.quick_test.started()) {
            r.quick_test.feedback(lab_output,quick,interaction,quick_routed_request);
            if(r.quick_test.terminal()) {
                request=0;
                if(!r.quick_test.passed())retain_quick_test_rejection(r,r.quick_test.issue(),true);
            }
        }
        if(r.human_native_input_pending&&request) {
            overlay::detail::cancel_human_native_handoff("new_lifecycle_request_during_human_switch");
            request=0; // Never queues a fallback or return ride.
        }
        // Only the explicit human shortcut on a native board takes this leg.
        // A proved already-on-foot action retains its original mount path.
        if(r.human_native_return_mount&&request) {
            end_ride("Native return mount canceled by a new lifecycle action.");return;
        }
        bool reverse_started_now{};
        if(overlay::detail::human_native_handoff_requested()&&quick_routed_request==1&&request==1&&
            (!quick_owner_frame.available||!quick_owner_frame.offboard)) {
            request=0; // A rejected source never falls through to the selector helper.
            const auto sample=human_native_return_sample(base,client,quick_owner_frame);
            auto context=admission_context(quick_owner_frame,sample.now);
            context.allowed=normal_input&&quick_allowed&&quick_owner_frame.available&&quick_owner_frame.timing_available&&
                quick_owner_frame.serial&&std::isfinite(quick_owner_frame.simulation_time)&&quick_owner_frame.simulation_time>=0&&
                r.native_chassis_mode&&!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_crashed&&!r.bike_carried&&
                !r.chassis_started&&!r.rehearsal&&!r.step_off_board.active();
            r.human_native_return_pending=overlay::detail::begin_human_native_return(sample,context);
            reverse_started_now=r.human_native_return_pending;
            if(reverse_started_now) {
                cancel_bmx_dismount();r.dismount_requested=false;r.dismount_retry_after=0;
                r.mount_dismount.reset();
                MountDismountSample settle;settle.now=sample.now;settle.deadline=overlay::detail::human_native_return_deadline();
                settle.input_sampled_ms=sample.sampled_ms;settle.serial=sample.serial;settle.native_time=sample.native_time;
                settle.owner={quick_owner_frame.client,quick_owner_frame.entity,quick_owner_frame.core,quick_owner_frame.context,quick_owner_frame.rig};
                settle.available=quick_owner_frame.available;settle.timing_available=quick_owner_frame.timing_available;
                settle.offboard=quick_owner_frame.offboard;settle.standable=quick_owner_frame.standable;settle.motion_supported=quick_owner_frame.motion_supported;
                settle.physics_available=sample.clock_coherent;settle.physics=sample.physics;settle.quiet=false;
                (void)r.mount_dismount.update(settle); // Retain the existing required settled-return gate.
                message("Native return admitted. Release LB/L1 and Y/Triangle; waiting for one original-input dismount.",true);
            } else message("Native return refused; no software dismount or automatic retry. See BMX status.",true);
        }
        if(r.human_native_return_pending) {
            r.human_native_return_tick_visited=true;
            if(!reverse_started_now) {
                if(request||quick.pressed||toggle_down||interact_down||carry_down) {
                    overlay::detail::cancel_human_native_return("new_lifecycle_request_during_native_return");request=0;
                } else {
                    const auto sample=human_native_return_sample(base,client,quick_owner_frame);
                    const auto context=admission_context(quick_owner_frame,sample.now);
                    const bool gesture_only=(input.buttons&~0x8100u)==0&&!input.right_shoulder&&!input.quick_hop&&
                        std::abs(input.left_x)<=.05f&&std::abs(input.left_y)<=.05f&&std::abs(input.right_x)<=.05f&&std::abs(input.right_y)<=.05f&&
                        input.pedal<=.05f&&input.brake<=.05f&&input.left_trigger<=.05f&&input.right_trigger<=.05f;
                    human_native_handoff::Guard guard{sample.now,sample.sampled_ms,sample.owner,sample.sdk_device,
                        normal_input&&quick_allowed&&sample.configuration&&sample.gates&&sample.canonical&&!sample.protected_owner&&sample.clock_coherent,
                        gesture_only,sample.gesture_released};
                    overlay::detail::update_human_native_return(sample,guard,context.sdk_chord_held);
                }
            }
            if(overlay::detail::human_native_return_active())return;
            r.human_native_return_pending=false;
            const bool completed=overlay::detail::human_native_return_complete();
            try {
                const auto status=overlay::detail::human_native_handoff_status();
                logging::write(logging::Level::info,logging::Channel::skater,status);
                const auto root=user_data::override_root();
                if(root&&r.human_native_reports<16) {
                    ++r.human_native_reports;
                    const auto detail=Json::parse(status);const auto generation=detail.at("generation").get<std::uint64_t>();
                    const auto name="bmx-human-native-return-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(generation)+".json";
                    std::ofstream output(*root/name,std::ios::trunc);
                    output<<Json{{"schema",1},{"process_id",GetCurrentProcessId()},{"reverse_native504_observed",completed},
                        {"physical_controller_acceptance_verified",false},{"handoff",detail}}.dump(2)<<'\n';output.flush();
                    if(!output.good())++r.human_native_report_drops;
                } else ++r.human_native_report_drops;
            } catch(...) {++r.human_native_report_drops;}
            if(!completed){r.mount_dismount.reset();request=0;message("Native return canceled; no software dismount or automatic retry.",true);return;}
            // Continue the existing settle/collision/acquisition route, with the
            // original admitted deadline and the legacy selector disabled.
            r.human_native_return_mount=true;r.human_native_return_until=overlay::detail::human_native_return_deadline();
            request=1;
        }
        if(r.private_native_input_started) {
            // A first-trial intent consumes later equipment requests; they must
            // never create a queued software mount or an automatic return ride.
            request=0;
            const auto phase=overlay::detail::private_bmx_native_input_handoff_phase();
            if(phase==native_input_handoff::Phase::complete||phase==native_input_handoff::Phase::aborted)return;
        }
        if(r.step_off_board.active()&&request) {
            cancel_step_off_board("Skateboard switch canceled; the bike remains beside you. Repeat the bike action to continue.");return;
        }
        if(request){cancel_carried_setdown();native_chassis::revoke_released_witness();}
        // There is no acquired riding body to park or carry during admission.
        // Treat either interaction gesture and explicit dismount as cancellation.
        if ((request<0||request==7||request==8)&&r.native_chassis_mode&&!r.chassis_started&&
            (r.pending||r.active.load())) {
            end_ride("BMX mount canceled.");return;
        }
        if (request < 0) {
            if(r.active.load()&&lifecycle_input) (void)park_bike(base,read_native_frame(base,client),false);
            else end_ride("BMX dismounted.");
            return;
        }
        if(request==7||request==8) {
            if(!lifecycle_input) {message("Close menus and focus the game to interact with the bike.",true);return;}
            const bool carry=request==8;const auto frame=read_native_frame(base,client);
            if(r.active.load()) {(void)park_bike(base,frame,carry);return;}
            if(r.bike_crashed&&!recover_crashed_bike(base,frame))return;
            if(r.stepping_off)return;
            if(r.bike_parked) {
                if(!frame.available||EquipmentOwner{frame.client,frame.entity,frame.core,frame.context,frame.rig}!=r.parked_owner||!frame.standable) {
                    message("Stand on clear ground beside the bike before picking it up or riding.",true);return;
                }
                const float approach=std::hypot(frame.position[0]-r.parked_bike.position[0],frame.position[2]-r.parked_bike.position[2]);
                if(!r.bike_carried&&!mount_transition::interaction_distance(approach,true)) {
                    message(quick.pressed?"Your bike is still parked where you left it. Move beside it, then press LB/L1 + Y/Triangle to ride.":
                        "Move closer to your bike, or use F6 to switch equipment.",true);return;
                }
                if(!carry&&!r.bike_carried&&!mount_transition::interaction_distance(approach,false)) {
                    message("Stand closer beside the bike before getting on.",true);return;
                }
                if(r.bike_carried) {
                    if(!begin_carried_setdown(frame,!carry,false))message("Stand on clear ground to set down the bike; bike kept in hand.",true);
                    else message("Checking nearby wheel support before setting down the bike.",true);
                    request=0;
                } else if(carry) {
                    r.bike_carried=true;r.parked_ground_verified=false;
                    message("Carrying the bike. Hold Y/Triangle to set it down, or tap to ride.",true);request=0;
                } else {r.mount_from_parked=true;request=1;}
            } else request=1;
        }
        if (request == 5) {
            if (!r.native_chassis_mode || !r.active.load() || !r.chassis_started || !r.chassis_step.ready || r.rehearsal) {
                message("BMX pause test requires an active isolated physical chassis outside a riding rehearsal.",true); return;
            }
            r.chassis_pause_requested = true; request = 0;
        }
        if(overlay::detail::private_bmx_native_input_handoff_requested()&&request==4&&quick_routed_request!=4) {
            r.quick_test.fail("private_input_route_requires_first_synthetic_quick_chord");
            message("Private native-input trial requires its explicit quick-switch prefix; no software board fallback.",true);return;
        }
        if (request == 4) {
            if(switch_had_crashed_bike&&!r.bike_crashed) {
                message("Equipment switch canceled because the crashed bike or rider changed.",true);return;
            }
            EquipmentOwner switch_owner;
            if(r.bike_crashed&&!preflight_crashed_board_switch(base,client,switch_owner))return;
            if(r.active.load()&&r.native_chassis_mode&&r.chassis_started&&!r.rehearsal) {
                if(!lifecycle_input)message("Close menus and focus the game before switching equipment.",true);
                else if(r.mount_blend<1)message("Finish getting on the bike before switching equipment.",true);
                else {
                    const auto canonical=read_native_frame(base,client);
                    const bool human_route=quick_routed_request==4&&overlay::detail::human_native_handoff_requested();
                    const auto admission_now=GetTickCount64();
                    const bool resumed_owner=!human_admission_resumed||(quick_owner_frame.available&&
                        BikeQuickSwitchOwner{canonical.client,canonical.entity,canonical.core,canonical.context,canonical.rig}==
                        BikeQuickSwitchOwner{quick_owner_frame.client,quick_owner_frame.entity,quick_owner_frame.core,quick_owner_frame.context,quick_owner_frame.rig});
                    const bool gesture_owner=quick_owner_frame.available&&
                        BikeQuickSwitchOwner{canonical.client,canonical.entity,canonical.core,canonical.context,canonical.rig}==
                        BikeQuickSwitchOwner{quick_owner_frame.client,quick_owner_frame.entity,quick_owner_frame.core,quick_owner_frame.context,quick_owner_frame.rig};
                    auto admission=admission_context(canonical,admission_now);admission.allowed=admission.allowed&&gesture_owner;
                    const bool resumed_clock=!human_admission_resumed||(canonical.serial>=quick_owner_frame.serial&&
                        canonical.simulation_time>=quick_owner_frame.simulation_time&&
                        (canonical.serial==quick_owner_frame.serial)==(canonical.simulation_time==quick_owner_frame.simulation_time));
                    const bool human_admitted=!human_route||(canonical.available&&resumed_owner&&resumed_clock&&admission.allowed&&admission.sdk_chord_held&&
                        (human_admission_resumed?overlay::detail::resume_human_native_handoff(admission):
                            overlay::detail::admit_human_native_handoff(admission)));
                    if(human_admission_resumed&&!human_admitted)
                        overlay::detail::cancel_human_native_admission("pending_final_canonical_or_context_refused");
                    const bool deferred=human_route&&!human_admitted&&!human_admission_resumed&&overlay::detail::human_native_admission_waiting();
                    if(deferred)message("Waiting briefly for the game's controller poll. Keep LB/L1 + Y/Triangle held.",true);
                    else if(!human_admitted)message("Native switch requires one current XInput pad, a neutral stance, then LB/L1 + fresh Y/Triangle. Bike retained.",true);
                    if(human_admitted&&park_bike(base,canonical,false)) {
                        r.human_native_input_pending=human_route;r.human_native_input_ticket={};
                        // park_bike calls end_ride internally. Arm only after its
                        // owned bike and existing step-off path have been retained.
                        if(!r.step_off_board.begin(GetTickCount64(),r.parked_owner,r.parked_world,r.parked_token,r.step_to)) {
                            r.stepping_off=r.carry_after_step=false;release_control();
                            DingoSDKOverlaySetBmxInteractionCapture(false);
                            overlay::detail::cancel_human_native_handoff("step_off_admission_refused");
                            message("Skateboard switch canceled; the bike remains beside you.",true);return;
                        }
                        if(!human_route&&!overlay::detail::private_bmx_native_input_handoff_requested())board_handoff_trace::arm_quick_switch();
                        message("Stepping off the bike before switching to the skateboard.",true);return;
                    }
                }
                if(overlay::detail::human_native_handoff_requested()&&!r.human_native_input_pending&&!overlay::detail::human_native_admission_waiting())
                    overlay::detail::cancel_human_native_handoff("park_admission_refused");
                // Rejected F6 must continue the ordinary live ride/mount tick.
                // This tick's world frame was cleared before processing input.
                request=0;
            } else if (r.active.load() || r.pending || r.bike_parked || r.bike_crashed) {
                if(!switch_owner.entity) {
                    const auto canonical=read_native_frame(base,client);
                    if(!canonical.available) {message("Wait for a fresh local rider before switching equipment.",true);return;}
                    switch_owner={canonical.client,canonical.entity,canonical.core,canonical.context,canonical.rig};
                    const EquipmentOwner expected=r.bike_parked?r.parked_owner:EquipmentOwner{r.client,r.entity,r.core,r.context,r.rig};
                    if(expected.entity&&expected!=switch_owner) {
                        if(service_retained_owner(base,client))return;
                        end_ride("Equipment switch canceled because the local rider changed.");return;
                    }
                }
                if(r.bike_crashed) {
                    const auto canonical=read_native_frame(base,client);
                    if(!canonical.available) {withdraw_unavailable_bike_observation();return;}
                    if(!native_chassis::park(base,canonical,true)) {
                        if(native_chassis::status().observation_suspended) {withdraw_unavailable_bike_observation();return;}
                        end_ride("Bike physics changed before switching equipment.");return;
                    }
                    // Parking succeeded under fresh proof. Avoid a second
                    // owner read in end_ride after this logical handoff.
                    r.bike_crashed=false;
                }
                end_ride("BMX removed. Switching to the native skateboard.");
                r.board_switch.request(GetTickCount64(),switch_owner);
            } else {
                r.board_switch.cancel(); cancel_bmx_board_mount();
                request = 1;
            }
        }
        if (request == 2 || request == 3 || request == 6) {
            if (r.active.load()) { message("Dismount before starting the isolated BMX rehearsal.", true); return; }
            const auto isolated_root = user_data::override_root();
            if (!user_data::development_requested() || !isolated_root) {
                message("BMX rehearsal requires a separate --dev-data-root test profile.", true); return;
            }
            r.rehearsal_root = *isolated_root;
            r.ramp_rehearsal = request == 6;
            if (r.ramp_rehearsal) {
                r.ramp_route = ramp_test::route();
                if (!r.ramp_route.ready || !chassis_ride::enabled() ||
                    _wcsicmp(isolated_root->filename().c_str(),L"bmx-user-data-ramp-test") != 0 ||
                    !r.ramp_route.prepared_at_ms || GetTickCount64() < r.ramp_route.prepared_at_ms ||
                    GetTickCount64()-r.ramp_route.prepared_at_ms > 2000) return;
                if (!r.ramp_driver.reset({r.ramp_route.origin,r.ramp_route.forward,r.ramp_route.start,
                    r.ramp_route.floor_y,r.ramp_route.lip_height,-r.ramp_route.lip_z})) return;
            }
            r.rehearsal_mode = request == 3 || request == 6 ? selftest::Mode::unattended : selftest::Mode::focused;
            r.background_input_used = r.rehearsal_expired = false;
            r.unfocused_ticks = 0; r.background_seconds = 0;
            r.rehearsal = true; r.rehearsal_seconds = r.max_distance = r.max_height = r.max_turn = 0;
            r.right_turn = {};r.observed_bike_turn = {};
            r.before_brake_speed = 0; r.after_brake_speed = r.final_speed = 1000;
            r.before_brake_measured = r.after_brake_measured = r.final_speed_measured = false;
            r.rehearsal_started = GetTickCount64();
            r.initial_updates = native_motion_updates(); r.initial_draws = visual::draws(); r.initial_poses = rider::updates();
            r.initial_world_placements = world::snapshot().entity_placements;
            r.initial_render_poses = rider::render_updates();
            r.chassis_samples.clear(); r.chassis_tracking_error = 0; r.chassis_step = {};
            r.chassis_ground_known = false;
            r.chassis_com_known = r.chassis_before_measured = r.chassis_after_measured = r.chassis_final_measured = false;
            r.chassis_distance = r.chassis_before_brake = r.chassis_hop_rise = r.chassis_takeoff_speed = 0;
            r.chassis_after_brake = r.chassis_final_speed = 1000;
            wchar_t trick_flag[2]{};
            r.trick_rehearsal = r.rehearsal_mode == selftest::Mode::unattended && chassis_ride::enabled() &&
                GetEnvironmentVariableW(L"RESKATE_BMX_TRICK_REHEARSAL",trick_flag,2)==1 && trick_flag[0]==L'1';
            r.catalog_rehearsal=r.trick_rehearsal && catalog_test::enabled();
            r.catalog_peak_render_poses=0; r.catalog_last_render_count=r.initial_render_poses;
            r.catalog_max_render_phase=0;
            r.catalog_observed_requested_writes=r.catalog_observed_applied_writes=r.catalog_observed_invalid_writes=0;
            r.catalog_observed_min_strength=r.catalog_observed_max_strength=0;
            r.trick_bar_release = r.trick_tail_release = r.trick_pitch_command = r.trick_pitch_torque = r.trick_pitch_rate = 0;
            r.trick_bar_catches = r.trick_tail_catches = 0;
            r.trick_pitch_serial = r.trick_pitch_response_serial = 0;
            r.trick_pitch_before_rate = r.trick_pitch_response = 0; r.trick_pitch_axis = {};
            r.chassis_min_contact_scale = 1;
            r.chassis_next_progress = r.rehearsal_started+2000;
            r.chassis_bridge_ms = r.chassis_bridge_peak_ms = r.chassis_world_ms = r.chassis_world_peak_ms = 0;
            r.chassis_bridge_calls = r.chassis_world_calls = 0;
        }
        if (request > 0 && request != 4 && !r.active.load()) {
            if(r.bike_crashed) {
                if(request!=1||!lifecycle_input||!recover_crashed_bike(base,read_native_frame(base,client)))return;
                r.mount_from_parked=true;
            }
            if (request == 1) r.ramp_rehearsal = false;
            r.board_switch.cancel(); cancel_bmx_board_mount();
            r.pending = true; r.pending_until = r.human_native_return_mount?r.human_native_return_until:GetTickCount64() + 15000; r.dismount_retry_after = 0;
            r.mount_seed_last=Json{};r.mount_seed_attempts=0;r.mount_seed_next_log=0;
            r.mount_owner.reset();if(!r.human_native_return_mount)r.mount_dismount.reset();r.mount_search.cancel();
            r.native_chassis_mode = chassis_ride::enabled();
        }
        if(overlay::detail::private_bmx_native_input_handoff_requested()&&r.board_switch.active()) {
            r.board_switch.cancel();cancel_bmx_board_mount();
            r.quick_test.fail("software_board_intent_forbidden_in_private_native_input_trial");return;
        }
        if (r.board_switch.active()) {
            if (!gameplay_ready || mp.active || mp.lobby_joining) {
                end_ride("Skateboard switch canceled because the solo world changed."); return;
            }
            const auto world_model = world::snapshot();
            // The prior BMX models and input lease must be gone before a board
            // request. WorldTick removes the models on this client tick.
            const bool ready = lifecycle_input && !input.capturing && world_model.live_parts == 0;
            NativeFrame frame;
            if (ready && enable_native_motion(base)) frame = read_native_frame(base, client);
            std::uint32_t physics{};
            const bool physics_valid = frame.available && memory::peek(frame.context + 0x1414, physics);
            r.observed_physics_valid.store(physics_valid);
            if (physics_valid) r.observed_physics.store(physics);
            const auto decision = r.board_switch.update(GetTickCount64(), ready, physics_valid,
                {frame.client, frame.entity, frame.core, frame.context, frame.rig},
                frame.offboard, frame.standable, physics == addr::no_bail::grounded_board_physics_state);
            if (decision == BoardSwitchResult::timed_out) {
                end_ride("Skateboard switch timed out. Stand on clear ground, close menus and try F6 again."); return;
            }
            if (decision == BoardSwitchResult::owner_changed) {
                end_ride("Skateboard switch canceled because the local rider changed."); return;
            }
            if (decision == BoardSwitchResult::complete) {
                cancel_bmx_board_mount();
                logging::write(logging::Level::info,logging::Channel::skater,
                    "Skateboard equipped: verified native grounded board state100; BMX world parts0 and capture released.");
                message("Skateboard equipped. LB/L1 + Y/Triangle: bike.",true);
                return;
            }
            if (decision == BoardSwitchResult::request_mount) {
                const auto request_wall=GetTickCount64();
                const bool accepted=request_bmx_board_mount(client,frame.entity); // Existing native call exactly once; retain retry behavior.
                if(accepted&&quick_test_process&&r.quick_test.started()&&!r.quick_test.terminal()) {
                    auto& receipt=r.quick_board_request;
                    if(!receipt.accepted_requests) {
                        receipt.request_owner={frame.client,frame.entity,frame.core,frame.context,frame.rig};
                        receipt.request_wall_ms=request_wall;receipt.request_serial=frame.serial;receipt.request_native_time=frame.simulation_time;
                        receipt.request_clock_valid=frame.available&&frame.timing_available&&frame.serial&&std::isfinite(frame.simulation_time);
                        receipt.world=native_chassis::status().world;receipt.token=r.chassis_step.body_token;
                    }
                    ++receipt.accepted_requests;
                }
                message(accepted ?
                    "Switching to skateboard: native mount requested; waiting for actual board state100." :
                    "Switching to skateboard: waiting for the game's on-foot board lock or mount readiness.");
            } else if (decision == BoardSwitchResult::blocked) {
                cancel_bmx_board_mount(); release_control();
                message("Switching to skateboard: close menus, focus the game and wait for BMX cleanup.");
            } else {
                message(physics_valid ? "Switching to skateboard: native physics state " + std::to_string(physics) + "; waiting for grounded mounting." :
                    "Switching to skateboard: waiting for the current local rider.");
            }
            return;
        }
        if (r.rehearsal && selftest::expired(r.rehearsal_started, GetTickCount64())) {
            r.rehearsal_expired = true;
            end_ride("BMX rehearsal expired after 120 wall seconds. Check the native simulation and readiness report."); return;
        }
        if (r.chassis_pause_active && GetTickCount64()-r.chassis_pause_started > 5000) {
            finish_chassis_pause_test(false,"wall limit expired"); end_ride("BMX pause test expired."); return;
        }
        if (r.rehearsal) {
            if (!user_data::runtime_development_selected({}, r.rehearsal_root.native())) {
                end_ride("BMX rehearsal stopped because its isolated data root changed."); return;
            }
        }
        if(tick_parked_bike(base,client,lifecycle_input,gameplay_ready&&!mp.active&&!mp.lobby_joining))return;
        if (!r.active.load() && !r.pending) return;
        if (!gameplay_ready || mp.active || mp.lobby_joining) {
            end_ride("BMX requires a loaded solo session. Run bmx mount after loading."); return;
        }
        const auto input_mode = r.rehearsal ? r.rehearsal_mode : selftest::Mode::focused;
        if (!(switch_process?lifecycle_input:selftest::input_allowed(input_mode, r.rehearsal,
            {ui.available, ui.in_menu, input.overlay_ready, input.focused, input.menu_blocked,
             input.capture_blocked, input.menu_requested, input.available}))) {
            if(r.native_chassis_mode&&r.chassis_started&&r.mount_blend<1&&!r.rehearsal) {
                end_ride("BMX mount canceled because focus or input ownership changed.");return;
            }
            r.mount_dismount.interrupt();
            release_control();
            cancel_bmx_dismount(); r.dismount_requested = false; r.dismount_retry_after = 0;
            r.mapper.reset(); r.clock.reset(); r.interrupted = true;
            message(r.pending ? "BMX mount queued. Close the menu and return both sticks to center." :
                                "BMX paused. Close menus, focus the game and center the sticks.");
            return;
        }
        if (r.rehearsal && !input.focused) ++r.unfocused_ticks;
        if (r.pending && !enable_native_motion(base)) {
            end_ride("BMX native motion contracts are unavailable: " + native_motion_issue()); return;
        }
        const auto frame = read_native_frame(base, client);
        std::uint32_t observed_physics{};
        const bool observed_valid = frame.available && memory::peek(frame.context + 0x1414, observed_physics);
        r.observed_physics_valid.store(observed_valid);
        if (observed_valid) r.observed_physics.store(observed_physics);
        if (r.pending) {
            const auto owner = r.mount_owner.update(GetTickCount64(), r.pending_until, frame.available,
                {frame.client, frame.entity, frame.core, frame.context, frame.rig});
            if (owner == MountOwnerResult::owner_changed) {
                if(service_retained_owner(base,client,&frame))return;
                end_ride("BMX mount canceled because the local rider changed."); return;
            }
            if (owner == MountOwnerResult::timed_out) {
                std::string last_guard;
                { std::lock_guard lock(r.status_mutex); last_guard = r.message; }
                stop_mount_admission("BMX mount timed out. Last readiness check: " + last_guard); return;
            }
            if (owner == MountOwnerResult::waiting && frame.available) {
                wait_for_mount("BMX waiting for complete native rider ownership."); return;
            }
        }
        if (!frame.available) {
            r.mount_dismount.interrupt();
            if(r.native_chassis_mode&&r.chassis_started&&r.mount_blend<1&&!r.rehearsal) {
                end_ride("BMX mount canceled because the live rider became unavailable.");return;
            }
            release_control(); r.mapper.reset(); r.clock.reset(); r.interrupted = true;
            visual::publish({});
            message(frame.issue);
            return;
        }
        auto dismount_settled=MountDismountResult::ready;
        if(r.pending&&r.native_chassis_mode) {
            MountDismountSample s;s.now=GetTickCount64();s.deadline=r.pending_until;
            s.input_sampled_ms=input.sampled_ms;s.serial=frame.serial;s.native_time=frame.simulation_time;
            s.owner={frame.client,frame.entity,frame.core,frame.context,frame.rig};
            s.available=frame.available;s.timing_available=frame.timing_available;s.offboard=frame.offboard;
            s.standable=frame.standable;s.motion_supported=frame.motion_supported;
            s.physics_available=observed_valid;s.physics=observed_physics;s.quiet=r.native_board_control_input_allowed;
            dismount_settled=r.mount_dismount.update(s);
            if(dismount_settled==MountDismountResult::cancelled) {
                stop_mount_admission("BMX mount canceled because the dismount owner, clock or deadline changed.");return;
            }
        }
        if (r.pending && r.native_chassis_mode) {
            // Admission is read-only. Refuse a retired/uncertain native pool
            // before requesting a board dismount or claiming riding ownership.
            const auto admission = native_chassis::mount_admission(base,frame);
            if(service_retained_owner(base,client))return;
            if (admission.decision == native_chassis::AdmissionDecision::refused) {
                const auto physical=native_chassis::status();
                const auto retained=native_chassis::released_ownership_status();
                logging::log(logging::Level::warning,logging::Channel::skater,
                    "BMX remount refused before forget: admission={}; chassis issue={}; ready/active/parked/detached/failed/blocked/suspended/deferred={}/{}/{}/{}/{}/{}/{}/{}; allocations={}; mounts={}; retained publication available={}; retained owner={}/{}/{}/{}/{}; current owner={}/{}/{}/{}/{}; retained world/state/index/generation/token={}/{}/{}/{}/{}; native offboard/standable={}/{}; snapshot issue={}.",
                    admission.issue,physical.issue,physical.ready,physical.active,physical.parked,physical.detached,
                    physical.failed,physical.blocked,physical.observation_suspended,physical.deferred_park,
                    physical.allocation_attempts,physical.mounts,retained.available,
                    retained.client,retained.entity,retained.core,retained.context,retained.rig,
                    frame.client,frame.entity,frame.core,frame.context,frame.rig,
                    retained.world,retained.native_state,retained.body_index,retained.body_generation,retained.token,
                    frame.offboard,frame.standable,static_cast<unsigned>(physical.last_snapshot_issue));
                // Cancel the SDK lease and discard its retained handles without
                // dereferencing them, just as acquisition's backstop would.
                native_chassis::forget();
                end_ride(admission.issue);
                return;
            }
            if (admission.decision == native_chassis::AdmissionDecision::wait) {
                r.mount_dismount.interrupt();
                wait_for_mount(admission.issue);
                return;
            }
        }
        if (!frame.offboard) {
            if (r.pending) {
                if(r.human_native_return_mount) {
                    stop_mount_admission("Native return mount canceled because actual offboard state was lost; no software fallback.");return;
                }
                const auto now = GetTickCount64();
                if (now >= r.dismount_retry_after) {
                    r.dismount_requested = request_bmx_dismount(client, frame.entity);
                    // The native request expires after 250ms. Retry only after that
                    // lease is gone; let the game's own selector choose the transition.
                    r.dismount_retry_after = now + 350;
                }
                message(r.dismount_requested ? "BMX waiting for the game's dismount transition." :
                    "BMX waiting for a grounded skateboard dismount."); return;
            }
            end_ride("Step off the skateboard first, then run bmx mount."); return;
        }
        if (r.active.load() && (frame.client != r.client || frame.entity != r.entity || frame.core != r.core ||
            frame.context != r.context || frame.rig != r.rig || !native_motion_healthy())) {
            end_ride("BMX stopped because the local rider or motion ownership changed."); return;
        }
        if (r.active.load() && !frame.motion_supported) {
            end_ride("BMX stopped for a native animation or ragdoll transition."); return;
        }
        if (r.pending) {
            if(r.native_chassis_mode&&dismount_settled!=MountDismountResult::ready) {
                wait_for_mount("BMX waiting for a settled native dismount and released controls.");return;
            }
            if (r.ramp_rehearsal && !prepared_ramp_entry_matches(r.ramp_route,frame)) {
                end_ride("Ramp rehearsal stopped because its prepared rider, start or fixture changed."); return;
            }
            if (!frame.standable) { wait_for_mount("BMX waiting for the rider to stand on the ground."); return; }
            State initial;
            initial.x = frame.position[0]; initial.y = frame.position[1]; initial.z = frame.position[2];
            initial.yaw = r.ramp_rehearsal ? r.ramp_route.yaw : std::atan2(frame.transform[8], frame.transform[10]);
            if(r.mount_from_parked) {initial.x=r.parked_bike.position[0];initial.z=r.parked_bike.position[2];initial.yaw=r.parked_bike.yaw;}
            const auto ground = wheels(initial, 0, frame.entity);
            const auto readiness = mount_ground_readiness(ground.ready, ground.supported, initial.y, ground.height);
            if (readiness != MountGroundResult::ready) {
                wait_for_mount(readiness == MountGroundResult::query_unavailable ?
                    "BMX waiting for the native wheel collision query to become ready." :
                    readiness == MountGroundResult::unsupported ?
                    "BMX waiting for verified ground under both wheels. Move to a clear, level surface." :
                    "BMX waiting for nearby wheel support. Move to a clear, level surface.");
                return;
            }
            r.root_height = static_cast<float>(initial.y) - ground.height;
            r.bike.reset(initial);
            r.mapper.reset(); r.clock.reset();
            r.client = client; r.entity = frame.entity; r.core = frame.core;
            r.context = frame.context; r.rig = frame.rig;
            r.initial_position = frame.position; r.initial_yaw = initial.yaw;
            r.previous_position = frame.position;
            if (!update_bmx_protection(client, frame.entity, true)) { end_ride("BMX cannot acquire the on-foot rider protection lease."); return; }
            r.active.store(true); r.pending = false; r.interrupted = false;
            r.human_native_return_mount=false;r.human_native_return_until=0;
            r.mount_blend=r.rehearsal?1.f:0.f;
            if (r.native_chassis_mode) message("BMX getting on the bike.");
            else message(std::string(mounted_controls),true);
        }
        if (!update_bmx_protection(client, frame.entity, true)) { end_ride("BMX rider protection ownership changed."); return; }
        if (r.native_chassis_mode) {
            RawInput raw;
            raw.left_x = input.left_x; raw.left_y = input.left_y;
            raw.right_x = input.right_x; raw.right_y = input.right_y;
            raw.pedal = input.pedal; raw.brake = input.brake; raw.quick_hop = input.quick_hop;
            raw.left_trigger=input.left_trigger;raw.right_trigger=input.right_trigger;
            raw.left_shoulder=input.left_shoulder;raw.right_shoulder=input.right_shoulder;
            raw.available = input.available; raw.focused = input.focused; raw.device = input.device;
            tick_native_chassis(base, frame, raw, input.focused,switch_process);
            return;
        }
        const auto& before = r.bike.state();
        const double dx = before.x - frame.position[0], dy = before.y - frame.position[1], dz = before.z - frame.position[2];
        if (dx * dx + dy * dy + dz * dz > 4) { end_ride("BMX stopped after a teleport or large rider displacement."); return; }
        r.bike.reconcile_position(frame.position[0], frame.position[1], frame.position[2]);
        if (r.interrupted) {
            auto at = r.bike.state(); at.speed = at.vertical_speed = 0;
            r.bike.reset(at); r.velocity = {}; r.interrupted = false;
        }
        auto support = wheels(r.bike.state(), r.root_height, frame.entity);
        if (!support.ready) {
            release_control(); r.mapper.reset(); r.clock.reset(); r.interrupted = true;
            message("BMX paused: collision under the wheels is unavailable or too steep."); return;
        }
        DingoSDKOverlaySetBmxInputCapture(true);
        r.observed_capture.store(true);
        if (!world::snapshot().ready) {
            release_native_motion(); r.clock.reset(); r.mapper.reset(); r.interrupted = true;
            publish_visual(frame, support.pitch);
            message("BMX waiting for its native world model: " + world::snapshot().issue);
            return;
        }
        if (!frame.timing_available) {
            release_native_motion(); r.clock.reset(); r.mapper.reset(); r.interrupted = true;
            publish_visual(frame, support.pitch);
            message("BMX waiting for the local rider's native physics steps."); return;
        }
        const double seconds = r.clock.sample_time(frame.serial, frame.simulation_time);
        if (r.clock.discontinuity()) {
            release_control(); r.clock.reset(); r.mapper.reset(); r.interrupted = true;
            r.velocity = {};
            message("BMX paused after a native simulation gap. Center both sticks to resume."); return;
        }
        if (frame.seconds <= 0) {
            release_native_motion();
            r.mapper.reset(); r.interrupted = true;
            publish_visual(frame, support.pitch);
            return;
        }
        if (seconds > 0) {
            RawInput raw;
            raw.left_x = input.left_x; raw.left_y = input.left_y;
            raw.right_x = input.right_x; raw.right_y = input.right_y;
            raw.pedal = input.pedal; raw.brake = input.brake;
            raw.quick_hop = input.quick_hop;
            raw.left_trigger=input.left_trigger;raw.right_trigger=input.right_trigger;
            raw.left_shoulder=input.left_shoulder;raw.right_shoulder=input.right_shoulder;
            raw.available = input.available; raw.focused = input.focused; raw.device = input.device;
            if (r.rehearsal) {
                r.rehearsal_seconds += seconds;
                const auto elapsed = r.rehearsal_seconds;
                const double native_speed = std::hypot(frame.position[0] - r.previous_position[0], frame.position[2] - r.previous_position[2]) / seconds;
                r.previous_position = frame.position;
                if (elapsed >= 4.5 && elapsed < 5) {
                    r.before_brake_speed = std::max(r.before_brake_speed, native_speed); r.before_brake_measured = true;
                }
                if (elapsed >= 5.8 && elapsed < 6) {
                    r.after_brake_speed = std::min(r.after_brake_speed, native_speed); r.after_brake_measured = true;
                }
                if (elapsed >= 8.7) { r.final_speed = native_speed; r.final_speed_measured = true; }
                raw = selftest::controls(elapsed);
                if (!input.focused) { r.background_input_used = true; r.background_seconds += seconds; }
                r.max_distance = std::max(r.max_distance, static_cast<double>(std::hypot(frame.position[0] - r.initial_position[0], frame.position[2] - r.initial_position[2])));
                if (support.supported) r.max_height = std::max(r.max_height, static_cast<double>(frame.position[1] - r.root_height - support.height));
                const double native_yaw = std::atan2(frame.transform[8], frame.transform[10]);
                r.max_turn = std::max(r.max_turn, std::abs(std::remainder(native_yaw - r.initial_yaw, tau)));
                r.right_turn.observe(elapsed, native_yaw);
                if (elapsed >= selftest::simulation_duration) {
                    finish_rehearsal(true);
                    end_ride("BMX native rehearsal finished. See bmx-selftest.json in the isolated data folder."); return;
                }
            }
            const bool grounded = r.bike.state().grounded || (support.supported && r.bike.state().vertical_speed <= 0 &&
                r.bike.state().y <= support.height + r.root_height + .02);
            const auto actions = r.mapper.update(raw, {grounded}, seconds);
            r.preload = static_cast<float>(actions.preload);
            r.front_weight = static_cast<float>(actions.front_weight);
            r.rear_weight = static_cast<float>(actions.rear_weight);
            const auto result = r.bike.advance(seconds, riding_command(actions), support.contacts);
            bool collision_available{};
            if (!path_clear(frame, r.bike.state(), r.root_height, collision_available)) {
                r.bike.stop_horizontal();
                r.bike.reconcile_position(frame.position[0], r.bike.state().y, frame.position[2]);
                if (!collision_available) {
                    release_control(); r.mapper.reset(); r.clock.reset(); r.interrupted = true;
                    message("BMX paused: the obstacle query is unavailable."); return;
                }
            }
            const auto& state = r.bike.state();
            if (!state.grounded) {
                if (actions.handlebar != TrickDirection::none && r.bar_time == 0) r.bar_time = actions.handlebar == TrickDirection::left ? -.55 : .55;
                if (actions.tailwhip != TrickDirection::none && r.whip_time == 0) r.whip_time = actions.tailwhip == TrickDirection::left ? -.7 : .7;
            } else if (!result.hopped) {
                r.bar_time = r.whip_time = r.barspin = r.tailwhip = 0;
            }
            auto animate = [=](double& time, double& angle, double duration) {
                if (!time) { angle = 0; return; }
                const double sign = time < 0 ? -1.0 : 1.0;
                const double remaining = std::max(0.0, std::abs(time) - seconds);
                angle = sign * tau * (1.0 - remaining / duration);
                time = sign * remaining;
                if (!remaining) angle = 0;
            };
            animate(r.bar_time, r.barspin, .55); animate(r.whip_time, r.tailwhip, .7);
            r.velocity = {static_cast<float>(std::sin(state.yaw) * state.speed),
                static_cast<float>(state.vertical_speed), static_cast<float>(std::cos(state.yaw) * state.speed)};
        }
        const auto& state = r.bike.state();
        const std::array<float, 3> target{static_cast<float>(state.x), static_cast<float>(state.y), static_cast<float>(state.z)};
        if (!submit_native_motion(frame, r.velocity, target, static_cast<float>(state.yaw))) {
            end_ride("BMX native movement rejected the current frame."); return;
        }
        publish_visual(frame, support.pitch);
        if (seconds > 0) {
            std::ostringstream info;
            info.precision(2); info << std::fixed << "BMX mounted: " << std::abs(state.speed) * 3.6 << " km/h; "
                << (state.grounded ? "grounded" : "airborne") << ". Left stick: handlebars. Right stick: rear/hop/tailwhip.";
            message(info.str());
        }
    } catch (...) {
        if(r.quick_preflight.started()) {
            r.quick_preflight.fail("preflight_runtime_exception");r.quick_test.fail("runtime_exception");
            retain_quick_test_rejection(r,"runtime_exception",false);
        }
        end_ride("BMX stopped after an integration error. See the runtime log.");
    }
}
}
