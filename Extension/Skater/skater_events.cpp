#include "skater_events.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Skater/skater_physics.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace dingosdk::skater_events {
namespace {
namespace sstate = addr::skater_state;
constexpr std::size_t most_kept = 32;
constexpr std::size_t cause_count = 35;
constexpr float heard_hit = 3.0f; // m/s: lighter touches are not news (Board Break's body_hit)
constexpr float fall_at_least = 0.4f, fall_at_most = 3.0f;
constexpr std::size_t most_steps = 512;
std::atomic<bool> steps_wanted{};

struct Shared {
    std::mutex mutex;
    std::deque<Landing> landings;
    std::deque<BodyHit> hits;
    std::deque<Fall> falls;
    std::deque<StepNews> steps;
    std::array<float, 3> velocity{};
    std::optional<std::array<float, 3>> spin;
    std::uint64_t velocity_at{};
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
struct SlamLatch {
    std::atomic<float> force{};
    std::atomic<std::int32_t> cause{-1};
    std::atomic<std::uint64_t> at{};
};
SlamLatch& slam() {
    static SlamLatch value;
    return value;
}
template <class T> void keep(std::deque<T>& list, T value) {
    if (list.size() >= most_kept) list.pop_front();
    list.push_back(std::move(value));
}
bool left_limb(skater_body::Bone bone) {
    using B = skater_body::Bone;
    switch (bone) {
    case B::left_hand: case B::left_forearm: case B::left_arm: case B::left_shoulder: case B::left_toe: case B::left_foot: case B::left_leg:
    case B::left_upleg: return true;
    default: return false;
    }
}
bool right_limb(skater_body::Bone bone) {
    using B = skater_body::Bone;
    switch (bone) {
    case B::right_hand: case B::right_forearm: case B::right_arm: case B::right_shoulder: case B::right_toe: case B::right_foot: case B::right_leg:
    case B::right_upleg: return true;
    default: return false;
    }
}
// The board's root body velocity (as the grind exit control and Board Break read it), and its spin
// (angular velocity, radians a second) when that reads as one.
bool read_board_velocity(const NoBailSkater& skater, std::array<float, 3>& velocity, std::optional<std::array<float, 3>>* spin = nullptr) noexcept {
    std::uintptr_t holder{}, physics{}, type{}, bodies{}, owner{};
    std::uint32_t count{};
    if (!memory::peek(skater.core + sstate::board_holder_offset, holder) || !memory::peek(holder + sstate::board_physics_offset, physics) ||
        !memory::peek(physics, type) || type != skater.base + sstate::board_physics_vtable ||
        !memory::peek(physics + sstate::physics_bodies_offset, bodies) || !memory::peek(bodies, count) || count != sstate::board_body_count)
        return false;
    const auto body = bodies + sstate::board_root_body * sstate::physics_body_size;
    if (!memory::peek(body + sstate::body_owner_offset, owner) || owner != physics || !memory::peek(body + sstate::body_velocity_offset, velocity))
        return false;
    const auto sane = [](const std::array<float, 3>& value) { return std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1000; }); };
    if (spin) {
        std::array<float, 3> turning{};
        spin->reset();
        if (memory::peek(body + sstate::body_spin_offset, turning) && sane(turning)) *spin = turning;
    }
    return sane(velocity);
}

// The physics thread's own.
struct Watch {
    bool was_air{};
    float air_down{}, air_speed{};
    std::array<std::uint64_t, skater_body::count> hit_at{};
    std::uint64_t any_hit_at{};
    std::optional<Fall> fall;
    std::uint64_t fall_since{};
    bool was_wipeout{};
};
void on_step(const NoBailSkater& skater, float seconds, bool wipeout) noexcept {
    static Watch w;
    const auto error = GetLastError();
    try {
        const auto now = GetTickCount64();
        auto& s = shared();
        std::uint32_t physics{};
        if (memory::peek(skater.context + sstate::physics_state_offset, physics)) {
            std::array<float, 3> v{};
            std::optional<std::array<float, 3>> spin;
            const bool known = read_board_velocity(skater, v, &spin);
            const bool air = physics == 200 || physics == 201, grinding = physics >= 400 && physics < 500,
                       rolling = physics >= 100 && physics < 200;
            if (known) {
                std::lock_guard lock(s.mutex);
                s.velocity = v;
                s.spin = spin;
                s.velocity_at = now;
            }
            if (air && known) {
                w.air_down = -v[1];
                w.air_speed = std::hypot(v[0], v[2]);
            }
            if (w.was_air && (rolling || grinding)) {
                std::lock_guard lock(s.mutex);
                keep(s.landings, Landing{w.air_down, w.air_speed, grinding});
            }
            w.was_air = air;
            // A fall starts as the wipeout does.
            const bool wiping = physics == 300;
            if (wiping && !w.was_wipeout && !w.fall) {
                w.fall = Fall{};
                w.fall_since = now;
            }
            w.was_wipeout = wiping;
        }
        if (skater_physics::available()) {
            const auto step = skater_physics::read_step(skater, seconds, wipeout);
            if (steps_wanted.load(std::memory_order_acquire)) {
                const double at = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard lock(s.mutex);
                if (s.steps.size() >= most_steps) s.steps.pop_front();
                s.steps.push_back({at, step});
            }
            const bool ragdoll = step.ragdoll.value_or(false);
            if (ragdoll) {
                std::optional<BodyHit> heard;
                for (std::size_t i = 1; i < skater_body::count; ++i) {
                    const auto& c = step.body.bodies[i];
                    if (!c.touching) continue;
                    const float slide = std::sqrt(c.slide[0] * c.slide[0] + c.slide[1] * c.slide[1] + c.slide[2] * c.slide[2]);
                    if (w.fall) {
                        w.fall->impact[i] += c.impact;
                        w.fall->slide[i] += std::isfinite(slide) ? slide * seconds : 0.0f;
                        const auto bone = static_cast<skater_body::Bone>(i);
                        if (left_limb(bone)) w.fall->left += c.impact;
                        if (right_limb(bone)) w.fall->right += c.impact;
                    }
                    if (heard || skater_body::foot(static_cast<skater_body::Bone>(i)) || c.impact < heard_hit) continue;
                    if (now - w.hit_at[i] < 220 || now - w.any_hit_at < 70) continue;
                    w.hit_at[i] = w.any_hit_at = now;
                    heard = BodyHit{static_cast<std::uint8_t>(i), c.impact, c.hit.vehicle, c.hit.board};
                }
                if (heard) {
                    std::lock_guard lock(s.mutex);
                    keep(s.hits, *heard);
                }
            }
            if (w.fall) {
                const float lasted = static_cast<float>(now - w.fall_since) / 1000.0f;
                if (lasted >= fall_at_most || (lasted >= fall_at_least && !ragdoll && step.ragdoll.has_value())) {
                    w.fall->seconds = lasted;
                    std::lock_guard lock(s.mutex);
                    keep(s.falls, *w.fall);
                    w.fall.reset();
                }
            }
        }
    } catch (...) {
    }
    SetLastError(error);
}
std::atomic<bool> listening{};
}

void want() noexcept {
    if (listening.exchange(true)) return;
    if (!add_no_bail_step_listener(&on_step)) {
        logging::write(logging::Level::warning, logging::Channel::skater,
                       "Plugins' skater events: No Bail has no room for another physics step listener.");
        listening.store(false);
    }
}
void want_steps() noexcept {
    want();
    steps_wanted.store(true, std::memory_order_release);
}
std::vector<StepNews> take_steps() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    std::vector<StepNews> out(s.steps.begin(), s.steps.end());
    s.steps.clear();
    return out;
}
std::vector<Landing> take_landings() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    std::vector<Landing> out(s.landings.begin(), s.landings.end());
    s.landings.clear();
    return out;
}
std::vector<BodyHit> take_hits() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    std::vector<BodyHit> out(s.hits.begin(), s.hits.end());
    s.hits.clear();
    return out;
}
std::vector<Fall> take_falls() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    std::vector<Fall> out(s.falls.begin(), s.falls.end());
    s.falls.clear();
    return out;
}
std::optional<std::array<float, 3>> board_velocity() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.velocity_at || GetTickCount64() - s.velocity_at > 250) return std::nullopt;
    return s.velocity;
}
std::optional<std::array<float, 3>> board_spin() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.velocity_at || GetTickCount64() - s.velocity_at > 250) return std::nullopt;
    return s.spin;
}
Slam last_slam() noexcept {
    auto& l = slam();
    return {l.force.load(std::memory_order_relaxed), l.cause.load(std::memory_order_relaxed), l.at.load(std::memory_order_acquire)};
}
void note_cause(std::int32_t reason, float magnitude) noexcept {
    auto& l = slam();
    const auto now = GetTickCount64();
    // The strongest of a burst: one slam records several causes.
    if (now - l.at.load(std::memory_order_relaxed) < 250 && magnitude <= l.force.load(std::memory_order_relaxed)) return;
    l.force.store(magnitude, std::memory_order_relaxed);
    l.cause.store(reason, std::memory_order_relaxed);
    l.at.store(now, std::memory_order_release);
}
void note_wipeout(std::uintptr_t causes) noexcept {
    std::array<std::uint8_t, cause_count> set{};
    std::array<float, cause_count> force{};
    std::int32_t reason{-1};
    float strongest{};
    if (causes && memory::peek_bytes(causes + 0x28, set.data(), set.size()) && memory::peek_bytes(causes + 0x4c, force.data(), sizeof(force)))
        for (std::size_t i = 0; i < cause_count; ++i)
            if (set[i] && (reason < 0 || force[i] > strongest)) {
                reason = static_cast<std::int32_t>(i);
                strongest = force[i];
            }
    if (reason < 0) return;
    auto& l = slam();
    l.force.store(strongest, std::memory_order_relaxed);
    l.cause.store(reason, std::memory_order_relaxed);
    l.at.store(GetTickCount64(), std::memory_order_release);
}
}
