#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace dingosdk::anim_studio {
struct ContactWindow {
    std::optional<double> since;
    bool observe(double now, bool contact) noexcept {
        if (!contact) { since.reset(); return false; }
        if (!since) since = now;
        return now - *since >= 1.0 / 30.0;
    }
};
struct GameplayClipClock {
    float time{-1}, air_rate{1}, landing_time{};
    double at{-1};
    std::optional<double> landing_started;
    void reset() noexcept { *this = {}; }
    float advance_air(double now, float start, float rate, float limit) noexcept {
        if (!std::isfinite(now) || !std::isfinite(start) || !std::isfinite(rate) || rate < 0 || !std::isfinite(limit)) return time;
        if (at >= 0 && now < at) return time;
        const double next = at < 0 || time < 0 ? start : time + (now - at) * rate;
        const float shown = static_cast<float>(std::min(static_cast<double>(limit), next));
        observe_air(now, shown, shown >= limit ? 0.0f : rate);
        return shown;
    }
    void observe_air(double now, float source_time, float rate) noexcept {
        if (!std::isfinite(now) || !std::isfinite(source_time) || !std::isfinite(rate)) return;
        at = now; time = source_time; air_rate = std::max(0.0f, rate); landing_started.reset();
    }
    float land(double now, float speed, float duration, float fallback) noexcept {
        if (!std::isfinite(now) || !std::isfinite(speed) || speed <= 0 || !std::isfinite(duration) || duration < 0) return time;
        if (at < 0 || time < 0) { at = now; time = std::clamp(fallback, 0.0f, duration); air_rate = speed; return time; }
        if (now < at) return time;
        if (!landing_started) { landing_started = at; landing_time = time; }
        // Continue from the last airborne sample, not clip.landing + contact age.
        // Integrate a smooth rate ramp exactly, including callbacks spanning the whole ramp.
        constexpr double ramp = 0.08;
        const double elapsed = std::max(0.0, now - *landing_started), u = std::clamp(elapsed / ramp, 0.0, 1.0);
        const double head = ramp * (air_rate * u + (speed - air_rate) * (u*u*u - 0.5*u*u*u*u));
        const double advance = head + speed * std::max(0.0, elapsed - ramp);
        time = static_cast<float>(std::min(static_cast<double>(duration), landing_time + advance));
        at = now;
        return time;
    }
};
inline int playback_pop_rank(const std::string& wanted, const std::string& saved) noexcept {
    // A different source pop has different native body/board motion. Only an
    // explicitly generic binding may substitute for the requested height.
    return saved == wanted ? 0 : saved.empty() ? 1 : -1;
}
inline bool episode_selection_changed(std::string_view selected, std::string_view reported,
                                      std::string_view selected_pop = {}, std::string_view wanted_pop = {}) noexcept {
    // Blank names are transient state. An explicit pop binding must be reconsidered
    // when the native grade refines an estimate; generic bindings remain stable.
    return !reported.empty() && (selected != reported || (!selected_pop.empty() && selected_pop != wanted_pop));
}
inline bool riding_switch(bool left_foot_leads, bool goofy) noexcept { return left_foot_leads == goofy; }
inline bool playback_stance_compatible(bool recorded_goofy, std::optional<bool> recorded_switch,
                                      bool live_goofy, std::optional<bool> live_switch) noexcept {
    // Equal lead feet are not enough: regular switch and goofy normal have different
    // placement/bind frames. Raw local-bone reflection is not a valid retargeter.
    // Legacy files predate switch metadata and retain their normal-stance assumption.
    return live_switch.has_value() && recorded_goofy == live_goofy &&
           recorded_switch.value_or(false) == *live_switch;
}
inline float advance_time(float time, float elapsed, float speed, float duration, bool loop) noexcept {
    if (!std::isfinite(elapsed) || elapsed <= 0 || !std::isfinite(speed) || speed <= 0 ||
        !std::isfinite(duration) || duration <= 0) return time;
    const double next = static_cast<double>(time) + static_cast<double>(elapsed) * speed;
    return static_cast<float>(loop && next > duration ? std::fmod(next, duration) : std::min(next, static_cast<double>(duration)));
}
inline std::string variation_name(std::string base, unsigned number) {
    const auto suffix = " " + std::to_string(number);
    base.resize(std::min(base.size(), std::size_t{48} - suffix.size()));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    return base + suffix;
}
inline bool fresh_sample(double now, double captured, double previous) noexcept {
    return std::isfinite(captured) && captured > previous && captured <= now && now - captured < 0.1;
}
} // namespace dingosdk::anim_studio
