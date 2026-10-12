#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::server {
struct PoseExcess {
    float reach{}, scale{}, board_scale{};
};
inline constexpr float stock_bone_reach = 1.f, stock_bone_scale = 2.f, stock_board_scale = 1.1f;
inline constexpr float suspicious_reach = 1.25f, suspicious_scale = 1.25f, suspicious_board_scale = 1.25f;

inline PoseExcess pose_excess(const multiplayer::Pose &pose) {
    using multiplayer::Transform;
    const auto scale_of = [](const Transform &t) {
        float most = 1.f;
        for (const float axis : t.scale)
            if (std::isfinite(axis)) most = std::max(most, axis);
        return most;
    };
    const auto length = [](const std::array<float, 3> &a, const std::array<float, 3> &b) {
        float squared{};
        for (unsigned i = 0; i < 3; ++i) squared += (a[i] - b[i]) * (a[i] - b[i]);
        return std::sqrt(squared);
    };
    constexpr std::array<float, 3> origin{};
    PoseExcess out;
    out.scale = scale_of(pose.root) / stock_bone_scale;
    for (std::size_t i = 0; i < pose.skater.size(); ++i) {
        out.scale = std::max(out.scale, scale_of(pose.skater[i]) / stock_bone_scale);
        if (i == 1) continue;
        const bool free = std::find(multiplayer::free_skater_bones.begin(), multiplayer::free_skater_bones.end(), i) !=
                          multiplayer::free_skater_bones.end();
        out.reach = std::max(out.reach, length(pose.skater[i].position, origin) / (free ? multiplayer::free_bone_reach : stock_bone_reach));
    }
    for (std::size_t i = 0; i < pose.board.size(); ++i) {
        out.board_scale = std::max(out.board_scale, scale_of(pose.board[i]) / stock_board_scale);
        if (!i) continue;
        out.reach = std::max(out.reach, i == 2 ? length(pose.board[i].position, pose.board[0].position) / 5.f
                                               : length(pose.board[i].position, origin) / stock_bone_reach);
    }
    return out;
}
}
