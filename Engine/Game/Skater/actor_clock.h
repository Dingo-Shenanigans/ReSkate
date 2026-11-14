#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

// Caller serialization protects this SDK observation clock. It counts qualified
// publications and sums their native dt; it is not a global engine-step clock.
namespace dingosdk::actor_clock {
struct Sample {
    std::uint64_t serial{}, observed_ms{};
    double seconds{};
};
template<class Identity> struct Key {
    std::uint64_t owner{}, sequence{}, generation{};
    Identity actor{};
    bool operator==(const Key&) const = default;
};
template<class Identity> struct Feed {
    Key<Identity> key{};
    std::uint64_t serial{};
};
template<class Identity> class Ledger {
public:
    static constexpr std::uint64_t freshness_ms = 100;
    static constexpr double maximum_step_seconds = static_cast<double>(.1f);
    // Seeds permit exhaustion/precision regressions without running for years.
    explicit Ledger(std::uint64_t serial = 0, double seconds = 0) noexcept : issued_(serial), seed_seconds_(seconds) {}
    bool enable(const Key<Identity>& key) noexcept {
        if (!key.owner || !key.sequence || !key.generation || exhausted_) return false;
        if (key == key_) return enabled_;
        if (key.generation <= key_.generation) return false;
        key_ = key; sample_ = {}; sample_.seconds = seed_seconds_; enabled_ = true;
        return true;
    }
    bool disable(const Key<Identity>& key) noexcept {
        if (key != key_ || !key.owner) return false;
        enabled_ = false; return true;
    }
    bool begin(const Key<Identity>& key, Feed<Identity>& feed) noexcept {
        feed = {};
        if (!enabled_ || key != key_ || exhausted_) return false;
        if (issued_ == maximum) { exhausted_ = true; return false; }
        feed = {key, ++issued_}; return true;
    }
    bool publish(const Feed<Identity>& feed, double dt, std::uint64_t observed_ms) noexcept {
        if (!enabled_ || exhausted_ || feed.key != key_ || !feed.serial || feed.serial > issued_ ||
            feed.serial <= sample_.serial || !observed_ms || observed_ms < sample_.observed_ms ||
            !std::isfinite(dt) || dt <= 0 || dt > maximum_step_seconds) return false;
        const auto next = sample_.seconds + dt;
        if (!std::isfinite(next) || next <= sample_.seconds) { exhausted_ = true; return false; }
        sample_ = {feed.serial, observed_ms, next}; return true;
    }
    bool read(const Key<Identity>& key, std::uint64_t now_ms, Sample& out) const noexcept {
        out = {};
        if (!enabled_ || exhausted_ || key != key_ || !sample_.serial || !sample_.observed_ms ||
            now_ms < sample_.observed_ms || now_ms - sample_.observed_ms > freshness_ms ||
            !std::isfinite(sample_.seconds) || sample_.seconds < 0) return false;
        out = sample_; return true;
    }
private:
    static constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Key<Identity> key_{};
    Sample sample_{};
    std::uint64_t issued_{};
    double seed_seconds_{};
    bool enabled_{}, exhausted_{};
};
} // namespace dingosdk::actor_clock
