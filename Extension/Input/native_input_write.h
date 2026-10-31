#pragma once
#include "Engine/Game/Input/input_tracked_write.h"

// Four tracked, owner-bound controller-write methods. All operations use the
// existing passive lease handle; a native Version is a query identity only.
namespace dingosdk::native_input_source {
bool write_request(std::uint64_t owner, input_lease::Handle,
    const input_lease::Snapshot&, const input_lease::Pad&,
    input_tracked_write::Version&, input_tracked_write::Reason&) noexcept;
bool write_state(std::uint64_t owner, input_lease::Handle,
    input_tracked_write::Version, input_tracked_write::Receipt&) noexcept;
// Stop new desired writes immediately. Already-owed output remains reserved.
// Repeated release of the same retained key is accepted; no sample is needed.
bool write_release(std::uint64_t owner, input_lease::Handle) noexcept;
// Only positive output handback and known selected callback drain permit ACK.
bool write_acknowledge(std::uint64_t owner, input_lease::Handle) noexcept;
// Existing lease_release/all owner close also revokes tracked writes; no extra
// cleanup API and no GC actuator. Lost/partial/unknown duty is retained.
}
