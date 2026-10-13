#pragma once
#include "controller.h"
#include <algorithm>
#include <array>
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
            poses_[0] = {point, now}; pose_count_ = 1;
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
                // A confirmed stop must not mix the preceding moving trace
                // into its next velocity estimate. Preserve the actual last
                // change time rather than shrinking it on repeated reads.
                poses_[0] = {baseline_, baseline_at_}; pose_count_ = 1;
            }
            return result_;
        }
        if (seconds < 1.0/240) return result_;
        if (seconds > .15 && stopped_) {
            // The first pose after a long verified rest starts a fresh motion
            // baseline. Keep this actual surface observation, never derive a
            // velocity across the entire parked interval.
            baseline_ = point; baseline_at_ = now; stopped_ = false;
            poses_[0] = {point, now}; pose_count_ = 1;
            result_ = {true, point, {}, now}; return result_;
        }
        if (seconds > .15 || !std::isfinite(distance)) {
            baseline_ = point; baseline_at_ = now; result_ = {};
            poses_[0] = {point, now}; pose_count_ = 1;
            stopped_ = false;
            return {};
        }
        std::size_t retained{};
        for (std::size_t i = 0; i < pose_count_; ++i)
            if (now-poses_[i].now <= .15) poses_[retained++] = poses_[i];
        pose_count_ = retained;
        // Native poses may publish slowly and be observed late, then on time.
        // A short adjacent callback interval is not a physical speed burst.
        // Span at least one 30 Hz pose period when available, with the first
        // bounded pair as startup. This keeps ordinary 30 Hz turning response
        // intact. Every output retains its actual point/time.
        const Pose* anchor = &poses_[0];
        for (std::size_t i = pose_count_; i > 0; --i)
            if (now-poses_[i-1].now+1e-9 >= 1./30) { anchor = &poses_[i-1]; break; }
        const auto span = now-anchor->now;
        const auto dx = static_cast<double>(point.x)-anchor->point.x;
        const auto dy = static_cast<double>(point.y)-anchor->point.y;
        const auto dz = static_cast<double>(point.z)-anchor->point.z;
        if (span <= 0 || span > .15 || std::hypot(dx, dy, dz)/span > 45) {
            baseline_ = point; baseline_at_ = now; result_ = {};
            poses_[0] = {point, now}; pose_count_ = 1; stopped_ = false;
            return {};
        }
        result_ = {true, point, {static_cast<float>(dx/span),
            static_cast<float>(dy/span), static_cast<float>(dz/span)}, now};
        if (pose_count_ == poses_.size()) {
            std::move(poses_.begin()+1, poses_.end(), poses_.begin()); --pose_count_;
        }
        poses_[pose_count_++] = {point, now};
        baseline_ = point; baseline_at_ = now;
        stopped_ = false;
        return result_;
    }
private:
    struct Pose { Vec3 point{}; double now{}; };
    std::array<Pose, 40> poses_{};
    std::size_t pose_count_{};
    bool have_{}, stopped_{};
    double seen_{}, baseline_at_{};
    Vec3 baseline_{};
    ContactMotionSample result_{};
};
}
