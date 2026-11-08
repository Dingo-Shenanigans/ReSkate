#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace dingosdk::traffic_vehicles {
using Vec3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>;
struct Pose { Vec3 position{}; Quaternion rotation{0, 0, 0, 1}; };
enum class State { finding, held, lost, released };
struct Sample { State state{State::lost}; Pose pose{}; std::string reason; };
inline bool within(const Vec3& a, const Vec3& b, float metres) noexcept {
    float squared{};
    for (int i = 0; i < 3; ++i) squared += (a[i] - b[i]) * (a[i] - b[i]);
    return std::isfinite(squared) && squared <= metres * metres;
}
inline bool parent_candidate(std::uint64_t child, std::uint64_t parent, const Vec3& child_world,
                             const Vec3& parent_world, unsigned climbs) noexcept {
    return parent && parent != child && climbs < 4 && within(child_world, parent_world, 10);
}
inline bool agent_candidate(std::uint64_t root, std::uint64_t other, const Vec3& root_position,
                            const Vec3& other_position) noexcept {
    return root && other != root && (other >> 32) != (root >> 32) &&
        within(root_position, other_position, 1);
}
// The returned number is opaque and is always checked together with the VM instance.
std::string take(std::uintptr_t image_base, std::uint64_t owner, std::uint64_t entity,
                 std::uint64_t root_hint, std::uint64_t& handle);
Sample read(std::uint64_t owner, std::uint64_t handle);
std::string set_pose(std::uint64_t owner, std::uint64_t handle, const Pose& pose);
bool release(std::uint64_t owner, std::uint64_t handle);
void release_all(std::uint64_t owner);
inline bool normalize(Quaternion& q) noexcept {
    double length{};
    for (float x : q) { if (!std::isfinite(x)) return false; length += static_cast<double>(x) * x; }
    if (!(length > 1e-8)) return false;
    const auto divisor = static_cast<float>(std::sqrt(length));
    for (auto& x : q) x /= divisor;
    return true;
}
}
