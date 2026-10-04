#include "skater_pose.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/Core/Log/logging.h"

namespace dingosdk::skater_pose {
namespace {
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;

bool read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    return address >= 0x10000 && size <= 0x10000 && address <= highest - size &&
        memory::peek_bytes(address, out, size);
}

struct Joint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{};
    float scale = 1;
};

std::array<float, 3> rotate(const std::array<float, 4>& q, const std::array<float, 3>& v) noexcept {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
        tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
        v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}

// Appends one parent-local joint record (scale | quaternion | translation,
// 0x30 bytes) to an accumulated world transform.
bool child(const Joint& parent, std::uintptr_t buffer, std::uint16_t index, Joint& out) noexcept {
    std::array<float, 12> bone{};
    if (!read(buffer + index * 0x30ULL, bone.data(), sizeof(bone))) return false;
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(bone[i]) || std::abs(bone[i]) > 1000000.0f) return false;
    const auto& q = parent.rotation;
    const auto offset = rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    for (std::size_t i = 0; i < 3; ++i) out.position[i] = parent.position[i] + offset[i];
    out.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
        q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
        q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
        q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    out.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return true;
}

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    Snapshot snapshot;
};
State& state() { static auto* value = new State; return *value; }

// One-time dump of the skeleton definition and resource: the parent array and
// bone tables the pose evaluation walks are in here, and mapping them from a
// live capture fixes the overlay's joint set without guessing. Rows of ints,
// marked for offline parsing in the log.
void dump_skeleton_definition(const std::function<bool(std::uintptr_t, void*, std::size_t)>& read,
    std::uintptr_t holder) {
    std::uintptr_t rig{};
    if (!read(holder + 0x78, &rig, 8) || rig < 0x10000) return;
    std::uintptr_t definition{};
    if (!read(rig + 0x18, &definition, 8) || definition < 0x10000) return;
    std::uintptr_t resource{};
    if (!read(definition + 0x1a0, &resource, 8) || resource < 0x10000) return;
    std::uint32_t count{};
    if (!read(resource + 0xc, &count, 4) || count > 512) return;
    logging::log(logging::Level::info, logging::Channel::skater,
        "HallOfMeat skeleton dump begin (definition {:#x}, resource {:#x}, {} bones)",
        definition, resource, count);
    auto dump_ints = [&](const char* what, std::uintptr_t at, std::size_t bytes) {
        std::vector<std::int32_t> values(bytes / 4, 0);
        if (!read(at, values.data(), values.size() * 4)) {
            logging::log(logging::Level::warning, logging::Channel::skater,
                "HallOfMeat skeleton dump: {} unreadable", what);
            return;
        }
        for (std::size_t row = 0; row < values.size(); row += 8) {
            std::string line = std::format("{} +{:04x}:", what, row);
            for (std::size_t i = row; i < std::min(row + 8, values.size()); ++i)
                line += std::format(" {:11d}", values[i]);
            logging::write(logging::Level::info, logging::Channel::skater, line);
        }
    };
    dump_ints("def", definition, 0x80);
    dump_ints("res", resource, 0x1000);
    logging::write(logging::Level::info, logging::Channel::skater,
        "HallOfMeat skeleton dump end");
}

}

bool capture(std::uintptr_t base, std::uintptr_t entity, Snapshot& out) noexcept {
    out.valid = false;
    std::uintptr_t component{};
    std::uintptr_t vtable{};
    if (entity < 0x10000 || !read(entity + 0x628, &component, 8) || component < 0x10000 ||
        !read(component, &vtable, 8) || vtable != base + addr::engine::skater_component_vtable)
        return false;
    std::uintptr_t holder{};
    if (!read(component + 0xa0, &holder, 8) || !holder) return false;
    multiplayer::NativePoseLayout pose;
    try {
        pose = multiplayer::read_native_pose_layout(
            [](std::uintptr_t address, void* destination, std::size_t size) {
                return read(address, destination, size);
            }, base, holder, 512);
    } catch (...) {
        return false;
    }
    if (!pose.buffer || pose.count != skeleton_joints) return false;
    static std::atomic<bool> dumped{};
    if (!dumped.exchange(true)) {
        dump_skeleton_definition(
            [](std::uintptr_t address, void* destination, std::size_t size) {
                return read(address, destination, size);
            }, holder);
    }
    Snapshot snapshot;
    Joint joint;
    for (std::size_t index = 0; index < head_chain.size(); ++index) {
        const auto parent = joint;
        if (!child(parent, pose.buffer, head_chain[index], joint)) return false;
        snapshot.chain[index] = joint.position;
        if (head_chain[index] == trajectory_joint) snapshot.origin = joint.position;
    }
    if (!std::isfinite(snapshot.origin[0]) || !std::isfinite(snapshot.origin[1]) ||
        !std::isfinite(snapshot.origin[2]))
        return false;
    snapshot.valid = true;
    snapshot.at = GetTickCount64();
    {
        auto& s = state();
        AcquireSRWLockExclusive(&s.lock);
        s.snapshot = snapshot;
        ReleaseSRWLockExclusive(&s.lock);
    }
    out = snapshot;
    return true;
}

Snapshot latest() noexcept {
    auto& s = state();
    AcquireSRWLockShared(&s.lock);
    const auto snapshot = s.snapshot;
    ReleaseSRWLockShared(&s.lock);
    return snapshot;
}
}
