#pragma once
#include "Extension/Style/style_clips.h"
#include "Extension/Style/style_pose.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/UI/Overlay/overlay.h"
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dingosdk::style {
// Reads on a detached thread. Any thread. Its owner must live as long as the program.
template <class T> class ReadOnce {
public:
    // Only the first call reads. `what` must be a literal: the thread keeps it.
    template <class Read> void start(std::string_view what, Read read) noexcept {
        if (started_.exchange(true, std::memory_order_acq_rel)) return;
        try {
            std::thread([this, what, read = std::move(read)]() noexcept { finish(what, read); }).detach();
        } catch (...) {
            done_.store(true, std::memory_order_release);
        }
    }
    [[nodiscard]] std::shared_ptr<const T> value() const noexcept { return value_.load(std::memory_order_acquire); }
    [[nodiscard]] bool done() const noexcept { return done_.load(std::memory_order_acquire); }
    [[nodiscard]] bool failed() const noexcept { return done() && !value(); }

private:
    template <class Read> void finish(std::string_view what, const Read &read) noexcept {
        try {
            value_.store(std::make_shared<const T>(read()), std::memory_order_release);
        } catch (const std::exception &failure) {
            logging::log(logging::Level::warning, logging::Channel::skater, "Style: could not read {}: {}.", what, failure.what());
        } catch (...) {
            logging::log(logging::Level::warning, logging::Channel::skater, "Style: could not read {}.", what);
        }
        done_.store(true, std::memory_order_release);
    }
    std::atomic<bool> started_{}, done_{};
    std::atomic<std::shared_ptr<const T>> value_;
};
} // namespace dingosdk::style

// The editor's shared parts: its notices, its guarded tick, the pace of its clip and the joints it reads.
namespace dingosdk::style {
inline constexpr std::string_view notice_title = "Style editor";
inline void notify_player(overlay::NoticeLevel level, std::string_view reason) noexcept {
    const auto logged = level == overlay::NoticeLevel::info      ? logging::Level::info
                        : level == overlay::NoticeLevel::warning ? logging::Level::warning
                                                                 : logging::Level::error;
    logging::log(logged, logging::Channel::skater, "Style editor: {}.", reason);
    try {
        overlay::notify(level, std::string(notice_title), sentence(reason));
    } catch (...) {}
}
// On a throw, `stop` ends the machine and returns true if it was running; only then is the error logged.
template <class Tick, class Stop>
void guarded_tick(logging::Channel channel, std::string_view what, Tick &&tick, Stop &&stop) noexcept {
    try {
        tick();
    } catch (...) {
        bool stopped{};
        try {
            stopped = stop();
        } catch (...) {}
        if (stopped) logging::log(logging::Level::warning, channel, "{} stopped after an error.", what);
    }
}
} // namespace dingosdk::style
namespace dingosdk::style_layer {
// The pace of the clip on the stand-in. A new keyframe starts as the pose shown at that pace.
void note_editor_pace(const style::Pace &pace);
struct Rig {
    style::RigJoints joints; // its parents are `parents`
    std::shared_ptr<const std::vector<std::int32_t>> parents;
};
} // namespace dingosdk::style_layer
