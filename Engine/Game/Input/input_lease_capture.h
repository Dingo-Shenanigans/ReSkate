#pragma once
#include "Engine/Game/Input/input_lease.h"
#include <cstdint>

namespace dingosdk::input_lease_capture {
using Pad = input_lease::Pad;
inline constexpr std::uint32_t maximum_callbacks = 128;
struct Key {
    std::uint64_t owner{}, lease_sequence{}, source_epoch{}, context_generation{};
    std::uint32_t user{};
    bool operator==(const Key&) const = default;
};
enum class Phase { idle, waiting_neutral, capturing, releasing, complete };
enum class Reason {
    none, unavailable, invalid_key, busy, exhausted, source_changed, source_stale,
    clock_regressed, authority_lost, context_changed, deadline,
    focus_lost, gameplay_inactive, menu_open, paused, interference,
    cancelled, publication_unconfirmed
};
constexpr const char* phase_name(Phase value) noexcept {
    switch (value) {
    case Phase::idle: return "idle";
    case Phase::waiting_neutral: return "waiting_neutral";
    case Phase::capturing: return "capturing";
    case Phase::releasing: return "releasing";
    case Phase::complete: return "complete";
    }
    return "invalid";
}
constexpr const char* reason_name(Reason value) noexcept {
    switch (value) {
    case Reason::none: return "none";
    case Reason::unavailable: return "unavailable";
    case Reason::invalid_key: return "invalid_key";
    case Reason::busy: return "busy";
    case Reason::exhausted: return "exhausted";
    case Reason::source_changed: return "source_changed";
    case Reason::source_stale: return "source_stale";
    case Reason::clock_regressed: return "clock_regressed";
    case Reason::authority_lost: return "authority_lost";
    case Reason::context_changed: return "context_changed";
    case Reason::deadline: return "deadline";
    case Reason::focus_lost: return "focus_lost";
    case Reason::gameplay_inactive: return "gameplay_inactive";
    case Reason::menu_open: return "menu_open";
    case Reason::paused: return "paused";
    case Reason::interference: return "interference";
    case Reason::cancelled: return "cancelled";
    case Reason::publication_unconfirmed: return "publication_unconfirmed";
    }
    return "invalid";
}
struct Receipt {
    Key key{};
    Phase phase{Phase::idle};
    Reason reason{Reason::none};
    std::uint64_t generation{}, revision{}, expires_ms{}, publications{}, neutral_releases{};
    bool debt{}, quarantined{};
    // These two values are copied by the native owner service, not the pure policy.
    std::uint32_t polls_in_flight{};
    bool orphaned{};
    // Callback capacity/counter failure retains unknown duty; ACK is forbidden.
    bool callbacks_unknown{};
};
}
