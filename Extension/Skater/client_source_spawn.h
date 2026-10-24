#pragma once
#include <cstdint>
#include <array>
#include <string>
#include "Extension/UI/Overlay/overlay.h"

namespace dingosdk {
// Binds the native camera functions the debug controls use. Installs no hooks.
void initialize_client_source_spawn(std::uintptr_t base);
// Installs the exact camera-reset handler guard; forwarding is unchanged until a Noclip request is queued.
bool start_client_noclip_velocity(std::uintptr_t base) noexcept;
// A plugin's push (skater_push.h): one horizontal velocity at the next on-board physics step,
// in competition physics only. Uses the verified local physics owner and never enables trainer
// boosts.
bool queue_skater_push_velocity(std::uintptr_t client,std::uintptr_t entity,const std::array<float,3>& velocity) noexcept;
// Trainer: at the local skater's next physics update, multiply its upward velocity by
// `factor` if it is off the board and rising (a hippy jump that has just started). One request
// at a time; it expires after 150 ms.
bool queue_jump_scale(std::uintptr_t client, std::uintptr_t entity, float factor) noexcept;
// What the last request did: 0 pending or none, 2 scaled, -1 the skater was not rising yet,
// -2 nothing to scale (on the board, or a state that keeps no velocity), -3 it failed.
struct JumpScaleResult {
    int outcome{};
    float up_speed{}; // before scaling
};
JumpScaleResult take_jump_scale_result() noexcept;
// Trainer: a function to run on the physics thread after each of the skater's physics updates (nullptr: none).
void set_physics_step_callback(void (*callback)() noexcept) noexcept;
// Trainer: the local skater's push speed as a multiple of the game's own (1 leaves the skater
// alone); `stock` is the game's top pushing speed in m/s. `cruise` above 0 is auto push: a
// rolling skater that is not braking gains speed up to it (m/s). Publish every client tick: it
// expires after 500 ms.
void set_push_speed(std::uintptr_t client, std::uintptr_t entity, float factor, float stock, float cruise) noexcept;
// A plugin's cruise control (reskate.skater.cruise): a rolling skater that is not braking is carried
// up to `speed` (m/s; 0 off, at most 30), and with `may_slow` eased down to it when not pushing. Rides
// on the trainer's skater and expiry (set_push_speed); it expires after 300 ms, so set it every tick.
void set_plugin_cruise(float speed, bool may_slow) noexcept;
// A plugin's velocity (reskate.skater.set_velocity): the board's bodies set to `velocity` (or it added
// to theirs) by the next physics step with the local skater on its board, on the ground or in the air,
// within 500 ms; dropped after. A new one replaces one still waiting.
void set_plugin_velocity(const std::array<float, 3>& velocity, bool add) noexcept;
// Trainer: gravity in the air on the board as a multiple of the game's own (1 leaves the skater alone). Publish every
// client tick: it expires after 500 ms. `air_gravity_steps` counts the physics steps it acted on.
// landing: the gravity the game plans landings with, as a multiple of its own (0: the same as factor). timing: 0 the game's
// own animation timing in the air, 1 the timing the flight would have at the game's gravity (air_animation_rate is the
// rate that gives it, 1 on the ground).
void set_air_gravity(std::uintptr_t client, std::uintptr_t entity, float factor, float landing = 0, float timing = 0) noexcept;
float air_animation_rate() noexcept;
std::uint32_t air_gravity_steps() noexcept;
// Trainer: the game has just launched a flight with this trajectory object (any skater's; any thread).
void note_flight_record(std::uintptr_t trajectory) noexcept;
// The trainer's pump power: speed added for as long as the skater pumps a transition, sized so
// that `factor` 2 is about one more of the game's own pumps (1 leaves the game alone, under 1
// takes speed away). `pumping` is the game's own word on whether the skater is pumping this tick; with
// `measure` the gain is measured even at factor 1. Publish every client tick: it expires after
// 500 ms.
void set_pump_power(std::uintptr_t client, std::uintptr_t entity, float factor, bool pumping, bool measure) noexcept;
// The pump now running, or the last one: the energy the game's pump gained (J per kg; negative: the
// skater lost speed while pumping) and what the trainer added to that.
struct PumpPowerReport {
    std::uint32_t pumps{}; // one more per pump
    float gained{}, given{}, speed{};
    std::uint32_t steps{}; // physics steps it has lasted
};
PumpPowerReport pump_power_report() noexcept;
// The trainer's revert boost: speed given back on a landing the game auto reverts (client_noclip.cpp).
// `strength` multiplies the boost (1: about +1.5 to +2.6 m/s a landing), 0 switches it off. Call
// every tick: it lapses half a second after the last call.
// What counts as a revert and what it is worth. The defaults are AutoRevertBoost's, plus the
// landing out of line with the travel (see client_noclip.cpp).
struct RevertTuning {
    float min_spin{90.0f};   // degrees the skater must have turned in the air
    float min_slip{90.0f};   // 90: off. Below it: degrees the board lands out of line with the direction of travel also count, or
    float min_twist{40.0f};  // degrees the board must land out of line with the body (a board bend)
    float bend_boost{2.0f};  // m/s for a landing whose board turned 40 degrees past the body, rising to
    float full_boost{4.0f};  // m/s at 360 degrees
    float auto_boost{1.5f};  // m/s once the board turned more than 140 degrees past the body (an auto revert)
    float max_speed{40.0f};  // m/s: nothing is added above this
    float cooldown{0.5f};    // seconds between boosts
    float min_air{0.25f};    // seconds a flight must last
    bool operator==(const RevertTuning &) const = default;
};
void set_revert_boost(std::uintptr_t client, std::uintptr_t entity, float strength, const RevertTuning &tuning = {}) noexcept;
// The last landing the revert boost judged, for the trainer's log: what it measured and what it did.
struct RevertBoostReport {
    std::uint64_t sequence{}; // one more per landing judged; 0: none yet
    float spin{}, board_offset{}, board_rotation{}; // degrees
    float slip{};                                   // degrees the board landed out of line with the direction of travel
    float speed{}, added{};                         // m/s
    std::uint32_t air_ms{}, landed_state{};
    bool board_read{};
    const char *outcome = ""; // "boost", or why not
};
RevertBoostReport revert_boost_report() noexcept;
// The trainer's help back into a ramp off a coping grind, a stall or a handplant (client_noclip.cpp). Publish every
// client tick: it expires after 500 ms. `edge` and `ramp` are level unit vectors (x, z): along the coping, and from
// the coping into the ramp.
struct TransitionAssist {
    float carry{};      // 0..1: how much of the speed along the coping is put back when a coping grind ends without a pop (0: off)
    float drop_speed{}; // m/s: the most speed across the coping, into the ramp, while it is put back (0: no limit)
    float unstick{};    // strength of the push out of a hang-up (0: off)
    float plant{};      // 0..1: a handplant's exit speed moved toward Skate 3's (0: off)
    bool coping{};      // the skater is on a coping grind or stall
    bool transition{};  // the skater rode steep ground in the last seconds
    bool measure{};     // report even with everything off
    std::array<float, 2> edge{}, ramp{};
};
void set_transition_assist(std::uintptr_t client, std::uintptr_t entity, const TransitionAssist &assist) noexcept;
// The last coping grind left without a pop (kind 1), handplant (2) or hung landing off a transition (3), for the
// trainer's log. Speeds in m/s: `in_*` on the last step on the coping, `game_*` what the game made of it a third of a
// second later, `out_*` what the skater had then (the same unless the trainer changed it).
struct TransitionReport {
    std::uint64_t sequence{}; // one more per report; 0: none yet
    int kind{};
    std::uint32_t from{}, to{}, to_last{}; // physics states: on the coping, the first after it, and when the watch ended
    float in_along{}, in_across{}, in_down{};
    float game_along{}, game_across{};
    float out_along{}, out_across{}, out_down{};
    float drop{}, fall{};      // metres lost by then; for a handplant: inside the plant, and a second and a half after it
    std::uint32_t hung{};      // physics steps spent slower than 1.1 m/s in the second and a half after
    std::uint32_t carried{}, kicked{}; // steps on which the trainer changed the velocity
};
TransitionReport transition_report() noexcept;
// Engine-thread-only interactive controls. Presentation callbacks only queue requests.
overlay::DebugModel on_client_debug_tick(std::uintptr_t base, std::uintptr_t client,
    bool can_control, bool camera_phase_observed, const overlay::DebugRequest* request = nullptr,
    const overlay::FlightInput* flight_input = nullptr);
bool restore_client_debug(std::uintptr_t base, std::uintptr_t client, bool camera_phase_observed);
bool client_free_camera_active() noexcept;
// The last verified mode-1 local view, or zero when native placement handed it
// back. Unlike the input-capture lease above, this proves actual selection.
std::uintptr_t client_free_camera_view() noexcept;
// Native camera lease for the party's Spectate action. Null restores only the
// camera acquired by this function; it never changes another debug camera. A positive
// `fov` is applied to the spectate camera while it is held; its own comes back after.
bool update_party_camera(std::uintptr_t base, std::uintptr_t client, bool ready, bool phase,
    const std::array<float, 16>* transform, std::string& detail, float fov = 0) noexcept;
// The local skater's animation component (what the pose hooks are called with), or 0. Client thread.
std::uintptr_t local_skater_component(std::uintptr_t base, std::uintptr_t client) noexcept;
bool read_local_camera_transform(std::uintptr_t base, std::uintptr_t client, std::array<float, 16>& transform) noexcept;
// Publishes the local view's camera (Engine/Game/UI/game_view.h) for things drawn over the
// world. Client thread, every tick while something needs it.
bool publish_local_camera_view(std::uintptr_t base, std::uintptr_t client) noexcept;
}
