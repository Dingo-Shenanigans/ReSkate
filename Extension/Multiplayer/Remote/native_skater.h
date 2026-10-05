#pragma once
#include "Extension/Multiplayer/Net/protocol.h"

namespace dingosdk::multiplayer {
struct NativeFrame {
    bool ready{};
    std::uintptr_t context{}, parent{}, entity{}, board_entity{};
    Pose pose;
    std::string detail;
};
struct NativeAnimationStats {
    std::uint64_t evaluated{}, skipped{}, evaluation_us{}, apply_us{};
};
// All entry points except model access run on the verified client update thread.
// Identity/root capture remains per client callback. Full rig buffers are only
// needed when the network send deadline is due.
NativeFrame capture_local(std::uintptr_t base, std::uintptr_t client, bool capture_pose);
NativeFrame capture_local(std::uintptr_t base, std::uintptr_t client);
bool show_remote(std::uintptr_t base, std::uintptr_t client, const NativeFrame &, const Pose &,
                 std::string &detail);
void remove_remote(std::uintptr_t base) noexcept;
void update_remote_cosmetics(std::uintptr_t base, const NativeFrame &, const Appearance &,
                             std::string &detail);
std::uint64_t remote_pose_updates() noexcept;
std::uint64_t remote_board_pose_updates() noexcept;
NativeAnimationStats remote_animation_stats() noexcept;
std::uintptr_t remote_skater_entity() noexcept;
// Whether any slot other than `except` has a remote skater now. Any thread.
bool other_remote_skaters(std::size_t except) noexcept;
std::uintptr_t remote_board_entity() noexcept;
// Changes whenever the current slot's actor is created, removed or destroyed.
std::uint64_t remote_skater_generation() noexcept;
// Client thread. Installs the shared animation/destruction hooks without
// creating a remote actor, for other features that create native actors.
bool install_entity_hooks(std::uintptr_t base, std::string &detail) noexcept;
// Called by the destruction hook before the engine frees any entity.
using EntityDestroyed = void (*)(std::uintptr_t entity) noexcept;
void set_entity_destroyed_listener(EntityDestroyed listener) noexcept;
// Called after the native animation update of every skater component that is
// not a remote peer, once its output pose for this update is written. Requires
// install_entity_hooks. Runs on the thread that evaluates animation.
using AnimationEvaluated = void (*)(std::uintptr_t component) noexcept;
void set_animation_evaluated_listener(AnimationEvaluated listener) noexcept;
// The same call, made before the evaluated listener: the style layer adjusts the pose that
// listener (first person) then reads.
void set_animation_style_listener(AnimationEvaluated listener) noexcept;
// Called on the client thread with the local skater pose capture_local is about to return,
// so a feature that changed the evaluated pose can decide what other players receive.
using LocalPoseFilter = void (*)(std::uintptr_t component, std::vector<Transform> &skater) noexcept;
void set_local_pose_filter(LocalPoseFilter filter) noexcept;
// Called after a skeleton's pose is handed to the renderer, with
// its animation interface (animation holder + 0xc0), on the calling thread.
using RenderPosePublished = void (*)(std::uintptr_t animation_interface) noexcept;
void set_render_pose_listener(RenderPosePublished listener) noexcept;
// The same call, made before the render pose listener, for the style layer.
void set_render_pose_style_listener(RenderPosePublished listener) noexcept;
// Far-player savings (puppet_cost.cpp): native work a remote player's skater and skateboard
// skip while far away. Within 200 m of the local skater or the camera nothing changes.
// Each is on by default; the setter is thread-safe and applies at each player's next
// note_remote_distance.
enum class PuppetSaving : std::uint8_t {
    // The skater's AntDriven component no longer re-places the entity every frame from its pose;
    // the network placement (place_actor) is the only one. Any distance.
    driven_placement,
    // The ECS attach that never completes on a remote skater or skateboard is retried once a
    // second instead of every frame. Any distance.
    ecs_retry,
    // Past 200 m the crowd-avoidance agent, AI stimulus and pedestrian collider update 4 times a
    // second with the elapsed game time, instead of every frame.
    far_components,
};
void set_puppet_saving(PuppetSaving saving, bool enabled) noexcept;
bool puppet_saving(PuppetSaving saving) noexcept;
// Client thread, for the current PeerScope slot, after show_remote: the player's distance in
// metres to the nearer of the local skater and the camera (non-finite = unknown, treated as
// near). Call it whenever the player's pose is sampled.
void note_remote_distance(float metres) noexcept;
// One console line: which savings are on and how many players each currently applies to.
std::string puppet_saving_status();
// First person shrinks one joint (the head) of the local skater's evaluated
// pose so it is not drawn across the view. Poses sent to other players carry
// the joint's real scale instead. component 0 ends the override.
void set_local_hidden_joint(std::uintptr_t component, std::uint16_t joint,
                            const std::array<float, 3> &scale) noexcept;
} // namespace dingosdk::multiplayer
