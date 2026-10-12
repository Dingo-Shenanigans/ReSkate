#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>

namespace dingosdk::server {
class Suspicion {
  public:
    enum class Signal : std::uint8_t {
        object_on_player,
        object_animation,
        object_flood,
        bone_reach,
        bone_scale,
        pinned_board,
        speed,
        name_spoof,
        count
    };
    static constexpr std::size_t signals = static_cast<std::size_t>(Signal::count);
    static constexpr std::array<std::string_view, signals> names{
        "objects on players", "animated objects", "object floods", "stretched skater", "resized skater",
        "board on players", "game speed", "spoofed name",
    };
    static constexpr std::array<double, signals> weights{2.0, 1.5, 6.0, 2.0, 0.5, 10.0, 10.0, 4.0};
    static constexpr double uncapped = 1e9;
    static constexpr std::array<double, signals> caps{uncapped, uncapped, uncapped, uncapped, 15.0, uncapped, uncapped, 8.0};
    static constexpr double half_life_us = 120000000.0;
    static constexpr double restrict_at = 10.0, kick_at = 25.0;
    static constexpr std::uint64_t lockout_window_us = 3600000000ULL;
    static constexpr std::array<std::uint64_t, 3> lockout_us{0, 15ULL * 60000000, 60ULL * 60000000};

    enum class Level : std::uint8_t { clean, restricted, kick };

    Level add(std::uint64_t now, Signal signal, double amount = 1.0) {
        decay(now);
        const auto index = static_cast<std::size_t>(signal);
        const double added = std::clamp(weights[index] * amount, 0.0, std::max(0.0, caps[index] - parts_[index]));
        parts_[index] += added;
        score_ += added;
        return level();
    }
    double score(std::uint64_t now) {
        decay(now);
        return score_;
    }
    double part(std::uint64_t now, Signal signal) {
        decay(now);
        return parts_[static_cast<std::size_t>(signal)];
    }
    Level level() const { return score_ >= kick_at ? Level::kick : score_ >= restrict_at ? Level::restricted : Level::clean; }
    Level level(std::uint64_t now) {
        decay(now);
        return level();
    }
    Signal strongest(std::uint64_t now) {
        decay(now);
        std::size_t best{};
        for (std::size_t i = 1; i < signals; ++i)
            if (parts_[i] > parts_[best]) best = i;
        return static_cast<Signal>(best);
    }
    std::uint64_t kicked(std::uint64_t now) {
        while (!kicks_.empty() && now - kicks_.front() > lockout_window_us) kicks_.pop_front();
        kicks_.push_back(now);
        score_ = kick_at * 0.8;
        for (auto &part : parts_) part = 0;
        const auto lockout = lockout_us[std::min(kicks_.size() - 1, lockout_us.size() - 1)];
        locked_until_ = lockout ? now + lockout : 0;
        return lockout;
    }
    bool locked(std::uint64_t now) const { return locked_until_ && now < locked_until_; }
    std::uint64_t locked_until() const { return locked_until_; }
    unsigned kicks() const { return static_cast<unsigned>(kicks_.size()); }
    bool forgettable(std::uint64_t now) {
        decay(now);
        return score_ < 0.5 && !locked(now) && (kicks_.empty() || now - kicks_.back() > lockout_window_us);
    }
    bool restricted_told{};

  private:
    double score_{};
    std::array<double, signals> parts_{};
    std::uint64_t decayed_at_{}, locked_until_{};
    std::deque<std::uint64_t> kicks_;

    void decay(std::uint64_t now) {
        if (decayed_at_ && now > decayed_at_) {
            const double factor = std::exp2(-static_cast<double>(now - decayed_at_) / half_life_us);
            score_ *= factor;
            for (auto &part : parts_) part *= factor;
        }
        decayed_at_ = now;
    }
};
}
