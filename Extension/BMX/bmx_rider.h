#pragma once
#include "bmx_visual.h"
#include "Engine/Game/BMX/bmx_native_render_phase.h"
#include <string>
namespace dingosdk {class Json;}
namespace dingosdk::bmx::board_handoff_trace {struct ClientState;}
namespace dingosdk::bmx::rider {
void poll_private_board_handoff(std::uintptr_t base,bool idle,const board_handoff_trace::ClientState&) noexcept;
void finish_private_grip_probe() noexcept; // explicit end_ride diagnostic export request; no native work
void begin_client_tick() noexcept; // revoke admission, preserving logical mount contacts
visual::Publication pending_placement() noexcept;
bool complete_placement(std::uint64_t revision,std::uintptr_t entity,bool all_parts_placed) noexcept;
void publish(const visual::Frame& frame) noexcept; // stage; no pose admission before placement receipt
bool available() noexcept;
std::uint64_t updates() noexcept;
std::uint64_t render_updates() noexcept;
struct PrivateHandoffPublication {bool inactive{};std::uint64_t poses{},renders{},board_hides{},revision{};};
PrivateHandoffPublication private_handoff_publication() noexcept;
// Timer gating only; native animation/render callbacks remain unchanged.
bool profiling_active() noexcept;
// Coherent contact errors/owner from the last completed native render-pose write,
// not visual QA. Inactive publication or an owner change invalidates the snapshot.
struct PoseMetrics {
    float hand_error{}, foot_error{};
    std::uintptr_t render_entity{};
    std::uint64_t render_observed_ms{}, render_count{};
    bool render_ready{};
    float hand_contact{1}, foot_contact{1}; // 0=free, 1=caught; errors follow intended targets
    catalog::TrickId movement_id{catalog::TrickId::none};
    float movement_phase{}, movement_progress{};
    visual::RiderMode rider_mode{visual::RiderMode::none};
    std::uint64_t publication_revision{}; // exact staged frame whose eight placement commands completed
    unsigned posture_attempts{};float posture_scale{},posture_fit_adjustment{},posture_gaze{};
    bool posture_requested{},posture_fallback{};
    bool observed_requested{},observed_applied{};float observed_strength{};
};
PoseMetrics metrics() noexcept;
struct CoupledAvatarBindingStatus {
    avatar_binding::Calibration calibration;
    avatar_binding::Epoch completed_pose_epoch;
    bool proximal_ready{},completed_pose_write{},actual_spring_applied{};
    float proximal_fit_adjustment{},hand_error{},foot_error{};
    native_render_phase::Receipt presentation; // sampled pair epoch and actual render clock stay distinct
};
// Bounded SDK receipt, never fresh native proof. Caller samples current actual
// pair and uses avatar_binding::sample; this cannot enable physical adoption.
CoupledAvatarBindingStatus coupled_avatar_binding_status() noexcept;
// Deferred client-thread JSON; one bounded endpoint snapshot, no callback I/O.
Json coupled_pose_diagnostic() noexcept;
std::string status();
}
