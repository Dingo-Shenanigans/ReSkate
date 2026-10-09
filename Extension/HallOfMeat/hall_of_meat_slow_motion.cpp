#include "hall_of_meat_slow_motion.h"
#include "hall_of_meat_skater.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Game/Settings/simulation_time.h"
#include "Extension/Settings/named_settings.h"
#include <charconv>
#include <utility>

namespace dingosdk::hall_of_meat {
namespace {
constexpr const char* time_scale = "SimulationTime.TimeScale";
constexpr const char* sim_rate = "SimulationTime.ForceSimRate";
constexpr const char* max_sim_fps = "SimulationTime.MaxSimFps";
// The steps first, then the clock: no step of the change runs at the old rate on the new clock.
constexpr std::array<const char*, 3> held_settings{sim_rate, max_sim_fps, time_scale};
constexpr std::size_t time_scale_index = 2;

bool failed(const std::string& result) { return result.starts_with("error: "); }
// A float as the settings read it back (named_setting_value), so what was written is recognised.
std::string text(float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string{};
}
std::optional<float> number(const std::string& value) {
    float result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result)) return {};
    return result;
}
}

bool SlowMotion::still_ours() const {
    for (std::size_t index = 0; index < held_settings.size(); ++index)
        if (!held_->ours[index].empty() && named_setting_value(held_settings[index]) != held_->ours[index]) return false;
    return true;
}

bool SlowMotion::set(float speed) {
    if (!(speed < 1.0f)) yielded_ = false; // this slow motion is over; the next break may slow the game again
    if (!(speed < 1.0f) || yielded_ || multiplayer_session_active()) {
        release();
        return false;
    }
    // The game's own rate, as the skater's step still has it (it is the held rate's only while held).
    if (!base_rate_) {
        const auto own = simulation_time::rate_of(step_length());
        if (!own) return false;
        base_rate_ = own;
    }
    if (!held_) {
        Held found;
        for (std::size_t index = 0; index < held_settings.size(); ++index) {
            auto value = named_setting_value(held_settings[index]);
            if (!value) return false;
            found.found[index] = std::move(*value);
        }
        held_ = std::move(found);
    } else if (!still_ours()) {
        yielded_ = true; // someone else's now: what still holds ours goes back
        release();
        return false;
    }
    const auto found = number(held_->found[time_scale_index]);
    const auto scale = found ? slow_motion_time_scale(*found, speed) : std::nullopt;
    if (!scale) {
        release(); // the game already runs as slowly: the found speed stays, or comes back
        return false;
    }
    const auto rate = slow_motion_rate(*base_rate_, *scale);
    const std::array<std::string, 3> values{std::to_string(rate), std::to_string(rate), text(*scale)};
    for (std::size_t index = 0; index < held_settings.size(); ++index) {
        if (values[index] == held_->ours[index]) continue;
        if (values[index].empty() || failed(change_named_setting(held_settings[index], values[index], false))) {
            release(); // all three or none
            return false;
        }
        held_->ours[index] = values[index];
    }
    // The skater steps as the simulation now does (a skater rebuilt meanwhile already does).
    if (set_step_length(simulation_time::step_seconds(rate))) skater_changed_ = true;
    return true;
}

void SlowMotion::release() {
    if (held_) {
        // Back to what was found; a setting that no longer holds ours was changed from elsewhere and stays.
        // Written back like any change, so an override the slow motion found (the trainer's) stays theirs,
        // and one it made itself ends where the value is the game's own again.
        for (std::size_t index = held_settings.size(); index-- > 0;)
            if (!held_->ours[index].empty() && named_setting_value(held_settings[index]) == held_->ours[index])
                (void)change_named_setting(held_settings[index], held_->found[index], false);
        held_.reset();
    }
    // The skater's own step back; while no skater can be reached (a teleport under way), the next call
    // tries again, and the game's own rate is kept for it until then.
    if (skater_changed_ && base_rate_ && set_step_length(simulation_time::step_seconds(*base_rate_))) skater_changed_ = false;
    if (!skater_changed_) base_rate_.reset();
}
}
