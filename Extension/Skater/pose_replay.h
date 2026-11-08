#pragma once
#include "Extension/Skater/pose_offset.h"
#include "Extension/Skater/pose_place.h"
#include <array>
#include <utility>

// A pose request resolved against the complete skeleton it will be drawn on. Keeping the request,
// rather than its animation-time local bones, lets world and joint frames use the current parents.
namespace dingosdk::pose_replay {
struct Entry {
    std::uint16_t joint{};
    anim_studio::Quat rotation{0, 0, 0, 1};
    anim_studio::Vec3 scale{1, 1, 1}; // each of its own axes
    anim_studio::Vec3 offset{}; // added in the parent's frame
    std::optional<pose_place::Place> place; // resolved after the offsets
    bool keep_children{}; // direct children keep their current world transforms
};
struct Range {
    std::uint32_t first{}, last{};
};
// Entries are in pose_place::order. Children are captured before any part of their parent's edit.
Range apply(std::span<anim_studio::Joint> pose, std::span<const Entry> entries, float weight);

// The renderer's bone lanes. The fourth scale and position lanes belong to the game and are opaque.
struct alignas(16) NativeBone {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(NativeBone) == 0x30);
using Written = std::vector<std::pair<std::uint16_t, pose_offset::Written>>;

// Resolves the requests on a copy of the current native pose. On success, only affected bones change;
// before contains the exact range to restore after rendering. A previous overlay still in the buffer
// is recovered first, so partial placement, scales and offsets never accumulate across draws.
// A broken result leaves every input and the previous write record unchanged.
bool draw(std::span<NativeBone> current, std::span<const Entry> entries, float weight,
          Written& written, std::vector<NativeBone>& before, Range& range);
} // namespace dingosdk::pose_replay
