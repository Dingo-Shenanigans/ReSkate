#pragma once
#include <cstdint>

namespace dingosdk::skater_action {
enum class Kind : std::uint8_t { on_foot, board };
enum class Status : std::uint8_t {
    pending, offered, selected, completed, already_target, rejected, cancelled, expired, invalidated
};
enum class Reason : std::uint8_t {
    none, invalid_owner, invalid_kind, unavailable, busy, receipt_capacity, context_changed,
    focus_changed, teleporting, motion_owned, actor_changed, actor_destroyed, world_changed,
    unsupported_state, protection_conflict, request_bits_conflict, request_write_failed,
    request_restore_failed, native_refused, selector_overridden, cancelled, expired, owner_released,
    native_lifetime_changed
};
struct Handle { std::uint64_t sequence{}; };
struct Receipt {
    Handle handle{};
    Kind kind{};
    Status status{Status::pending};
    Reason reason{Reason::none};
    std::uint64_t requested_ms{}, offered_ms{}, observed_ms{};
    bool original_known{}, final_known{}, committed_known{};
    std::uint32_t original_candidate{}, final_candidate{}, committed_state{};
    bool ragdoll_known{}, ragdoll{}, in_air_known{}, in_air{};
    bool flag_restored{};
    bool quarantined{}; // terminal failure may still hold the native request guard
};
constexpr bool terminal(Status status) noexcept {
    return status != Status::pending && status != Status::offered && status != Status::selected;
}
constexpr const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::pending: return "pending";
    case Status::offered: return "offered";
    case Status::selected: return "selected";
    case Status::completed: return "completed";
    case Status::already_target: return "already_target";
    case Status::rejected: return "rejected";
    case Status::cancelled: return "cancelled";
    case Status::expired: return "expired";
    case Status::invalidated: return "invalidated";
    }
    return "invalid";
}
constexpr const char* reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::none: return "none";
    case Reason::invalid_owner: return "invalid_owner";
    case Reason::invalid_kind: return "invalid_kind";
    case Reason::unavailable: return "unavailable";
    case Reason::busy: return "busy";
    case Reason::receipt_capacity: return "receipt_capacity";
    case Reason::context_changed: return "context_changed";
    case Reason::focus_changed: return "focus_changed";
    case Reason::teleporting: return "teleporting";
    case Reason::motion_owned: return "motion_owned";
    case Reason::actor_changed: return "actor_changed";
    case Reason::actor_destroyed: return "actor_destroyed";
    case Reason::world_changed: return "world_changed";
    case Reason::unsupported_state: return "unsupported_state";
    case Reason::protection_conflict: return "protection_conflict";
    case Reason::request_bits_conflict: return "request_bits_conflict";
    case Reason::request_write_failed: return "request_write_failed";
    case Reason::request_restore_failed: return "request_restore_failed";
    case Reason::native_refused: return "native_refused";
    case Reason::selector_overridden: return "selector_overridden";
    case Reason::cancelled: return "cancelled";
    case Reason::expired: return "expired";
    case Reason::owner_released: return "owner_released";
    case Reason::native_lifetime_changed: return "native_lifetime_changed";
    }
    return "invalid";
}
} // namespace dingosdk::skater_action
