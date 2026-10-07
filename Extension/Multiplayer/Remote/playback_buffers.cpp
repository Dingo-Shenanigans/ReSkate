#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace dingosdk::multiplayer {
namespace {
float distance_squared(const Transform &a, const Transform &b) {
    float d{};
    for (unsigned i = 0; i < 3; ++i)
        d += (a.position[i] - b.position[i]) * (a.position[i] - b.position[i]);
    return d;
}
bool valid_pose_anchors(const Pose &pose) {
    return valid_transform(pose.root) &&
           (pose.skater.size() <= 1 || valid_transform(pose.skater[1])) &&
           (pose.board.empty() || valid_transform(pose.board[0])) &&
           (pose.board.size() <= 2 || valid_transform(pose.board[2]));
}
// Prediction and correction move only the root, skater joint 1, the board entity and board
// joint 2 (offset_pose). Arrival continuity reads just those, not the ~460 bone transforms.
std::size_t skater_anchor_count(const Pose &pose) { return std::min<std::size_t>(pose.skater.size(), 2); }
std::size_t board_anchor_count(const Pose &pose) { return std::min<std::size_t>(pose.board.size(), 3); }
void copy_pose(Pose &out, const Pose &in, bool anchors_only) {
    if (!anchors_only) {
        out = in;
        return;
    }
    out.root = in.root;
    out.skater.assign(in.skater.begin(), in.skater.begin() + static_cast<std::ptrdiff_t>(skater_anchor_count(in)));
    out.board.assign(in.board.begin(), in.board.begin() + static_cast<std::ptrdiff_t>(board_anchor_count(in)));
}
} // namespace
bool AppearanceBuffer::push(const Packet &p) {
    if (p.kind != PacketKind::cosmetics || !p.epoch || !valid_appearance(p.appearance) ||
        (value_ && (p.epoch != epoch_ || !newer_sequence(p.sequence, sequence_))))
        return false;
    value_ = p.appearance;
    epoch_ = p.epoch;
    sequence_ = p.sequence;
    return true;
}
void AudioBuffer::clear() {
    frames_.clear();
    current_.reset();
    epoch_ = last_arrival_ = played_source_ = seen_ = 0;
    clock_offset_ = 0;
    sequence_ = 0;
}
bool AudioBuffer::push(const Packet &p, std::uint64_t arrival) {
    if (p.kind != PacketKind::audio || !p.epoch || !valid_audio_batch(p.audio) ||
        arrival > INT64_MAX || p.time_us > INT64_MAX ||
        (epoch_ && (epoch_ != p.epoch || arrival < last_arrival_))) return false;
    // Reliable action edges and disposable continuous states may arrive in a
    // different order. Deduplicate both host and direct copies without losing
    // a late edge merely because a newer continuous packet got there first.
    if (!seen_) { sequence_ = p.sequence; seen_ = 1; }
    else if (newer_sequence(p.sequence, sequence_)) {
        const auto shift = p.sequence - sequence_;
        seen_ = shift >= 64 ? 1 : (seen_ << shift) | 1;
        sequence_ = p.sequence;
    } else {
        const auto behind = sequence_ - p.sequence;
        if (behind >= 64 || (seen_ & (1ULL << behind))) return false;
        seen_ |= 1ULL << behind;
    }
    const auto source = p.time_us ? p.time_us : arrival;
    const auto offset = static_cast<std::int64_t>(arrival) - static_cast<std::int64_t>(source);
    if (!epoch_) clock_offset_ = offset;
    else {
        const auto drift = static_cast<std::int64_t>(std::min<std::uint64_t>(arrival - last_arrival_, 1000000) / 1000);
        clock_offset_ = std::min(clock_offset_ + std::min(drift, INT64_MAX - std::max<std::int64_t>(clock_offset_, 0)), offset);
    }
    for (const auto &sample : p.audio) {
        const auto time = source >= sample.age_us ? source - sample.age_us : 0;
        const auto at = std::upper_bound(frames_.begin(), frames_.end(), time,
            [](auto value, const auto &frame) { return value < frame.source; });
        frames_.insert(at, {sample.state, time, sample.event});
    }
    while (frames_.size() > 256) frames_.pop_front();
    epoch_ = p.epoch;
    last_arrival_ = arrival;
    return true;
}
std::optional<AudioState> AudioBuffer::sample(std::uint64_t now) {
    if (!epoch_ || now < last_arrival_ || now - last_arrival_ > 1000000) {
        current_.reset();
        return {};
    }
    const auto target = static_cast<std::uint64_t>(std::clamp(
        static_cast<long double>(now) - clock_offset_ - 100000, 0.0L, static_cast<long double>(INT64_MAX)));
    while (!frames_.empty() &&
           ((target > frames_.front().source && target - frames_.front().source > 250000) ||
            (!frames_.front().event && frames_.front().source < played_source_))) frames_.pop_front();
    while (frames_.size() > 1 && frames_[1].source <= target && !frames_[0].event && !frames_[1].event &&
           !audio_event_changed(frames_[0].state, frames_[1].state))
        frames_.pop_front();
    if (!frames_.empty() && frames_.front().source <= target) {
        auto frame = std::move(frames_.front());
        frames_.pop_front();
        // Play a still-timely retransmitted edge once, then restore the latest
        // state on the next render frame instead of rewinding continuous audio.
        if (frame.source < played_source_) return frame.state;
        current_ = frame.state;
        played_source_ = frame.source;
    }
    return current_;
}
void PoseBuffer::clear() {
    playback_at_ = correction_start_ = correction_end_ = 0;
    correction_ = {};
    playback_ = {};
    sender_clock_ = false;
    clock_offset_ = 0;
    frames_.clear();
    epoch_ = 0;
    sequence_ = 0;
}
bool PoseBuffer::push(const Packet &p, std::uint64_t arrival) {
    if (p.kind != PacketKind::pose || !valid_pose(p.pose) || !p.epoch || arrival > INT64_MAX ||
        p.time_us > INT64_MAX || !valid_pose_interval(p.pose_interval_us))
        return false;
    return push_validated(p, arrival);
}
// P is const Packet & (the pose is copied) or Packet (the pose is moved into the frame).
template <class P> bool PoseBuffer::push_frame(P &&p, std::uint64_t arrival) {
    if (p.kind != PacketKind::pose || !p.epoch || arrival > INT64_MAX || p.time_us > INT64_MAX ||
        !valid_pose_interval(p.pose_interval_us) ||
        !valid_transform(p.pose.root) || p.pose.skater.size() > max_skater_bones ||
        p.pose.board.size() > max_board_bones)
        return false;
    // Session code validates epochs before calling this buffer.
    if (!frames_.empty() && p.epoch == epoch_ && !newer_sequence(p.sequence, sequence_))
        return false;
    if (!frames_.empty() && (arrival < frames_.back().arrival ||
                             (p.epoch == epoch_ && sender_clock_ && p.time_us <= frames_.back().source)))
        return false;
    const bool reset = p.epoch != epoch_ ||
        (!frames_.empty() && (distance_squared(frames_.back().pose.root, p.pose.root) > 400 ||
                              frames_.back().pose.skater.size() != p.pose.skater.size() ||
                              frames_.back().pose.board.size() != p.pose.board.size()));
    std::optional<std::array<float, 3>> previous;
    if (!reset && playback_at_ && arrival >= playback_at_ && arrival - playback_at_ <= 200000 &&
        (playback_.mode == PosePlaybackMode::predicted || playback_.mode == PosePlaybackMode::held ||
         correction_end_ > arrival)) {
        PosePlayback ignored;
        if (predict(arrival, ignored, anchors_, true)) {
            correct(anchors_, arrival);
            previous = anchors_.root.position;
        }
    }
    if (reset)
        clear();
    epoch_ = p.epoch;
    sequence_ = p.sequence;
    if (frames_.empty()) {
        sender_clock_ = p.time_us != 0;
        clock_offset_ = static_cast<std::int64_t>(arrival) - static_cast<std::int64_t>(p.time_us);
    } else if (sender_clock_) {
        const auto observed = static_cast<std::int64_t>(arrival) - static_cast<std::int64_t>(p.time_us);
        // Follow the fastest observed delivery, allowing slow upward clock drift.
        // Late/bursty arrivals must not stretch the recorded animation timeline.
        const auto drift = static_cast<std::int64_t>(
            std::min<std::uint64_t>(arrival - frames_.back().arrival, 1000000) / 1000);
        clock_offset_ = std::min(
            observed, clock_offset_ + std::min(drift, INT64_MAX - std::max<std::int64_t>(clock_offset_, 0)));
    }
    const auto source = sender_clock_ ? p.time_us : arrival;
    frames_.push_back({std::forward<P>(p).pose, arrival, source});
    interpolation_delay_us_ = std::max(100000U, p.pose_interval_us + 50000);
    while (frames_.size() > 64)
        frames_.pop_front();
    // Playback samples at most the interpolation delay (<= 250 ms) plus one frame gap (<= 500 ms)
    // behind the newest frame, and prediction reads the last three. Keep about a second, keeping
    // the frame just before that window as the interpolation start.
    while (frames_.size() > 3 && frames_.back().source - frames_[1].source >= 1000000)
        frames_.pop_front();
    if (previous) {
        PosePlayback ignored;
        const bool predicted = predict(arrival, ignored, anchors_, true);
        std::array<float, 3> correction{};
        float distance{};
        if (predicted) for (unsigned i = 0; i < 3; ++i) {
            correction[i] = (*previous)[i] - anchors_.root.position[i];
            distance += correction[i] * correction[i];
        }
        // Reconcile small display errors only. Keep an existing deadline so
        // successive packets cannot perpetually extend the correction delay.
        if (predicted && distance > .000001f && distance <= 4.f) {
            correction_ = correction;
            correction_start_ = arrival;
            if (correction_end_ <= arrival) correction_end_ = arrival + 100000;
        } else {
            correction_ = {};
            correction_start_ = correction_end_ = 0;
        }
    }
    return true;
}
bool PoseBuffer::push_validated(const Packet &p, std::uint64_t arrival) { return push_frame(p, arrival); }
bool PoseBuffer::push_validated(Packet &&p, std::uint64_t arrival) { return push_frame(std::move(p), arrival); }
std::uint64_t PoseBuffer::target_time(std::uint64_t now) const {
    return
        sender_clock_
            ? static_cast<std::uint64_t>(std::clamp(static_cast<long double>(now) - clock_offset_ - interpolation_delay_us_,
                                                    0.0L, static_cast<long double>(INT64_MAX)))
            : (now > interpolation_delay_us_ ? now - interpolation_delay_us_ : 0);
}
bool PoseBuffer::sample(std::uint64_t now, Pose &out) const { return sample_frames(now, out, false); }
bool PoseBuffer::sample_frames(std::uint64_t now, Pose &out, bool anchors_only) const {
    if (frames_.empty() || now < frames_.back().arrival || now - frames_.back().arrival > 1000000)
        return false;
    const auto target = target_time(now);
    if (target <= frames_.front().source) {
        copy_pose(out, frames_.front().pose, anchors_only);
        return true;
    }
    // Sources only increase, and the target trails the newest frame by about the
    // interpolation delay: search from the newest end for the first frame after it.
    auto next = frames_.size();
    while (next > 1 && frames_[next - 1].source > target)
        --next;
    if (next == frames_.size()) {
        copy_pose(out, frames_.back().pose, anchors_only);
        return true;
    }
    const auto &a = frames_[next - 1];
    const auto &b = frames_[next];
    if (b.source - a.source > 500000) {
        copy_pose(out, b.pose, anchors_only);
        return true;
    }
    const float t = static_cast<float>(target - a.source) / static_cast<float>(b.source - a.source);
    out.root = interpolate(a.pose.root, b.pose.root, t);
    out.skater.resize(anchors_only ? skater_anchor_count(a.pose) : a.pose.skater.size());
    out.board.resize(anchors_only ? board_anchor_count(a.pose) : a.pose.board.size());
    for (std::size_t j = 0; j < out.skater.size(); ++j)
        out.skater[j] = interpolate(a.pose.skater[j], b.pose.skater[j], t);
    for (std::size_t j = 0; j < out.board.size(); ++j)
        out.board[j] = interpolate(a.pose.board[j], b.pose.board[j], t);
    return true;
}
std::optional<Pose> PoseBuffer::sample(std::uint64_t now) const {
    Pose out;
    return sample(now, out) ? std::optional<Pose>(std::move(out)) : std::nullopt;
}
bool PoseBuffer::predict(std::uint64_t now, PosePlayback &state, Pose &pose, bool anchors_only) const {
    const bool sampled = sample_frames(now, pose, anchors_only);
    state = {};
    if (!sampled) return false;
    state.mode = PosePlaybackMode::buffered;
    const auto target = target_time(now);
    const auto &last = frames_.back();
    if (target <= last.source) return true;
    state.mode = PosePlaybackMode::held;
    if (!sender_clock_ || frames_.size() < 2) return true;
    const auto &before = frames_[frames_.size() - 2];
    const auto elapsed = last.source - before.source;
    if (elapsed < multiplayer_pose_interval(120) || elapsed > 250000) return true;
    const auto seconds = static_cast<float>(elapsed) / 1000000.f;
    std::array<float, 3> velocity{};
    float speed_squared{};
    for (unsigned i = 0; i < 3; ++i) {
        velocity[i] = (last.pose.root.position[i] - before.pose.root.position[i]) / seconds;
        speed_squared += velocity[i] * velocity[i];
    }
    // Teleports, implausible speeds and recent abrupt acceleration do not
    // provide a useful motion estimate. No input, collision or trick guessing.
    if (!std::isfinite(speed_squared) || speed_squared > 45.f * 45.f) return true;
    if (frames_.size() >= 3) {
        const auto &older = frames_[frames_.size() - 3];
        const auto previous_elapsed = before.source - older.source;
        if (previous_elapsed >= multiplayer_pose_interval(120) && previous_elapsed <= 250000) {
            const auto previous_seconds = static_cast<float>(previous_elapsed) / 1000000.f;
            float acceleration_squared{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto old_velocity = (before.pose.root.position[i] - older.pose.root.position[i]) / previous_seconds;
                const auto acceleration = (velocity[i] - old_velocity) / ((seconds + previous_seconds) * .5f);
                acceleration_squared += acceleration * acceleration;
            }
            if (acceleration_squared > 60.f * 60.f) return true;
        }
    }
    const auto ahead = target - last.source;
    state.prediction_us = std::min<std::uint64_t>(ahead, 100000);
    std::array<float, 3> displacement{};
    const float duration = std::min(static_cast<float>(state.prediction_us) / 1000000.f,
        speed_squared > .0001f ? 2.f / std::sqrt(speed_squared) : .1f);
    for (unsigned i = 0; i < 3; ++i) displacement[i] = velocity[i] * duration;
    offset_pose(pose, displacement); // Translate both world anchors; child bones and rotations stay recorded.
    if (!valid_pose_anchors(pose)) {
        state.prediction_us = 0;
        copy_pose(pose, last.pose, anchors_only);
        return true;
    }
    state.mode = ahead < 100000 && duration * std::sqrt(speed_squared) < 2.f
        ? PosePlaybackMode::predicted : PosePlaybackMode::held;
    return true;
}
void PoseBuffer::correct(Pose &pose, std::uint64_t now) const {
    if (now < correction_start_ || now >= correction_end_ || correction_end_ <= correction_start_) return;
    const auto weight = static_cast<float>(correction_end_ - now) /
        static_cast<float>(correction_end_ - correction_start_);
    auto offset = correction_;
    for (auto &v : offset) v *= weight;
    for (const auto *anchor : {&pose.root, pose.skater.size() > 1 ? &pose.skater[1] : nullptr,
        !pose.board.empty() ? &pose.board[0] : nullptr, pose.board.size() > 2 ? &pose.board[2] : nullptr}) {
        if (!anchor) continue;
        auto moved = *anchor;
        for (unsigned i = 0; i < 3; ++i) moved.position[i] += offset[i];
        if (!valid_transform(moved)) return;
    }
    offset_pose(pose, offset);
}
bool PoseBuffer::heard_within(std::uint64_t now, std::uint64_t age) const {
    return !frames_.empty() && (now < frames_.back().arrival || now - frames_.back().arrival <= age);
}
bool PoseBuffer::sample_remote(std::uint64_t now, Pose &pose) {
    if (playback_at_ && (now < playback_at_ || now - playback_at_ > 200000)) {
        correction_start_ = correction_end_ = 0;
        correction_ = {};
    }
    const bool sampled = predict(now, playback_, pose);
    playback_at_ = now;
    if (sampled) {
        correct(pose, now);
        playback_.correcting = correction_start_ <= now && now < correction_end_;
    }
    return sampled;
}
std::optional<Pose> PoseBuffer::sample_remote(std::uint64_t now) {
    Pose out;
    return sample_remote(now, out) ? std::optional<Pose>(std::move(out)) : std::nullopt;
}
} // namespace dingosdk::multiplayer
