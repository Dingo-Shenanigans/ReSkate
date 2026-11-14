#pragma once
#include <array>
#include <cmath>
#include <cstdint>

// A transient objects.pose request is not a body observation or an authored layout edit. Keep it
// until the server copy, then the client body, are actually read back at that pose, or for a bounded
// time. The native callers still check ownership, world/body lifetimes and editing before using it.
namespace dingosdk::placement_pose {
using Pose = std::array<float, 12>;
enum class Side { client, server };
// A new claim gives a server copy one second to resolve before accepting a bodiless copy. The
// request has that grace plus half a second for both native ticks; retries never extend the deadline.
inline constexpr std::uint64_t lifetime_ms = 1500, retry_ms = 16, ack_fresh_ms = 100;

inline bool matches(const Pose& a, const Pose& b) noexcept {
    double distance{}, dot{}, norm_a{}, norm_b{};
    for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
            return false;
    if (a[0] <= .001f || b[0] <= .001f)
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (std::abs(a[i] - b[i]) > .0001f)
            return false;
        const double delta = static_cast<double>(a[8 + i]) - b[8 + i];
        distance += delta * delta;
    }
    // Two millimetres allows native float/transform round trips, without accepting a displaced prop.
    if (distance > .002 * .002)
        return false;
    for (unsigned i = 4; i < 8; ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        norm_a += static_cast<double>(a[i]) * a[i];
        norm_b += static_cast<double>(b[i]) * b[i];
    }
    if (norm_a <= .95 || norm_a >= 1.05 || norm_b <= .95 || norm_b >= 1.05)
        return false;
    // q and -q describe the same rotation. Normalize the dot product because the native getter may
    // normalize an accepted near-unit quaternion; cos(0.05 degrees / 2) bounds the angular error.
    return std::abs(dot) / std::sqrt(norm_a * norm_b) >= .9999999048070578;
}

class Pending {
public:
    Pending(const Pose& target, std::uint64_t now) noexcept : target_(target), accepted_at_(now) {}
    const Pose& target() const noexcept { return target_; }
    bool expired(std::uint64_t now) const noexcept { return now < accepted_at_ || now - accepted_at_ >= lifetime_ms; }
    bool has_written(Side side) const noexcept { return progress(side).issued; }
    bool retry_due(Side side, std::uint64_t now) const noexcept {
        const auto& copy = progress(side);
        return !expired(now) && (!copy.attempted || (now >= copy.at && now - copy.at >= retry_ms));
    }
    // "issued" means the native setter was called, not that the body or transform accepted it.
    void attempted(Side side, std::uint64_t now, bool issued) noexcept {
        auto& copy = progress(side);
        copy.attempted = true;
        copy.at = now;
        if (issued) {
            copy.issued = true;
            copy.issued_at = now;
        }
        if (side == Side::server)
            server_observed_ = false;
    }
    // Called before any write in a later server tick, with a real server-copy readback. A matching
    // client sample before that publication/readback cannot acknowledge the request.
    void observe_server(const Pose& actual, std::uint64_t now) noexcept {
        const bool matched = !expired(now) && server_.issued && now > server_.issued_at && matches(actual, target_);
        if (matched) {
            if (!server_observed_)
                server_observed_at_ = now;
            server_fresh_at_ = now;
        }
        server_observed_ = matched;
    }
    void server_unavailable() noexcept { server_observed_ = false; }
    bool client_confirmed(const Pose& actual, std::uint64_t now) const noexcept {
        return !expired(now) && client_.issued && server_observed_ && now > server_observed_at_ &&
               now >= server_fresh_at_ && now - server_fresh_at_ <= ack_fresh_ms && matches(actual, target_);
    }

private:
    struct Progress {
        bool attempted{}, issued{};
        std::uint64_t at{}, issued_at{};
    };
    const Progress& progress(Side side) const noexcept { return side == Side::client ? client_ : server_; }
    Progress& progress(Side side) noexcept { return side == Side::client ? client_ : server_; }
    Pose target_;
    std::uint64_t accepted_at_{};
    Progress client_, server_;
    bool server_observed_{};
    std::uint64_t server_observed_at_{}, server_fresh_at_{};
};
} // namespace dingosdk::placement_pose
