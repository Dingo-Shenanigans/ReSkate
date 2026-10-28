#pragma once
#include <cstddef>
#include <cstdint>

// A selected observed XInput stream, not native actor or physical device ownership.
// No operation in this observation-only contract changes a game input report.
namespace dingosdk::input_lease {
struct Pad {
    std::uint16_t buttons{};
    std::uint8_t left_trigger{}, right_trigger{};
    std::int16_t left_x{}, left_y{}, right_x{}, right_y{};
    bool operator==(const Pad&) const = default;
};
inline constexpr std::int32_t left_stick_rest = 7849, right_stick_rest = 8689, trigger_rest = 30;
constexpr bool stick_at_rest(std::int16_t x, std::int16_t y, std::int32_t radius) noexcept {
    const std::int64_t dx = x, dy = y, r = radius;
    return dx * dx + dy * dy <= r * r;
}
constexpr bool neutral(const Pad& pad) noexcept {
    return !pad.buttons && pad.left_trigger <= trigger_rest && pad.right_trigger <= trigger_rest &&
        stick_at_rest(pad.left_x, pad.left_y, left_stick_rest) &&
        stick_at_rest(pad.right_x, pad.right_y, right_stick_rest);
}
inline constexpr std::size_t retained_capacity = 8;
inline constexpr std::uint64_t lifetime_ms = 250, maximum_age_ms = 100;
struct Handle {
    std::uint64_t sequence{};
    bool operator==(const Handle&) const = default;
};
enum class Status { warming, active, released, revoked, expired };
enum class Reason {
    none, unavailable, busy, invalid_owner, invalid_source, wrong_thread,
    client_not_ready, source_unavailable, source_changed, source_stale,
    context_changed, focus_lost, gameplay_inactive, menu_open, interference,
    deadline, stale_epoch, capacity, sequence_exhausted, cancelled
};
inline bool terminal(Status value) noexcept {
    return value == Status::released || value == Status::revoked || value == Status::expired;
}
inline const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::warming: return "warming";
    case Status::active: return "active";
    case Status::released: return "released";
    case Status::revoked: return "revoked";
    case Status::expired: return "expired";
    }
    return "invalid";
}
inline const char* reason_name(Reason value) noexcept {
    switch (value) {
    case Reason::none: return "none";
    case Reason::unavailable: return "unavailable";
    case Reason::busy: return "busy";
    case Reason::invalid_owner: return "invalid_owner";
    case Reason::invalid_source: return "invalid_source";
    case Reason::wrong_thread: return "wrong_thread";
    case Reason::client_not_ready: return "client_not_ready";
    case Reason::source_unavailable: return "source_unavailable";
    case Reason::source_changed: return "source_changed";
    case Reason::source_stale: return "source_stale";
    case Reason::context_changed: return "context_changed";
    case Reason::focus_lost: return "focus_lost";
    case Reason::gameplay_inactive: return "gameplay_inactive";
    case Reason::menu_open: return "menu_open";
    case Reason::interference: return "interference";
    case Reason::deadline: return "deadline";
    case Reason::stale_epoch: return "stale_epoch";
    case Reason::capacity: return "capacity";
    case Reason::sequence_exhausted: return "sequence_exhausted";
    case Reason::cancelled: return "cancelled";
    }
    return "invalid";
}
struct Snapshot {
    Handle handle{};
    std::uint64_t source_epoch{}, observation_serial{}, observed_ms{}, context_generation{};
    std::uint32_t user{}, packet{};
    Pad pad{};
    bool operator==(const Snapshot&) const = default;
};
struct Receipt {
    Handle handle{};
    Status status{Status::warming};
    Reason reason{Reason::none};
    std::uint32_t user{};
    std::uint64_t requested_ms{}, expires_ms{}, observed_ms{}, source_epoch{}, context_generation{};
    std::uint64_t last_observation{}, renewals{};
    bool source_released{}, orphaned{};
    // Only callbacks that began with this lease reserve its terminal record.
    // A zero count is not evidence that native input consumed a report.
    std::uint32_t polls_in_flight{};
};
// Trusted native facts. Context is the observed client/readiness generation;
// it is deliberately not described as actor, body, world, or rider authority.
struct Guard {
    std::uint64_t now_ms{}, context_generation{};
    bool ready{}, focused{}, gameplay{}, menu_closed{}, interference_clear{};
};
struct Source {
    std::uint32_t user{}, packet{};
    std::uint64_t epoch{}, observation_serial{}, observed_ms{}, context_generation{};
    Pad pad{};
    bool connected{}, eligible{};
};
}
