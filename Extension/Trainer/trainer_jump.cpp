#include "trainer_jump.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/no_bail.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

namespace dingosdk::trainer {
namespace {
// Game build 25414733. void set_trajectory(trajectory, parameters): the launch velocity is
// the vector at parameters + 0x80. Measured with a scripted controller:
//  - a no comply calls it from 0x482f646 on entering the pop (state 200) after a crouch
//    (state 103) of about 0.20 s; an ollie crouches for 0.08 s and launches elsewhere;
//  - a boneless calls it from 0x47691f8 while the foot is planted (state 602).
constexpr std::uintptr_t set_trajectory = 0x4752aa0, no_comply_call = 0x482f646, boneless_call = 0x47691f8;
constexpr std::array<unsigned char, 32> set_trajectory_prefix{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x88, 0xbb, 0x00,
                                                              0x00, 0x00, 0x48, 0x8b, 0xd9, 0x0f, 0x85, 0xf6, 0x00, 0x00, 0x00,
                                                              0xc5, 0xfc, 0x28, 0x05, 0x62, 0x7d, 0xe8, 0x01, 0xc5, 0xfc};
constexpr std::array<unsigned char, 5> no_comply_call_bytes{0xe8, 0x55, 0x34, 0xf2, 0xff}, boneless_call_bytes{0xe8, 0xa3, 0x98, 0xfe, 0xff};
constexpr std::uint32_t crouch_state = 103, pop_state = 200, plant_state = 602;
constexpr float no_comply_crouch = 0.12f; // seconds: between an ollie's crouch and a no comply's
constexpr std::size_t velocity_y = 0x84 / sizeof(float);

using SetTrajectory = void (*)(std::uintptr_t, float *);
struct Hooked {
    std::atomic<SetTrajectory> original{};
    std::uintptr_t base{};
    std::atomic<float> no_comply{1}, boneless{1};
    std::atomic<TrajectoryModifier> modifier{};
    // The last few launches, for the trainer's log line: written by the physics thread, read
    // by the client tick.
    std::array<std::atomic<std::uint64_t>, 8> seen{};
    std::atomic<std::uint32_t> written{}, read{};
    bool attempted{}, ready{};
};
Hooked &hooked() {
    static auto *value = new Hooked;
    return *value;
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
    if (parameters && (caller == no_comply_call + 5 || caller == boneless_call + 5)) {
        const auto watch = watched_physics_state(); // the local skater's
        Trick trick{};
        if (caller == boneless_call + 5 && watch.valid && watch.state == plant_state) trick = Trick::boneless;
        else if (caller == no_comply_call + 5 && watch.valid && watch.state == pop_state && watch.previous == crouch_state &&
                 watch.previous_seconds >= no_comply_crouch)
            trick = Trick::no_comply;
        const float up = parameters[velocity_y];
        if (trick != Trick::none && std::isfinite(up) && up > 0.5f) {
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
    if (const auto original = h.original.load(std::memory_order_acquire)) original(trajectory, parameters);
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
        auto *target = reinterpret_cast<void *>(base + set_trajectory);
        void *original{};
        if (hook_prepare(target, reinterpret_cast<void *>(&set_trajectory_hook), &original) != HookOk) return false;
        if (!original) {
            (void)hook_remove(target);
            return false;
        }
        h.original.store(reinterpret_cast<SetTrajectory>(original), std::memory_order_release);
        if (hook_enable(target) != HookOk) return false;
        h.ready = true;
        logging::write(logging::Level::info, logging::Channel::skater, "Trainer: no comply and boneless heights ready.");
    } catch (...) {}
    return h.ready;
}
void set_trick_heights(float no_comply, float boneless) noexcept {
    const auto speed = [](float height) { return std::isfinite(height) && height > 0 ? std::sqrt(std::min(height, 1.0e6f)) : 1.0f; };
    hooked().no_comply.store(speed(no_comply), std::memory_order_relaxed);
    hooked().boneless.store(speed(boneless), std::memory_order_relaxed);
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
