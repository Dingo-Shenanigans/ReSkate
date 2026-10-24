#include "skater_push.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dingosdk::skater_push {
namespace {
// A queued request is not delivery: bail/teleport physics may still be busy or have replaced the
// body, so a push stays pending until the physics step acknowledges it, or two seconds pass.
struct Delivery {
    std::mutex mutex;
    std::optional<Vec3> pending;
    std::uint64_t until{};
    std::atomic<bool> held{};
    void clear() noexcept { pending.reset(); until = 0; }
};
Delivery& delivery() { static auto* value = new Delivery; return *value; }
} // namespace
bool valid(const Vec3& velocity) noexcept {
    return std::all_of(velocity.begin(), velocity.end(), [](float n) { return std::isfinite(n); }) && velocity[1] == 0.f &&
           std::hypot(velocity[0], velocity[2]) <= max_speed + .001f;
}
void request(const Vec3& velocity) noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    d.clear();
    if (valid(velocity)) { d.pending = velocity; d.until = GetTickCount64() + 2000; }
}
void wait() noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    const auto now = GetTickCount64();
    if (d.pending && now < d.until) d.until = now + 2000;
}
void reset() noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    d.clear();
}
void hold(bool on) noexcept { delivery().held.store(on, std::memory_order_release); }
bool holding() noexcept { return delivery().held.load(std::memory_order_acquire); }
bool pending() noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    if (GetTickCount64() >= d.until) d.clear();
    return d.pending.has_value();
}
std::optional<Vec3> peek() noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    if (GetTickCount64() >= d.until) d.clear();
    // A pending push wins over the hold: "hold still, then push" needs no release in between.
    if (d.pending) return d.pending;
    return holding() ? std::optional(Vec3{}) : std::nullopt;
}
void accepted(const Vec3& velocity) noexcept {
    auto& d = delivery();
    std::lock_guard lock(d.mutex);
    if (d.pending && *d.pending == velocity) d.clear();
}
} // namespace dingosdk::skater_push
