#pragma once
#include <array>
#include <cstdint>

// The local skater's animation pose, composed into world space along the
// verified joint chains. The pose itself comes from ReSkate's native layout
// (Extension/Multiplayer/Remote/native_pose_layout.h); the joint composition
// is the one client_first_person.cpp measured on a live skater (2026-09-21).
// Captured on the client tick; the overlay reads the latest snapshot.
namespace dingosdk::skater_pose {

inline constexpr std::uint32_t skeleton_joints = 395; // AnimBase_Default_Skeleton
inline constexpr std::uint16_t trajectory_joint = 1;  // carries the world placement
inline constexpr std::uint16_t head_joint = 103;
// Root -> neck -> head, the verified chain.
inline constexpr std::array<std::uint16_t, 10> head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};

struct Snapshot {
    bool valid{};
    std::array<std::array<float, 3>, head_chain.size()> chain{}; // world positions along head_chain
    std::array<float, 3> origin{};                               // the character's placement (joint 1)
    std::uint64_t at{};                                          // GetTickCount64() of the capture
};

// Client tick: read the local skater's pose and compose the known chains.
// `entity` is the resolved local skater entity (DebugModel::skater_identity).
bool capture(std::uintptr_t base, std::uintptr_t entity, Snapshot& out) noexcept;
// Overlay: the latest capture (invalid when never captured).
Snapshot latest() noexcept;
}
