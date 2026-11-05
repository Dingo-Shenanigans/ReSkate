#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::plugins {
// The local skater's movement (reskate.skater.movement). velocity is the skater's in the world;
// walking_velocity the walk's own while grounded on foot. stick is the movement stick turned into the
// camera's frame, flat (y 0), as long as the stick is pushed (0 to 1); device is the controller input
// it came from. focused: the game window has focus and no menu, free camera or park editor is up.
struct SkaterMovement {
    std::array<float, 3> velocity{}, walking_velocity{}, stick{};
    bool grounded{}, on_foot{}, focused{};
    std::uint32_t device{}, physics_state{};
};
}
