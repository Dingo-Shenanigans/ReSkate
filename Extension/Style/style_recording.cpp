#include "Extension/Style/style_recording.h"
#include <cstring>
#include <utility>

namespace dingosdk::style {
namespace {
Transform transform_of(const RecordedBone &bone, RigKind kind) noexcept {
    const std::array<float, 3> scale = kind == RigKind::skater ? std::array<float, 3>{bone.scale[0], bone.scale[1], bone.scale[2]}
                                                               : std::array<float, 3>{1, 1, 1};
    return {{bone.position[0], bone.position[1], bone.position[2]}, normalized(bone.rotation), scale};
}
} // namespace

std::span<std::byte> append_rig(std::vector<std::uint8_t> &recording, std::uint64_t ms, std::uintptr_t holder, std::size_t joints) {
    if (rig_kind(joints) == RigKind::other) return {};
    const RecordedHeader header{ms, holder, joints};
    const auto at = recording.size();
    recording.resize(at + sizeof(header) + joints * sizeof(RecordedBone));
    std::memcpy(recording.data() + at, &header, sizeof(header));
    return {reinterpret_cast<std::byte *>(recording.data() + at + sizeof(header)), joints * sizeof(RecordedBone)};
}

std::vector<RecordedRig> read_recording(std::span<const std::uint8_t> recording) {
    std::vector<RecordedRig> rigs;
    for (std::size_t at = 0; at + sizeof(RecordedHeader) <= recording.size();) {
        RecordedHeader header;
        std::memcpy(&header, recording.data() + at, sizeof(header));
        at += sizeof(header);
        if (header.joints > max_rig_joints || header.joints * sizeof(RecordedBone) > recording.size() - at) break;
        const auto joints = static_cast<std::size_t>(header.joints);
        if (const auto kind = rig_kind(joints); kind != RigKind::other) {
            RecordedRig rig{kind, header.holder, {static_cast<std::uint32_t>(header.ms), {}}};
            rig.frame.joints.reserve(joints);
            for (std::size_t i = 0; i < joints; ++i) {
                RecordedBone bone;
                std::memcpy(&bone, recording.data() + at + i * sizeof(bone), sizeof(bone));
                rig.frame.joints.push_back(transform_of(bone, kind));
            }
            rigs.push_back(std::move(rig));
        }
        at += joints * sizeof(RecordedBone);
    }
    return rigs;
}
} // namespace dingosdk::style
