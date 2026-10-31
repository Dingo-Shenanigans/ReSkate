#pragma once
#include "Engine/Game/Input/input_lease_capture.h"
#include "Extension/Input/native_input_source.h"

// Selected observed-XInput capture only. No actor/body/action/clock authority or
// physical device identity is granted. All four operations use the SAME opaque
// passive lease handle; no plugin chooses a native key or writes a report.
namespace dingosdk::native_input_source {
bool capture_request(std::uint64_t owner, input_lease::Handle,
    const input_lease::Snapshot&, input_lease_capture::Reason&) noexcept;
bool capture_state(std::uint64_t owner, input_lease::Handle,
    input_lease_capture::Receipt&) noexcept;
bool capture_release(std::uint64_t owner, input_lease::Handle) noexcept;
bool capture_acknowledge(std::uint64_t owner, input_lease::Handle) noexcept;
// Existing lease_release_all(owner) is the idempotent owner-close operation for
// both passive and capture records. Unconfirmed debt and exact callbacks remain
// retained; a closed owner cannot be adopted by a new VM.

// Internal trusted leaf supplied ONLY by the already-installed raw wrappers.
// While holding the publication gate it writes one full packet+Pad value to its
// caller-owned output, with protected-copy failure reported as false. No locks,
// allocation, environment/native graph reads, SDK calls, plugin callbacks or
// extension filters are permitted here. This is not a registration or Lua API.
// staged_revision is sampled before the upstream candidate flags/filters; the
// finalizer checks it again before writing. The begin revision is separate.
using PublishReport = bool (*)(void*, std::uint32_t, const input_lease::Pad&) noexcept;
std::uint32_t finish_capture(Poll&, std::uint32_t original_result, bool raw_known,
    std::uint32_t raw_packet, const input_lease::Pad& raw,
    std::uint32_t returned_result, bool returned_known, std::uint32_t returned_packet,
    const input_lease::Pad& returned, std::uint64_t staged_revision,
    void* output, PublishReport) noexcept;
}
