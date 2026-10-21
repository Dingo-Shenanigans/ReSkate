#include "turnaround_stage.h"
#include "turnaround.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dingosdk::turnaround {
namespace {
// The Board screen turns its board on the stick, as the player does: the turnaround holds the
// stick over for the whole turn, through a stand-in controller when none is connected
// (DingoSDKOverlaySetVirtualPad), and gives the gamepad filter back afterwards.
std::atomic<std::int16_t> spin_value{};
std::atomic<std::uint8_t> spin_axis{2};
void spin_filter(unsigned, overlay::GamepadValues& pad) noexcept {
    const auto v = spin_value.load();
    switch (spin_axis.load()) {
    case 0: pad.left_x = v; break;
    case 1: pad.left_y = v; break;
    case 3: pad.right_y = v; break;
    default: pad.right_x = v; break;
    }
}

class StickStage final : public Stage {
public:
    StickStage(float spin, std::uint8_t axis, bool skater) : spin_(spin), axis_(axis), skater_(skater) {}
    bool begin(std::string& why) override {
        if (std::abs(spin_) < 0.05f) {
            why = "The spin is set too gently to turn the board.";
            return false;
        }
        hold(1);
        spin_axis.store(axis_);
        previous_ = DingoSDKOverlayGamepadFilter();
        DingoSDKOverlaySetGamepadFilter(&spin_filter);
        DingoSDKOverlaySetVirtualPad(true);
        // The board's shadow on the backdrop would be cut out with it: shadows off while it turns.
        queue_console(skater_ ? "turnaround shadows off skater" : "turnaround shadows off");
        held_ = true;
        return true;
    }
    void pose(double, bool) override {}
    // Eased off by at most half each time: how the game turns the stick into a speed is not known
    // (a curve), and easing off too far at once leaves the board standing still. Never into the
    // stick's dead zone (XInput's is about a quarter of the way): there nothing turns at all.
    bool slow_down(double ratio) override {
        constexpr float least = 0.3f;
        const float now = std::abs(spin_) * strength_;
        const float next = std::max(least, now * static_cast<float>(std::clamp(ratio * 1.1, 0.5, 1.0)));
        if (next >= now - 0.005f) return false;
        faster_ = strength_;
        strength_ = next / std::abs(spin_);
        hold(strength_);
        return true;
    }
    // Back to the last speed that turned it.
    bool speed_up() override {
        if (faster_ <= strength_) return false;
        strength_ = faster_;
        hold(strength_);
        return true;
    }
    void hold(float strength) {
        const float value = std::clamp(spin_ * std::clamp(strength, 0.0f, 1.0f), -1.0f, 1.0f);
        spin_value.store(static_cast<std::int16_t>(std::lround(value * 32767)));
    }
    bool changes_background() const override { return false; }
    bool continuous() const override { return true; }
    void end() override {
        if (!held_) return;
        held_ = false;
        DingoSDKOverlaySetVirtualPad(false);
        DingoSDKOverlaySetGamepadFilter(previous_ == &spin_filter ? nullptr : previous_);
        queue_console("turnaround shadows restore");
    }
    ~StickStage() override { end(); }

private:
    float spin_;
    std::uint8_t axis_;
    bool skater_;
    float strength_{1}, faster_{1};
    overlay::GamepadFilter previous_{};
    bool held_{};
};
} // namespace

std::shared_ptr<Stage> board_stage(const Options& options) {
    return std::make_shared<StickStage>(options.spin, options.spin_axis, options.subject == Subject::skater);
}
} // namespace dingosdk::turnaround
