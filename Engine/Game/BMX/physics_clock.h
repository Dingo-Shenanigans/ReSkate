#pragma once
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx {
// A render/client tick may see the same native simulation step repeatedly. Gesture
// recognition and physics must advance only by scaled native time, never wall time.
class PhysicsClock {
public:
    void reset() noexcept { ready_ = false; }
    bool discontinuity() const noexcept { return discontinuity_; }
    // Cumulative time comes from No Bail's additive per-physics-step observer. A
    // variable timestep must be summed at its source, not multiplied by the last dt.
    double sample_time(std::uint64_t serial, double total_seconds) noexcept {
        discontinuity_ = false;
        if (!std::isfinite(total_seconds) || total_seconds < 0) { ready_ = false; discontinuity_ = true; return 0; }
        if (!ready_) { serial_ = serial; total_ = total_seconds; ready_ = true; return 0; }
        const auto count = serial - serial_;
        const double elapsed = total_seconds - total_;
        serial_ = serial; total_ = total_seconds;
        if (count > 12 || elapsed < 0 || elapsed > .1 || (!count && elapsed != 0)) {
            discontinuity_ = true; return 0;
        }
        return elapsed;
    }
    double sample(std::uint32_t step, double step_seconds) noexcept {
        discontinuity_ = false;
        if (!ready_) { step_ = step; ready_ = true; return 0; }
        const auto count = step - step_;
        step_ = step;
        if (!std::isfinite(step_seconds) || step_seconds < 0 || step_seconds > .1 || count > 12) { discontinuity_ = true; return 0; }
        const double elapsed = count * step_seconds;
        if (elapsed > .1) { discontinuity_ = true; return 0; }
        return elapsed;
    }
private:
    std::uint32_t step_{};
    std::uint64_t serial_{};
    double total_{};
    bool ready_{}, discontinuity_{};
};
}
