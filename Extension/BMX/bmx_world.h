#pragma once
#include "bmx_visual.h"
#include <cstdint>
#include <cstddef>
#include <string>

namespace dingosdk::bmx::world {
inline constexpr std::size_t part_count=8;
struct Snapshot {
    bool active{}, ready{};
    unsigned live_parts{};
    std::uint64_t entities_created{}, entity_placements{};
    std::string issue;
};
struct PlacementReceipt {
    std::uint64_t revision{};std::uintptr_t entity{};unsigned live{},placed{};bool complete{};
};
// Call every verified client tick, also with inactive {} during dismount/loading.
// Cleanup and all normal engine factory/setter calls stay on that native realm.
// No native operation is performed by a static destructor or overlay callback.
PlacementReceipt tick(std::uintptr_t base, std::uintptr_t client, const visual::Frame& frame, std::uint64_t revision=0) noexcept;
Snapshot snapshot(); // last client tick; counters measure native calls, not pixels
} // namespace dingosdk::bmx::world
