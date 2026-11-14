#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace dingosdk::teleport_observation {
enum class Observation : std::uint8_t {
    not_attempted, known, lock_refused, inactive, manager_read_failed,
    manager_null, phase_read_failed, exception, queue_changed, queue_exhausted
};
struct Identity {
    bool active{};
    std::uintptr_t base{};
    bool operator==(const Identity&) const = default;
};
struct Result {
    bool known{}, pending{true};
    Observation observation{Observation::not_attempted};
    std::uint64_t revision{};
    std::uintptr_t base{}, manager{};
};
constexpr bool same_idle(const Result& before, const Result& after) noexcept {
    return before.known && after.known && !before.pending && !after.pending &&
        before.revision && before.revision == after.revision && before.base && before.base == after.base &&
        before.manager && before.manager == after.manager;
}

// Own the queue being observed. Producers still take the existing profile
// mutex first. Observers take only this mutex, and never wait for it.
class Queue {
public:
    struct State {
        std::optional<std::array<float, 16>> position_teleport;
        std::optional<bool> position_teleport_on_board;
        std::uint64_t position_teleport_at{}, teleport_token{}, teleport_queued_at{};
        void set_position(const std::array<float, 16>& transform, std::optional<bool> on_board,
            std::uint64_t at) noexcept {
            position_teleport = transform;
            position_teleport_on_board = on_board;
            position_teleport_at = at;
        }
        void clear_position() noexcept {
            position_teleport.reset();
            position_teleport_on_board.reset();
        }
    };
    class Mutation {
        Queue& queue_;
        std::unique_lock<std::recursive_mutex> lock_;
        bool changed_{}, dispatch_{};
        friend class Queue;
        explicit Mutation(Queue& queue) : queue_(queue), lock_(queue.mutex_) {}
    public:
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
        ~Mutation() noexcept {
            if (dispatch_) --queue_.dispatch_depth_;
            if (changed_) queue_.advance();
        }
        const State& read() const noexcept { return queue_.state_; }
        bool dispatching() const noexcept { return queue_.dispatch_depth_ != 0; }
        State& write() noexcept {
            if (!changed_) { queue_.advance(); changed_ = true; }
            return queue_.state_;
        }
        // Every publication/invalidation of the enclosing profile must stamp
        // this same revision, including a same-value lifecycle replacement.
        void lifecycle_changed() noexcept { (void)write(); }
        // Declare before clearing a queue and before constructing the native
        // transition. Keep this scope alive through native transition teardown.
        void begin_dispatch() noexcept {
            if (dispatch_) return;
            (void)write();
            if (queue_.dispatch_depth_ == std::numeric_limits<std::uint32_t>::max()) {
                queue_.valid_ = false;
                return;
            }
            ++queue_.dispatch_depth_;
            dispatch_ = true;
        }
    };

    explicit Queue(std::uint64_t initial_revision = 1) noexcept
        : revision_(initial_revision), valid_(initial_revision != 0) {}
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Mutation mutate() { return Mutation(*this); }

    // Reader supplies active/base identity and safe native manager/phase reads.
    // It must invoke no game function, profile API, logger or caller callback.
    // The getter/setter pair preserves this helper's Win32 LastError effects.
    // Existing position-dispatch observations keep their position-only contract.
    // An actor admission can also include the real accepted placed-object queue.
    template<class Reader> Result observe(Reader& reader, bool include_placement_token = false) noexcept {
        const auto saved_error = reader.get_error();
        struct Restore {
            Reader& reader;
            decltype(saved_error) value;
            ~Restore() noexcept { reader.set_error(value); }
        } restore{reader, saved_error};
        Result result;
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) { result.observation = Observation::lock_refused; return result; }
            if (!valid_) { result.observation = Observation::queue_exhausted; return result; }
            const auto before = reader.identity();
            if (!before.active || !before.base) { result.observation = Observation::inactive; return result; }
            const auto revision = revision_;
            bool pending = dispatch_depth_ != 0 || state_.position_teleport.has_value() ||
                (include_placement_token && state_.teleport_token != 0);
            std::uintptr_t observed_manager{};
            if (!pending) {
                std::uintptr_t manager{};
                std::uint32_t phase{};
                if (!reader.read_manager(before.base, manager)) {
                    result.observation = Observation::manager_read_failed; return result;
                }
                if (!manager) { result.observation = Observation::manager_null; return result; }
                if (!reader.read_phase(manager, phase)) {
                    result.observation = Observation::phase_read_failed; return result;
                }
                pending = phase != 0;
                std::uintptr_t closing_manager{};
                std::uint32_t closing_phase{};
                if (!reader.read_manager(before.base, closing_manager)) {
                    result.observation = Observation::manager_read_failed; return result;
                }
                if (!closing_manager) { result.observation = Observation::manager_null; return result; }
                if (closing_manager != manager) { result.observation = Observation::queue_changed; return result; }
                if (!reader.read_phase(closing_manager, closing_phase)) {
                    result.observation = Observation::phase_read_failed; return result;
                }
                pending = pending || closing_phase != 0;
                observed_manager = manager;
            }
            const auto after = reader.identity();
            if (!after.active) { result.observation = Observation::inactive; return result; }
            if (!valid_) { result.observation = Observation::queue_exhausted; return result; }
            if (before != after || revision != revision_) {
                result.observation = Observation::queue_changed; return result;
            }
            result.known = true;
            result.pending = pending;
            result.observation = Observation::known;
            result.revision = revision;
            result.base = before.base;
            result.manager = observed_manager;
            return result;
        } catch (...) { result.observation = Observation::exception; return result; }
    }
private:
    void advance() noexcept {
        if (!valid_) return;
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) valid_ = false;
        else ++revision_;
    }
    std::recursive_mutex mutex_;
    State state_;
    std::uint64_t revision_;
    std::uint32_t dispatch_depth_{};
    bool valid_;
};
} // namespace dingosdk::teleport_observation
