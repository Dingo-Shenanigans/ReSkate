#pragma once
#include "input_lease_capture.h"
#include <cstdint>

// A versioned output-buffer writer for one observed XInput stream. No actor,
// native clock, physical-device or game-consumption authority is represented.
namespace dingosdk::input_tracked_write {
using Pad = input_lease::Pad;
using Key = input_lease_capture::Key;
using Reason = input_lease_capture::Reason;
inline constexpr std::uint64_t write_lifetime_ms = 100, cleanup_boundary_ms = 1000;
inline constexpr std::uint32_t maximum_callbacks = input_lease_capture::maximum_callbacks;
struct Version {
    std::uint64_t generation{}, revision{};
    bool operator==(const Version&) const = default;
};
enum class Phase : std::uint8_t { pending, active, releasing, complete };
enum class WrittenKind : std::uint8_t { none, desired, cleanup, handback };
constexpr const char* phase_name(Phase value) noexcept {
    switch (value) {
    case Phase::pending: return "pending";
    case Phase::active: return "active";
    case Phase::releasing: return "releasing";
    case Phase::complete: return "complete";
    }
    return "invalid";
}
constexpr const char* written_kind_name(WrittenKind value) noexcept {
    switch (value) {
    case WrittenKind::none: return "none";
    case WrittenKind::desired: return "desired";
    case WrittenKind::cleanup: return "cleanup";
    case WrittenKind::handback: return "handback";
    }
    return "invalid";
}
constexpr const char* reason_name(Reason value) noexcept { return input_lease_capture::reason_name(value); }
struct Receipt {
    Key key{};
    Version version{};
    Phase phase{Phase::pending};
    Reason reason{Reason::none};
    std::uint64_t desired_revision{}, published_revision{};
    std::uint64_t write_expires_ms{}, source_expires_ms{};
    std::uint64_t version_publications{}, publications{}, cleanup_publications{}, handback_publications{};
    WrittenKind last_written_kind{WrittenKind::none};
    bool output_debt{}, output_released{}, quarantined{}, orphaned{};
    std::uint32_t polls_in_flight{};
    bool callbacks_unknown{};
};
}
