#pragma once
#include <array>
#include <cstddef>

namespace dingosdk::overlay { struct Model; struct CallbacksV3; }
namespace dingosdk::first_person_host {
// A transient read of ReSkate's values, never a second preference store.
// No vendor camera types cross this boundary: Jish's Spring layout differs.
struct Snapshot {
    bool available{}, enabled{}, spring{}, board_only{};
    float fov{}, game_fov{};
    std::array<float, 3> offset{}, rotation{};
    std::array<float, 4> strength{}; // up, down, left, right
    bool stabilize{}, follow_flips{}, camera_cycle{};
    float smoothing{}, head_pitch{}, head_roll{}, bob{};
};
enum class Field {
    enabled, fov, spring, board_only,
    offset_x, offset_y, offset_z, pitch, yaw, roll,
    spring_up, spring_down, spring_left, spring_right,
    stabilize, follow_flips, camera_cycle, smoothing, head_pitch, head_roll, bob
};
Snapshot read(const overlay::Model& model) noexcept;
// Queue one native setting. No direct engine writes and no cached ownership.
// Acceptance means queued, not yet applied; subsequent snapshots are authoritative.
bool queue(const overlay::CallbacksV3& callbacks, Field field, float value,
           char* result, std::size_t result_size);
}
