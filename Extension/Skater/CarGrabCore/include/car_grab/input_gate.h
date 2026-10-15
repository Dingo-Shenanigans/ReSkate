#pragma once
#include <cstdint>
#include <cmath>

namespace car_grab {
struct GrabInputSample {
    bool active{}, held{}, keyboard{};
    std::uint32_t device{};
};
// Native focus/ownership checks produce these value samples; no engine access.
class GrabInputGate {
public:
    void invalidate() noexcept { require_release_ = true; }
    bool observe(GrabInputSample fresh, GrabInputSample published) noexcept {
        if (!fresh.active || !published.active) { invalidate(); return false; }
        // An explicit focused G press can choose keyboard input even after
        // a pad disconnect. It still needs a subsequent real release to rearm.
        if (fresh.held && fresh.keyboard) keyboard_release_pending_ = true;
        if (!fresh.held) {
            // A missing pad synthesizes zero buttons. That is not a release.
            // A keyboard grab remains independent of unrelated pad changes.
            if (!keyboard_grab_ && !keyboard_release_pending_ &&
                (fresh.device != published.device || (controller_grab_ && !fresh.device))) {
                invalidate(); return false;
            }
            require_release_ = false;
            if (keyboard_release_pending_) { keyboard_grab_ = true; controller_grab_ = false; }
            keyboard_release_pending_ = false;
            return false;
        }
        if (fresh.held && !fresh.keyboard && (!fresh.device || fresh.device != published.device)) {
            invalidate(); return false;
        }
        // Client publication establishes a recent trusted device/focus lease.
        // Physics observes the latest button state; the older button sample
        // must not swallow a press or delay a release by another client tick.
        if (require_release_) return false;
        keyboard_grab_ = fresh.keyboard;
        controller_grab_ = !fresh.keyboard;
        return true;
    }
private:
    bool require_release_ = true;
    bool keyboard_grab_{}, controller_grab_{};
    bool keyboard_release_pending_{};
};

// Retains only a private attachment choice while current motion is unavailable.
// A true result never authorizes a body or animation write.
class GrabContinuity {
public:
    void clear() noexcept { *this = {}; }
    void remember(double now, std::uint64_t world, std::uint64_t skater,
                  std::uint64_t vehicle) noexcept;
    bool can_wait(double now, std::uint64_t world, std::uint64_t skater,
                  std::uint64_t vehicle, bool held, bool identity_verified) const noexcept;
private:
    double last_ready_{};
    std::uint64_t world_{}, skater_{}, vehicle_{};
};
inline void GrabContinuity::remember(double now, std::uint64_t world, std::uint64_t skater,
                                     std::uint64_t vehicle) noexcept {
    clear();
    if (!std::isfinite(now) || now <= 0 || !world || !skater || !vehicle) return;
    last_ready_ = now; world_ = world; skater_ = skater; vehicle_ = vehicle;
}
inline bool GrabContinuity::can_wait(double now, std::uint64_t world, std::uint64_t skater,
                                     std::uint64_t vehicle, bool held, bool identity_verified) const noexcept {
    const auto elapsed = now - last_ready_;
    return held && identity_verified && vehicle_ && world == world_ && skater == skater_ &&
        vehicle == vehicle_ && std::isfinite(elapsed) && elapsed >= 0 && elapsed <= .35;
}
}
