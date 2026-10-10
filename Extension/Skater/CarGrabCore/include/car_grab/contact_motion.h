#pragma once
#include "controller.h"
#include <cmath>

namespace car_grab {
// Paired observations of the SAME locked car-local surface point. Turning
// velocity belongs to this point, rather than to the vehicle origin.
struct ContactMotionSample {
    bool valid{};
    Vec3 point{}, velocity{};
    double sampled_at{};
};
class ContactMotion {
public:
    void reset() noexcept { *this = {}; }
    ContactMotionSample sample(double now, Vec3 point) noexcept {
        const auto finite = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!std::isfinite(now) || now <= 0 || !finite(point)) { reset(); return {}; }
        if (!have_ || now <= seen_ || now - seen_ > .15) {
            have_ = true; seen_ = baseline_at_ = now; baseline_ = point; result_ = {};
            stopped_ = false;
            return {};
        }
        seen_ = now;
        const auto seconds = now - baseline_at_;
        const Vec3 delta{point.x-baseline_.x, point.y-baseline_.y, point.z-baseline_.z};
        const auto distance = std::hypot(delta.x, delta.y, delta.z);
        if (distance < .00001) {
            if (seconds >= .05) {
                result_ = {true, point, {}, now};
                stopped_ = true;
            }
            return result_;
        }
        if (seconds < 1.0/240) return result_;
        if (seconds > .15 && stopped_) {
            // The first pose after a long verified rest starts a fresh motion
            // baseline. Keep this actual surface observation, never derive a
            // velocity across the entire parked interval.
            baseline_ = point; baseline_at_ = now; stopped_ = false;
            result_ = {true, point, {}, now}; return result_;
        }
        if (seconds > .15 || !std::isfinite(distance) || distance/seconds > 45) {
            baseline_ = point; baseline_at_ = now; result_ = {};
            stopped_ = false;
            return {};
        }
        result_ = {true, point, {static_cast<float>(delta.x/seconds),
            static_cast<float>(delta.y/seconds), static_cast<float>(delta.z/seconds)}, now};
        baseline_ = point; baseline_at_ = now;
        stopped_ = false;
        return result_;
    }
private:
    bool have_{}, stopped_{};
    double seen_{}, baseline_at_{};
    Vec3 baseline_{};
    ContactMotionSample result_{};
};
}
