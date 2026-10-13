#include "Extension/Style/style_recording.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace dingosdk;
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
// {0, 0, 0, 2} is an unnormalized identity rotation.
bool append(std::vector<std::uint8_t> &recording, std::uint64_t ms, std::uintptr_t holder, std::size_t joints) {
    const auto room = append_rig(recording, ms, holder, joints);
    if (room.empty()) return false;
    for (std::size_t i = 0; i < joints; ++i) {
        const RecordedBone bone{{0.5f, 0.5f, 0.5f, 0}, {0, 0, 0, 2}, {static_cast<float>(i), 1, 2, 0}};
        std::memcpy(room.data() + i * sizeof(bone), &bone, sizeof(bone));
    }
    return true;
}
} // namespace

int main() {
    check(rig_kind(addr::style::skeleton_joints) == RigKind::skater && rig_kind(2) == RigKind::board && rig_kind(32) == RigKind::board,
          "a skater has the skeleton's joints, a board 2 to 32");
    check(rig_kind(0) == RigKind::other && rig_kind(1) == RigKind::other && rig_kind(33) == RigKind::other, "other rigs are neither");
    std::vector<std::uint8_t> recording;
    check(append(recording, 16, 0x1000, addr::style::skeleton_joints) && append(recording, 17, 0x2000, 3),
          "a skater and a board are recorded");
    const auto size = recording.size();
    check(!append(recording, 18, 0x3000, 1) && recording.size() == size, "a 1-joint rig is not recorded");
    const auto rigs = read_recording(recording);
    check(rigs.size() == 2 && rigs[0].kind == RigKind::skater && rigs[0].frame.at == 16 && rigs[0].holder == 0x1000 &&
              rigs[0].frame.joints.size() == addr::style::skeleton_joints && rigs[1].kind == RigKind::board && rigs[1].holder == 0x2000,
          "the rigs read back in order");
    check(rigs[0].frame.joints[7].position == std::array<float, 3>{7, 1, 2} && rigs[0].frame.joints[7].rotation == Quat{0, 0, 0, 1},
          "a bone keeps its position and gets a normalized rotation");
    check(rigs[0].frame.joints[0].scale == std::array<float, 3>{0.5f, 0.5f, 0.5f} &&
              rigs[1].frame.joints[0].scale == std::array<float, 3>{1, 1, 1},
          "a skater keeps its scale and a board gets 1");
    // A board rig's bytes: three 64-bit words (ms, holder, joints), then the 48-byte bones.
    const std::uint64_t header[3]{20, 0x4000, 2};
    std::vector<std::uint8_t> now;
    const auto room = append_rig(now, 20, 0x4000, 2);
    check(room.size() == 2 * 48 && now.size() == sizeof(header) + 2 * 48 && std::memcmp(now.data(), header, sizeof(header)) == 0,
          "a rig's header and room are the bytes of the documented layout");
    recording.resize(recording.size() - 1);
    check(read_recording(recording).size() == 1, "a cut rig is not read");
    check(read_recording({}).empty(), "an empty recording has no rigs");
    {
        std::vector<std::uint8_t> oversized;
        (void)append(oversized, 1, 0x5000, 3);
        const RecordedHeader huge{2, 0x6000, max_rig_joints + 1};
        const auto at = oversized.size();
        oversized.resize(at + sizeof(huge) + (max_rig_joints + 1) * sizeof(RecordedBone));
        std::memcpy(oversized.data() + at, &huge, sizeof(huge));
        (void)append(oversized, 3, 0x7000, 3);
        const auto read = read_recording(oversized);
        check(read.size() == 1 && read[0].holder == 0x5000, "a header with an oversized joint count stops the read");
    }
    if (!failures) std::cout << "style recording tests passed\n";
    return failures ? 1 : 0;
}
