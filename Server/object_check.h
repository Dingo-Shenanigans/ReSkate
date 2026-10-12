#pragma once
#include "Engine/Game/World/network_objects.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace dingosdk::server {
class ObjectCheck {
  public:
    using Position = std::array<float, 3>;
    struct Rules {
        float max_scale = 5.f;
        float clearance = 2.f;
    };
    static constexpr double move_rate = 30.0;
    static constexpr double move_burst = 150.0;
    static constexpr std::uint64_t throttle_gap_us = 500000;

    struct Result {
        std::vector<NetworkObject> layout;
        unsigned on_players{}, newly_on_players{}, chasing{}, throttled{};
        std::uint64_t throttled_us{};
        bool retry{};
    };

    static bool touches(const NetworkObject &object, std::span<const Position> others, float clearance) {
        if (!(clearance > 0.f)) return false;
        const float reach = clearance * std::max(1.f, object.scale);
        for (const auto &other : others) {
            float squared{};
            for (unsigned i = 0; i < 3; ++i) squared += (object.position[i] - other[i]) * (object.position[i] - other[i]);
            if (squared <= reach * reach) return true;
        }
        return false;
    }

    Result filter(std::uint64_t now, std::vector<NetworkObject> layout, const std::map<std::uint64_t, NetworkObject> &shared,
                  std::span<const Position> others, const Rules &rules) {
        refill(now);
        Result result;
        result.layout.reserve(layout.size());
        std::map<std::uint64_t, NetworkObject> withheld;
        for (auto &object : layout) {
            if (rules.max_scale > 0.f && object.scale > rules.max_scale) object.scale = rules.max_scale;
            const auto found = shared.find(object.id);
            const bool known = found != shared.end();
            if (known && found->second == object) {
                result.layout.push_back(object);
                continue;
            }
            bool keep = true;
            if (touches(object, others, rules.clearance)) {
                keep = false;
                ++result.on_players;
                const auto before = withheld_.find(object.id);
                if (before == withheld_.end()) ++result.newly_on_players;
                else if (before->second != object) ++result.chasing;
                withheld.emplace(object.id, object);
            } else if (known) {
                if (tokens_ >= 1.0) tokens_ -= 1.0;
                else {
                    keep = false;
                    ++result.throttled;
                }
            }
            if (keep) result.layout.push_back(object);
            else {
                result.retry = true;
                if (known) result.layout.push_back(found->second);
            }
        }
        withheld_ = std::move(withheld);
        if (result.throttled) {
            if (last_throttled_ && now > last_throttled_ && now - last_throttled_ <= throttle_gap_us)
                result.throttled_us = now - last_throttled_;
            last_throttled_ = now;
        }
        return result;
    }

    void restart() { *this = {}; }

  private:
    double tokens_ = move_burst;
    std::uint64_t refilled_at_{}, last_throttled_{};
    std::map<std::uint64_t, NetworkObject> withheld_;

    void refill(std::uint64_t now) {
        if (refilled_at_ && now > refilled_at_)
            tokens_ = std::min(move_burst, tokens_ + static_cast<double>(now - refilled_at_) / 1000000.0 * move_rate);
        refilled_at_ = now;
    }
};
}
