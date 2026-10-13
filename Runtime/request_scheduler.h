#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <type_traits>
#include <optional>
#include <string>
#include <variant>

namespace dingosdk::runtime {
struct LoadRequest { std::string asset, start, lm_level, lm_start; };
struct ConsoleRequest {
    std::string text;
    std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
};
struct RequestContext { bool tick_ready{}, healthy{}, loader_ready{}, offline_ready{}; };

// Runtime's mutex serializes this scheduler and the published UI model.
// The engine thread takes work without holding that mutex during native calls.
class RequestScheduler {
public:
    enum class LoadPhase { idle, queued, preparing, inflight };
    enum class Kind { console, debug, offline };
private:
    using Work = std::variant<ConsoleRequest, overlay::DebugRequest, overlay::OfflineFeatureRequest>;
    std::deque<Work> work_;
    std::optional<Kind> active_;
    std::uint32_t active_thread_{};
    std::size_t continuations_{};
    LoadPhase load_phase_{};
    std::optional<LoadRequest> load_;
    template<class T> std::size_t count() const {
        return static_cast<std::size_t>(std::count_if(work_.begin(), work_.end(),
            [](const Work& value) { return std::holds_alternative<T>(value); }));
    }
    template<class T> void clear() {
        const auto end = std::next(work_.begin(), static_cast<std::ptrdiff_t>(continuations_));
        continuations_ -= static_cast<std::size_t>(std::count_if(work_.begin(), end,
            [](const Work& value) { return std::holds_alternative<T>(value); }));
        std::erase_if(work_, [](const Work& value) { return std::holds_alternative<T>(value); });
    }
    template<class T> void append(T value, std::uint32_t thread) {
        // A console command's gameplay operation must precede work accepted
        // after that command. Other producers retain FIFO order.
        if (executing_console(thread)) {
            work_.emplace(std::next(work_.begin(), static_cast<std::ptrdiff_t>(continuations_)), std::move(value));
            ++continuations_;
        } else work_.emplace_back(std::move(value));
    }
public:
    bool loading() const noexcept { return load_phase_ != LoadPhase::idle; }
    bool pending_load() const noexcept { return load_phase_ == LoadPhase::queued; }
    bool inflight() const noexcept { return load_phase_ == LoadPhase::inflight; }
    bool idle() const noexcept { return !loading() && !active_ && work_.empty(); }
    bool executing_console(std::uint32_t thread) const noexcept {
        return active_ == Kind::console && active_thread_ == thread;
    }
    bool can_load(const RequestContext& context, std::uint32_t thread) const noexcept {
        return context.tick_ready && context.healthy && context.loader_ready && !loading() &&
            ((executing_console(thread) && continuations_ == 0) || (!active_ && work_.empty()));
    }
    bool enqueue(LoadRequest request, const RequestContext& context, std::uint32_t thread) {
        if (!can_load(context, thread)) return false;
        load_ = std::move(request); load_phase_ = LoadPhase::queued; return true;
    }
    bool enqueue(ConsoleRequest request, const RequestContext& context, std::uint32_t thread) {
        if (!context.tick_ready || !context.healthy || count<ConsoleRequest>() >= 32) return false;
        append(std::move(request), thread); return true;
    }
    bool enqueue(overlay::DebugRequest request, const RequestContext& context, std::uint32_t thread) {
        const bool restore = request.action == overlay::DebugAction::restore_debug;
        if (!context.tick_ready || (!context.healthy && !restore) || loading() ||
            request.action < overlay::DebugAction::set_free_camera ||
            request.action > overlay::DebugAction::set_style_editor || !std::isfinite(request.value) ||
            (!restore && count<overlay::DebugRequest>() >= 16)) return false;
        if (restore) clear<overlay::DebugRequest>();
        append(request, thread); return true;
    }
    bool enqueue(overlay::OfflineFeatureRequest request, const RequestContext& context, std::uint32_t thread) {
        const bool restore = request.group == overlay::OfflineFeatureGroup::restore_all;
        if (!context.tick_ready || !context.offline_ready || (!context.healthy && !restore) || loading() ||
            static_cast<std::size_t>(request.group) > static_cast<std::size_t>(overlay::OfflineFeatureGroup::restore_all) ||
            (!restore && count<overlay::OfflineFeatureRequest>() >= 16)) return false;
        if (restore) clear<overlay::OfflineFeatureRequest>();
        append(request, thread); return true;
    }
    template<class T> std::optional<T> take(std::uint32_t thread) {
        if (loading() || active_ || work_.empty() || !std::holds_alternative<T>(work_.front())) return {};
        T value = std::move(std::get<T>(work_.front()));
        work_.pop_front();
        if constexpr (std::is_same_v<T, ConsoleRequest>) active_ = Kind::console;
        else if constexpr (std::is_same_v<T, overlay::DebugRequest>) active_ = Kind::debug;
        else active_ = Kind::offline;
        active_thread_ = thread;
        return value;
    }
    void finish_work() noexcept { active_.reset(); active_thread_ = 0; continuations_ = 0; }
    std::optional<LoadRequest> take_load() {
        if (!pending_load() || active_) return {};
        auto value = std::move(load_); load_.reset();
        load_phase_ = LoadPhase::preparing; // Keep the reservation through validation/restoration.
        return value;
    }
    void submitted_load() noexcept { load_phase_ = LoadPhase::inflight; }
    void finish_load() noexcept { load_.reset(); load_phase_ = LoadPhase::idle; }
    void fault() noexcept { work_.clear(); finish_work(); finish_load(); }
};
}
