#pragma once
#include "Extension/Style/style_clips.h"
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The layer's recording of the local skater and the boards, which the editor reads to make a clip. No game access.
namespace dingosdk::style {
struct RecordedBone {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(RecordedBone) == addr::style::bone_size);
static_assert(offsetof(RecordedBone, rotation) == addr::style::bone_rotation);
static_assert(offsetof(RecordedBone, position) == addr::style::bone_position);
// Each recorded rig starts with this, then `joints` bones.
struct RecordedHeader {
    std::uint64_t ms{}, holder{}, joints{};
};
struct RecordedRig {
    RigKind kind{};
    std::uint64_t holder{};
    RigFrame frame;
};
// Returns the room for the bones. Appends nothing for another kind of rig.
std::span<std::byte> append_rig(std::vector<std::uint8_t> &recording, std::uint64_t ms, std::uintptr_t holder, std::size_t joints);
// A board's scale reads as 1: its recorded scale can be the hidden one.
[[nodiscard]] std::vector<RecordedRig> read_recording(std::span<const std::uint8_t> recording);
} // namespace dingosdk::style
