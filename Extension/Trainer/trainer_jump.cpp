#include "trainer_jump.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/launches.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <intrin.h>

namespace dingosdk::trainer {
namespace {
// Game build 25414733. void set_trajectory(trajectory, parameters): the launch velocity is
// the vector at parameters + 0x80. Measured with a scripted controller:
//  - a no comply calls it from 0x482f646 on entering the pop (state 200) after a crouch
//    (state 103) of about 0.20 s; an ollie crouches for 0.08 s and launches elsewhere;
//  - a boneless calls it from 0x47691f8 while the foot is planted (state 602);
//  - a flat-ground ollie calls it from 0x4824f7f (Engine/Game/Build/20260929/launches.h).
constexpr std::uintptr_t set_trajectory = 0x4752aa0, no_comply_call = 0x482f646, boneless_call = 0x47691f8, ollie_call = 0x4824f7f;
constexpr std::array<unsigned char, 32> set_trajectory_prefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x88, 0xbb, 0x00,
                                                              0x00, 0x00, 0x48, 0x8b, 0xd9, 0x0f, 0x85, 0xf6, 0x00, 0x00, 0x00,
                                                              0xc5, 0xfc, 0x28, 0x05, 0x62, 0x7d, 0xe8, 0x01, 0xc5, 0xfc};
// An ollie (riding or out of a manual) launches from 0x4824f7f (ollie_call) with the state 103 after 100.
constexpr std::array<unsigned char, 5> ollie_call_bytes{0xe8, 0x1c, 0xdb, 0xf2, 0xff};
constexpr std::array<unsigned char, 5> no_comply_call_bytes{0xe8, 0x55, 0x34, 0xf2, 0xff}, boneless_call_bytes{0xe8, 0xa3, 0x98, 0xfe, 0xff};
constexpr std::uint32_t crouch_state = 103, pop_state = 200, plant_state = 602;
constexpr float no_comply_crouch = 0.12f; // seconds: between an ollie's crouch and a no comply's
constexpr std::size_t velocity_y = 0x84 / sizeof(float);

using SetTrajectory = void (*)(std::uintptr_t, float *);
struct Hooked {
    std::atomic<SetTrajectory> original{};
    std::uintptr_t base{};
    std::atomic<float> no_comply{1}, boneless{1}, manual{1};
    std::atomic<std::uint64_t> manual_until{}; // GetTickCount64: the skater was in a manual until about then
    std::atomic<std::int32_t> manual_intent{-1};
    bool ollie_known{};
    std::atomic<std::uint32_t> launch_trick{0xff}; // CurrentFlipTrick as the trainer read it last tick
    std::atomic<std::uintptr_t> trick_at{};        // where the game keeps that number (0: not known)
    std::atomic<TrajectoryModifier> modifier{};
    std::array<std::atomic<float>, 64> flip_launch{}; // least upward launch speed by trick, m/s; 0: the game's own
    // The last few launches, for the trainer's log line: written by the physics thread, read
    // by the client tick.
    std::array<std::atomic<std::uint64_t>, 8> seen{};
    std::atomic<std::uint32_t> written{}, read{};
    // The newest pop (ollie, manual, grind): its trajectory object, when, and the upward speed it left with.
    std::atomic<std::uintptr_t> flight_object{};
    std::atomic<std::uint64_t> flight_tick{};
    std::atomic<float> flight_up{};
    // What that pop was launched with (physics thread only), and a higher upward speed to launch it again with.
    std::array<float, 256> flight_parameters{};
    bool flight_kept{};
    std::atomic<float> raise_to{};
    std::atomic<int> raised{}; // 1 done, -1 failed; taken by the trainer
    bool attempted{}, ready{};
    // The pop out of a grind: the sideways multiplier, the travel on the grind, and the newest pop as the game made it.
    std::atomic<float> grind_side{1}, travel_x{}, travel_z{};
    std::atomic<float> pop_along{}, pop_side{}, pop_nudge{};
    std::atomic<float> grind_push{}, grind_nudge{};
    std::atomic<std::uint32_t> pop_serial{}, pop_taken{};
};
Hooked &hooked() {
    static auto *value = new Hooked;
    return *value;
}
// The trick the game has in mind right now (the trainer's own reading is a tick old: out of a manual or a grind the
// trick is often chosen in the tick of the pop).
std::uint32_t live_trick(std::uintptr_t at) noexcept {
    if (!at) return 0xff;
    __try {
        return *reinterpret_cast<const std::uint32_t *>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xff;
    }
}
bool keep_floats(const float *from, float *to, std::size_t count) noexcept {
    __try {
        std::memcpy(to, from, count * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_guarded(SetTrajectory function, std::uintptr_t trajectory, float *parameters) noexcept {
    __try {
        function(trajectory, parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// After a physics update: a pop that has to go higher after all is launched again, from where it started (it is a tick
// or two old), with the higher upward speed. Writing the trajectory object does nothing (measured): the setter has to run.
void raise_step() noexcept {
    auto &h = hooked();
    const float up = h.raise_to.exchange(0, std::memory_order_acq_rel);
    if (!(up > 0)) return;
    const auto original = h.original.load(std::memory_order_acquire);
    const auto object = h.flight_object.load(std::memory_order_relaxed);
    if (!original || !object || !h.flight_kept || GetTickCount64() - h.flight_tick.load(std::memory_order_relaxed) > 400) {
        h.raised.store(-1, std::memory_order_release);
        return;
    }
    h.flight_parameters[velocity_y] = up;
    const bool done = call_guarded(original, object, h.flight_parameters.data());
    if (done) h.flight_up.store(up, std::memory_order_relaxed);
    h.raised.store(done ? 1 : -1, std::memory_order_release);
}
void set_trajectory_hook(std::uintptr_t trajectory, float *parameters) {
    auto &h = hooked();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - h.base;
    if (const auto modifier = h.modifier.load(std::memory_order_acquire); modifier && parameters) {
        float velocity[3]{parameters[velocity_y - 1], parameters[velocity_y], parameters[velocity_y + 1]};
        modifier(caller - 5, trajectory, velocity);
        if (std::isfinite(velocity[0]) && std::isfinite(velocity[1]) && std::isfinite(velocity[2])) {
            parameters[velocity_y - 1] = velocity[0];
            parameters[velocity_y] = velocity[1];
            parameters[velocity_y + 1] = velocity[2];
        }
    }
    // A dedicated server's ollie lock (session_tools.h): the ollie launches with no upward speed,
    // so the skater stays on the ground.
    if (parameters && caller == ollie_call + 5 && !session_ollies_allowed() && parameters[velocity_y] > 0) parameters[velocity_y] = 0;
    const auto trick_then = h.launch_trick.load(std::memory_order_relaxed), trick_live = live_trick(h.trick_at.load(std::memory_order_relaxed));
    if (parameters && (caller == no_comply_call + 5 || caller == boneless_call + 5)) {
        const auto watch = watched_physics_state(); // the local skater's
        Trick trick{};
        if (caller == boneless_call + 5 && watch.valid && watch.state == plant_state) trick = Trick::boneless;
        else if (caller == no_comply_call + 5 && watch.valid && watch.state == pop_state && watch.previous == crouch_state &&
                 watch.previous_seconds >= no_comply_crouch)
            trick = Trick::no_comply;
        const float up = parameters[velocity_y];
        // Any launch that still goes up: riding downhill takes most of the upward speed away, and
        // those launches were being left alone.
        if (trick != Trick::none && std::isfinite(up) && up > 0.05f) {
            const float factor = (trick == Trick::boneless ? h.boneless : h.no_comply).load(std::memory_order_relaxed);
            if (factor != 1 && up * factor < 200.0f) parameters[velocity_y] = up * factor;
            std::uint32_t bits[2];
            std::memcpy(&bits[0], &up, 4);
            std::memcpy(&bits[1], &factor, 4);
            // 31 bits of each float (both positive) and the trick in the top bit.
            const auto packed = (std::uint64_t{bits[0]} << 32) | (std::uint64_t{bits[1]} << 1) | (trick == Trick::boneless ? 1u : 0u);
            h.seen[h.written.fetch_add(1, std::memory_order_relaxed) % h.seen.size()].store(packed, std::memory_order_release);
        }
    }
    const bool ollie = parameters && h.ollie_known && caller == ollie_call + 5;
    // The pop out of a grind or a slide comes through the no comply's call, with a 400s state before it (playtest of 2026-10-10).
    bool grind = false;
    if (parameters && caller == no_comply_call + 5) {
        const auto watch = watched_physics_state();
        grind = watch.valid && watch.state == pop_state && game::build::v20260929::launches::is_grind_state(watch.previous);
    }
    if (grind) {
        const float tx = h.travel_x.load(std::memory_order_relaxed), tz = h.travel_z.load(std::memory_order_relaxed);
        const float length = std::sqrt(tx * tx + tz * tz);
        float &vx = parameters[velocity_y - 1], &vz = parameters[velocity_y + 1];
        if (length > 0.3f && std::isfinite(vx) && std::isfinite(vz)) {
            const float nx = tx / length, nz = tz / length, along = vx * nx + vz * nz;
            const float sx = vx - along * nx, sz = vz - along * nz, side = std::sqrt(sx * sx + sz * sz);
            h.pop_along.store(along, std::memory_order_relaxed);
            h.pop_side.store(side, std::memory_order_relaxed);
            h.pop_serial.fetch_add(1, std::memory_order_release);
            const float factor = h.grind_side.load(std::memory_order_relaxed);
            if (factor != 1 && factor >= 0 && side * factor < 60.0f) {
                vx = along * nx + sx * factor;
                vz = along * nz + sz * factor;
            }
            // Skate 3's pop: the stick's sideways push times a speed, to the side the stick points.
            const float push = h.grind_push.load(std::memory_order_relaxed), nudge = std::clamp(h.grind_nudge.load(std::memory_order_relaxed), -1.0f, 1.0f);
            h.pop_nudge.store(nudge, std::memory_order_relaxed);
            if (push != 0 && std::abs(push) < 60.0f && std::abs(nudge) > 0.05f) {
                vx += -nz * nudge * push;
                vz += nx * nudge * push;
            }
        }
    }
    // The ollie's launch, out of a manual (the game pops lower from one; see set_manual_pop).
    if (ollie && GetTickCount64() < h.manual_until.load(std::memory_order_relaxed)) {
        const float up = parameters[velocity_y], factor = h.manual.load(std::memory_order_relaxed);
        if (factor != 1 && std::isfinite(up) && up > 0.05f && up * factor < 200.0f) parameters[velocity_y] = up * factor;
    }
    // A pop into a flip trick that is set to turn slowly: at least the air it needs (see set_flip_launch).
    bool floor = ollie || grind;
    if (floor) {
        const auto floor_of = [&](std::uint32_t trick) { return trick < h.flip_launch.size() ? h.flip_launch[trick].load(std::memory_order_relaxed) : 0.0f; };
        const float up = parameters[velocity_y], least = std::max(floor_of(trick_live), floor_of(trick_then));
        if (least > 0 && least <= 20.0f && std::isfinite(up) && up > 0.05f && up < least) parameters[velocity_y] = least;
    }
    if (ollie || grind) {
        h.flight_object.store(trajectory, std::memory_order_relaxed);
        h.flight_up.store(parameters[velocity_y], std::memory_order_relaxed);
        h.flight_tick.store(GetTickCount64(), std::memory_order_relaxed);
        h.raise_to.store(0, std::memory_order_relaxed);
        h.flight_kept = keep_floats(parameters, h.flight_parameters.data(), h.flight_parameters.size());
    }
    if (trajectory) note_flight_record(trajectory);
    if (const auto original = h.original.load(std::memory_order_acquire)) original(trajectory, parameters);
}
bool known_bytes(std::uintptr_t at, const unsigned char *bytes, std::size_t size) {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(at), bytes, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool known_code(std::uintptr_t base) {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(base + set_trajectory), set_trajectory_prefix.data(), set_trajectory_prefix.size()) == 0 &&
               std::memcmp(reinterpret_cast<const void *>(base + no_comply_call), no_comply_call_bytes.data(), no_comply_call_bytes.size()) == 0 &&
               std::memcmp(reinterpret_cast<const void *>(base + boneless_call), boneless_call_bytes.data(), boneless_call_bytes.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool start_trick_heights(std::uintptr_t base) noexcept {
    auto &h = hooked();
    if (h.attempted || !base) return h.ready;
    h.attempted = true;
    try {
        if (!known_code(base)) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                           "Trainer: this game build's trick code is not the known one; no comply and boneless heights are off.");
            return false;
        }
        h.base = base;
        h.ollie_known = known_bytes(base + ollie_call, ollie_call_bytes.data(), ollie_call_bytes.size());
        auto *target = reinterpret_cast<void *>(base + set_trajectory);
        void *original{};
        if (hook_prepare(target, reinterpret_cast<void *>(&set_trajectory_hook), &original) != HookOk) return false;
        if (!original) {
            (void)hook_remove(target);
            return false;
        }
        h.original.store(reinterpret_cast<SetTrajectory>(original), std::memory_order_release);
        if (hook_enable(target) != HookOk) return false;
        set_physics_step_callback(&raise_step);
        h.ready = true;
        logging::write(logging::Level::info, logging::Channel::skater, "Trainer: no comply and boneless heights ready.");
    } catch (...) {}
    return h.ready;
}
// Gravity on foot. A skater off the board falls under a gravity of its own, which the game writes from one instruction
// (mov dword ptr [rbx + 0x934], -9.8). Measured (2026-10-12): at -4.9 a drop on foot fell at 4.4 m/s2. The trainer changes
// that number, after checking the instruction is the known one, and puts it back at x 1.
namespace {
constexpr std::uintptr_t foot_gravity = 0x4834a95; // the instruction's number; the instruction starts 6 bytes before
constexpr std::array<unsigned char, 6> foot_gravity_bytes{0xc7, 0x83, 0x34, 0x09, 0x00, 0x00};
constexpr float foot_gravity_stock = -9.8f;
std::atomic<int> gravity_known{}; // 0 not looked, 1 the instruction is the known one, -1 it is not
std::atomic<float> gravity_now{1};
bool read_constant(std::uintptr_t at, float &value) noexcept {
    __try {
        value = *reinterpret_cast<const float *>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_constant(std::uintptr_t at, float value) noexcept {
    DWORD old{};
    if (!VirtualProtect(reinterpret_cast<void *>(at), sizeof(float), PAGE_EXECUTE_READWRITE, &old)) return false;
    bool done = false;
    __try {
        *reinterpret_cast<float *>(at) = value;
        done = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD again{};
    VirtualProtect(reinterpret_cast<void *>(at), sizeof(float), old, &again);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), sizeof(float));
    return done;
}
} // namespace
bool set_foot_gravity(float factor) noexcept {
    const auto base = hooked().base;
    if (!base) return false;
    if (!std::isfinite(factor) || factor < 0.02f || factor > 20.0f) factor = 1.0f;
    int known = gravity_known.load(std::memory_order_relaxed);
    if (known == 0) {
        float value{};
        known = known_bytes(base + foot_gravity - foot_gravity_bytes.size(), foot_gravity_bytes.data(), foot_gravity_bytes.size()) &&
                read_constant(base + foot_gravity, value) && value == foot_gravity_stock ? 1 : -1;
        gravity_known.store(known, std::memory_order_relaxed);
        if (known < 0)
            logging::write(logging::Level::warning, logging::Channel::skater, "Trainer: this game build's on-foot gravity is not where it is known to be; gravity acts on the board only.");
    }
    if (known < 0) return false;
    if (gravity_now.load(std::memory_order_relaxed) == factor) return true;
    if (!write_constant(base + foot_gravity, foot_gravity_stock * factor)) return false;
    gravity_now.store(factor, std::memory_order_relaxed);
    return true;
}
float foot_gravity_in_force() noexcept { return gravity_now.load(std::memory_order_relaxed); }
// The speed limit on foot. The game cuts a walking skater's speed along the ground to one number of its own (10 m/s),
// read by that code alone (FollowGround, 0x48318a0); the walkable slope angles 60, 55 and 75 follow it, which is how it
// is recognised. Measured (2026-10-13): sprint speed 13 ran at 10.0 m/s with the game's own limit.
namespace {
constexpr std::uintptr_t foot_speed_cap = 0x65e8238;
constexpr float foot_speed_cap_stock = 10.0f;
std::atomic<int> cap_known{};
std::atomic<float> cap_now{foot_speed_cap_stock};
} // namespace
bool set_foot_speed_cap(float metres_per_second) noexcept {
    const auto base = hooked().base;
    if (!base) return false;
    if (!std::isfinite(metres_per_second) || metres_per_second < foot_speed_cap_stock) metres_per_second = foot_speed_cap_stock;
    metres_per_second = std::min(metres_per_second, 1000.0f);
    int known = cap_known.load(std::memory_order_relaxed);
    if (known == 0) {
        float value[4]{};
        known = read_constant(base + foot_speed_cap, value[0]) && read_constant(base + foot_speed_cap + 4, value[1]) && read_constant(base + foot_speed_cap + 8, value[2]) &&
                read_constant(base + foot_speed_cap + 12, value[3]) && value[0] == foot_speed_cap_stock && value[1] == 60.0f && value[2] == 55.0f && value[3] == 75.0f ? 1 : -1;
        cap_known.store(known, std::memory_order_relaxed);
    }
    if (known < 0) return false;
    if (cap_now.load(std::memory_order_relaxed) == metres_per_second) return true;
    DWORD old{};
    const auto at = base + foot_speed_cap;
    if (!VirtualProtect(reinterpret_cast<void *>(at), sizeof(float), PAGE_READWRITE, &old)) return false;
    bool done = false;
    __try {
        *reinterpret_cast<float *>(at) = metres_per_second;
        done = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD again{};
    VirtualProtect(reinterpret_cast<void *>(at), sizeof(float), old, &again);
    if (done) cap_now.store(metres_per_second, std::memory_order_relaxed);
    return done;
}
void set_trick_heights(float no_comply, float boneless) noexcept {
    const auto speed = [](float height) { return std::isfinite(height) && height > 0 ? std::sqrt(std::min(height, 1.0e6f)) : 1.0f; };
    hooked().no_comply.store(speed(no_comply), std::memory_order_relaxed);
    hooked().boneless.store(speed(boneless), std::memory_order_relaxed);
}
// The trajectory object, as floats (read after the setter, 2026-10-10): the start point at 44..46, the launch velocity
// at 48..50, gravity at 52..54 and the step at 57 (0..2 looks like where it will come down).
bool raise_flight(float up, float &was, float &time) noexcept {
    auto &h = hooked();
    was = h.flight_up.load(std::memory_order_relaxed);
    const auto age = GetTickCount64() - h.flight_tick.load(std::memory_order_relaxed);
    time = static_cast<float>(age) / 1000.0f;
    if (!h.flight_object.load(std::memory_order_relaxed) || age > 250 || !(was > 0.05f) || !(up > was + 0.05f) || up > 20.0f) return false;
    h.raise_to.store(up, std::memory_order_release);
    return true;
}
int take_flight_raise() noexcept { return hooked().raised.exchange(0, std::memory_order_acq_rel); }
void set_flip_launch(std::uint32_t trick, float up) noexcept {
    auto &h = hooked();
    if (trick < h.flip_launch.size()) h.flip_launch[trick].store(std::isfinite(up) && up > 0 ? up : 0.0f, std::memory_order_relaxed);
}
void set_launch_trick(std::uint32_t trick, std::uintptr_t at) noexcept {
    hooked().launch_trick.store(trick, std::memory_order_relaxed);
    hooked().trick_at.store(at, std::memory_order_relaxed);
}
void set_grind_side(float side, float travel_x, float travel_z, bool grinding, float push, float nudge) noexcept {
    auto &h = hooked();
    h.grind_side.store(std::isfinite(side) ? side : 1.0f, std::memory_order_relaxed);
    h.grind_push.store(std::isfinite(push) ? push : 0.0f, std::memory_order_relaxed);
    if (grinding && std::isfinite(travel_x) && std::isfinite(travel_z)) {
        h.travel_x.store(travel_x, std::memory_order_relaxed);
        h.travel_z.store(travel_z, std::memory_order_relaxed);
        // The stick as it was on the grind: the game's scripts put the state back to nothing when the grind ends.
        h.grind_nudge.store(std::isfinite(nudge) ? nudge : 0.0f, std::memory_order_relaxed);
    }
}
bool take_grind_pop(float &along, float &side, float &nudge) noexcept {
    auto &h = hooked();
    const auto serial = h.pop_serial.load(std::memory_order_acquire);
    if (serial == h.pop_taken.exchange(serial, std::memory_order_relaxed)) return false;
    along = h.pop_along.load(std::memory_order_relaxed);
    side = h.pop_side.load(std::memory_order_relaxed);
    nudge = h.pop_nudge.load(std::memory_order_relaxed);
    return true;
}
void set_manual_pop(float height, std::int32_t intent) noexcept {
    auto &h = hooked();
    h.manual.store(std::isfinite(height) && height > 0 ? std::sqrt(std::min(height, 1.0e6f)) : 1.0f, std::memory_order_relaxed);
    h.manual_intent.store(intent, std::memory_order_relaxed);
    // The intent goes back to none as the stick is pulled for the pop: the launch comes within half a second.
    if (intent > 0) h.manual_until.store(GetTickCount64() + 600, std::memory_order_relaxed);
}
bool take_trick_launch(TrickLaunch &out) noexcept {
    auto &h = hooked();
    const auto written = h.written.load(std::memory_order_acquire);
    auto read = h.read.load(std::memory_order_relaxed);
    if (read == written) return false;
    if (written - read > h.seen.size()) read = written - static_cast<std::uint32_t>(h.seen.size());
    const auto packed = h.seen[read % h.seen.size()].load(std::memory_order_acquire);
    h.read.store(read + 1, std::memory_order_relaxed);
    const std::uint32_t bits[2]{static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed >> 1) & 0x7fffffffu};
    out.trick = packed & 1 ? Trick::boneless : Trick::no_comply;
    std::memcpy(&out.up_speed, &bits[0], 4);
    std::memcpy(&out.factor, &bits[1], 4);
    return true;
}
void set_trajectory_modifier(TrajectoryModifier modifier) noexcept {
    hooked().modifier.store(modifier, std::memory_order_release);
}
} // namespace dingosdk::trainer
