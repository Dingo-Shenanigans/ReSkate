#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace dingosdk::actor_control {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
    "Actor-control native callbacks require a lock-free 64-bit claim.");
enum class Domain : std::uint8_t { equipment, follow };
enum class Phase : std::uint8_t { idle, admitting, published, revoking };
enum class Barrier : std::uint8_t { none = 0, native_consumed = 1, body_parked = 2 };
constexpr Barrier operator|(Barrier a, Barrier b) noexcept {
    return static_cast<Barrier>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
struct Ticket {
    std::uint64_t generation{};
    bool operator==(const Ticket&) const = default;
    explicit operator bool() const noexcept { return generation != 0; }
};
struct Snapshot {
    bool available{};
    Ticket ticket{};
    std::uint64_t owner{};
    Domain domain{Domain::equipment};
    Phase phase{Phase::idle};
    Barrier required{Barrier::none}, pending{Barrier::none};
    unsigned in_flight{};
};

class Arbiter;
// A cooperating SDK writer can run only while no claim exists. Its marker is
// in the same atomic word as reservation, so an idle read cannot race admission.
// This is dispatch exclusion, not native consumption or restoration evidence.
class IdleCall {
public:
    IdleCall() noexcept = default;
    ~IdleCall() noexcept;
    IdleCall(const IdleCall&) = delete;
    IdleCall& operator=(const IdleCall&) = delete;
    IdleCall(IdleCall&&) = delete;
    IdleCall& operator=(IdleCall&&) = delete;
    bool active() const noexcept { return arbiter_ != nullptr; }
private:
    friend class Arbiter;
    Arbiter* arbiter_{};
};
// One original native invocation owns one cookie. It cannot be copied or moved
// into a second return path. The service must outlive its native invocations.
class Call {
public:
    Call() noexcept = default;
    ~Call() noexcept;
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call(Call&&) = delete;
    Call& operator=(Call&&) = delete;
    bool active() const noexcept { return arbiter_ != nullptr; }
private:
    friend class Arbiter;
    Arbiter* arbiter_{};
    Ticket ticket_{};
    std::uint64_t owner_{};
};

// This is an internal ownership gate, not an actuator or a native completion
// detector. FOLLOW always requires body park and actual publisher drain. A
// particular operation may explicitly require native consumption as well;
// synchronous copied values need not claim native pose-history erasure.
// A single atomic word binds phase, callbacks and barriers to one generation.
class Arbiter {
    static constexpr unsigned required_shift = 2, pending_shift = 4, count_shift = 6, generation_shift = 14;
    static constexpr std::uint64_t phase_mask = 3, barrier_mask = 3, count_unit = std::uint64_t{1} << count_shift;
    static constexpr std::uint64_t reserving = std::numeric_limits<std::uint64_t>::max();
    static constexpr unsigned attempts = 64;
public:
    static constexpr unsigned maximum_in_flight = 255;
    static constexpr std::uint64_t maximum_generation =
        (std::numeric_limits<std::uint64_t>::max() >> generation_shift) - 1;
    // The seed permits an exhaustion regression without wrapping a real token.
    explicit Arbiter(std::uint64_t first_generation = 1) noexcept : next_(first_generation) {}
    Arbiter(const Arbiter&) = delete;
    Arbiter& operator=(const Arbiter&) = delete;

    bool enter_idle(IdleCall& call) noexcept {
        if (call.active()) return false;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            auto word = word_.load(std::memory_order_acquire);
            // An idle writer word has no generation and no phase/barriers.
            if ((word & ~(static_cast<std::uint64_t>(maximum_in_flight) << count_shift)) ||
                count(word) == maximum_in_flight) return false;
            if (word_.compare_exchange_weak(word, word + count_unit, std::memory_order_acq_rel)) {
                call.arbiter_ = this; return true;
            }
        }
        return false;
    }
    bool leave_idle(IdleCall& call) noexcept {
        if (call.arbiter_ != this) return false;
        // Reservation requires exactly zero. This still-active marker prevents
        // any claim generation from occupying the word until this subtraction.
        // fetch_sub cannot strand a marker after bounded-CAS contention.
        (void)word_.fetch_sub(count_unit, std::memory_order_acq_rel);
        call.arbiter_ = nullptr; return true;
    }
    bool reserve(std::uint64_t owner, Domain domain, Ticket& ticket) noexcept {
        ticket = {};
        if (!owner || !valid(domain)) return false;
        std::uint64_t idle{};
        if (!word_.compare_exchange_strong(idle, reserving, std::memory_order_acq_rel)) return false;
        if (!next_ || next_ > maximum_generation) {
            word_.store(0, std::memory_order_release); return false;
        }
        const auto generation = next_;
        next_ = generation == maximum_generation ? 0 : generation + 1;
        owner_.store(owner, std::memory_order_relaxed);
        domain_.store(domain, std::memory_order_relaxed);
        ticket.generation = generation;
        word_.store(pack(ticket, Phase::admitting), std::memory_order_release);
        return true;
    }
    bool publish(std::uint64_t owner, Ticket ticket) noexcept {
        auto word = current(owner, ticket);
        if (!word || phase(word) != Phase::admitting) return false;
        return word_.compare_exchange_strong(word, pack(ticket, Phase::published), std::memory_order_acq_rel);
    }
    // Only the admission owner can abandon its unpublished generation. Ordinary
    // terminal observers use revoke/retire, which never clear an admission.
    bool abandon(std::uint64_t owner, Ticket ticket) noexcept {
        auto word = current(owner, ticket);
        if (!word || phase(word) != Phase::admitting) return false;
        return word_.compare_exchange_strong(word, 0, std::memory_order_acq_rel);
    }
    bool revoke(std::uint64_t owner, Ticket ticket, Barrier required = Barrier::none) noexcept {
        if (!valid(required)) return false;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            auto word = current(owner, ticket);
            if (!word) return false;
            if (domain_.load(std::memory_order_acquire) == Domain::follow &&
                required != Barrier::body_parked &&
                required != (Barrier::native_consumed | Barrier::body_parked)) return false;
            if (phase(word) == Phase::revoking) return barriers(word, required_shift) == required;
            if (phase(word) != Phase::published) return false;
            const auto bits = static_cast<std::uint64_t>(required);
            const auto desired = (word & ~phase_mask) | static_cast<unsigned>(Phase::revoking) |
                (bits << required_shift) | (bits << pending_shift);
            if (word_.compare_exchange_weak(word, desired, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    bool enter(std::uint64_t owner, Ticket ticket, Call& call) noexcept {
        if (call.active()) return false;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            auto word = current(owner, ticket);
            if (!word || phase(word) != Phase::published || count(word) == maximum_in_flight) return false;
            if (word_.compare_exchange_weak(word, word + count_unit, std::memory_order_acq_rel)) {
                call.arbiter_ = this; call.owner_ = owner; call.ticket_ = ticket; return true;
            }
        }
        return false;
    }
    bool leave(Call& call) noexcept {
        if (call.arbiter_ != this) return false;
        const auto word = current(call.owner_, call.ticket_);
        if (!word || !count(word) || (phase(word) != Phase::published && phase(word) != Phase::revoking)) return false;
        // This exact cookie still contributes one count, so retirement and a
        // replacement generation remain impossible until its subtraction.
        // Revocation and barrier changes preserve that count. A guaranteed RMW
        // cannot lose a destructor's only drain attempt to bounded-CAS contention.
        (void)word_.fetch_sub(count_unit, std::memory_order_acq_rel);
        call.arbiter_ = nullptr; call.owner_ = 0; call.ticket_ = {}; return true;
    }
    bool satisfy(std::uint64_t owner, Ticket ticket, Barrier proof) noexcept {
        if (!valid(proof) || proof == Barrier::none) return false;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            auto word = current(owner, ticket);
            if (!word || phase(word) != Phase::revoking) return false;
            const auto required = static_cast<unsigned>(barriers(word, required_shift));
            const auto supplied = static_cast<unsigned>(proof);
            if ((supplied & required) != supplied) return false;
            const auto desired = word & ~(static_cast<std::uint64_t>(supplied) << pending_shift);
            if (desired == word || word_.compare_exchange_weak(word, desired, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    bool retire(std::uint64_t owner, Ticket ticket) noexcept {
        auto word = current(owner, ticket);
        if (!word || phase(word) != Phase::revoking || count(word) ||
            barriers(word, pending_shift) != Barrier::none) return false;
        return word_.compare_exchange_strong(word, 0, std::memory_order_acq_rel);
    }
    bool publication_allowed() const noexcept { return word_.load(std::memory_order_acquire) == 0; }
    bool owns(std::uint64_t owner, Ticket ticket, Domain domain) const noexcept {
        const auto word = current(owner, ticket);
        return word && valid(domain) && domain_.load(std::memory_order_acquire) == domain &&
            word_.load(std::memory_order_acquire) == word;
    }
    Snapshot snapshot() const noexcept {
        Snapshot out;
        const auto word = word_.load(std::memory_order_acquire);
        if (word == reserving) return out;
        if (!word) { out.available = true; return out; }
        if (!(word & ~(static_cast<std::uint64_t>(maximum_in_flight) << count_shift))) {
            // Do not leak a previous claim's retained owner/domain as the
            // identity of a generation-zero idle dispatch marker.
            out.in_flight = count(word);
            out.available = word_.load(std::memory_order_acquire) == word;
            return out.available ? out : Snapshot{};
        }
        out.ticket.generation = word >> generation_shift;
        out.owner = owner_.load(std::memory_order_acquire);
        out.domain = domain_.load(std::memory_order_acquire);
        out.phase = phase(word); out.in_flight = count(word);
        out.required = barriers(word, required_shift); out.pending = barriers(word, pending_shift);
        if (word_.load(std::memory_order_acquire) != word) return {};
        out.available = true; return out;
    }

private:
    static constexpr bool valid(Domain domain) noexcept { return domain == Domain::equipment || domain == Domain::follow; }
    static constexpr bool valid(Barrier barrier) noexcept { return static_cast<unsigned>(barrier) <= barrier_mask; }
    static constexpr std::uint64_t pack(Ticket ticket, Phase value) noexcept {
        return (ticket.generation << generation_shift) | static_cast<unsigned>(value);
    }
    static constexpr Phase phase(std::uint64_t word) noexcept { return static_cast<Phase>(word & phase_mask); }
    static constexpr unsigned count(std::uint64_t word) noexcept { return static_cast<unsigned>((word >> count_shift) & maximum_in_flight); }
    static constexpr Barrier barriers(std::uint64_t word, unsigned shift) noexcept { return static_cast<Barrier>((word >> shift) & barrier_mask); }
    std::uint64_t current(std::uint64_t owner, Ticket ticket) const noexcept {
        if (!owner || !ticket.generation || ticket.generation > maximum_generation) return 0;
        const auto word = word_.load(std::memory_order_acquire);
        if (!word || word == reserving || (word >> generation_shift) != ticket.generation ||
            owner_.load(std::memory_order_acquire) != owner) return 0;
        return word;
    }
    std::atomic<std::uint64_t> word_{}, owner_{};
    std::atomic<Domain> domain_{Domain::equipment};
    // The reserving word serializes access, including failed exhaustion.
    std::uint64_t next_;
};
inline Call::~Call() noexcept { if (arbiter_) (void)arbiter_->leave(*this); }
inline IdleCall::~IdleCall() noexcept { if (arbiter_) (void)arbiter_->leave_idle(*this); }
} // namespace dingosdk::actor_control
