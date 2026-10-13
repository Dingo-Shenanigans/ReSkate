#pragma once
#include <array>

// A screen that takes over the camera (a feature's editor), and a camera a feature holds for a moment (a capture).
// The debug camera code calls these; each call is safe before set().
namespace dingosdk::editor_screen {
struct Screen {
    bool (*camera_pose)(std::array<float, 16> &matrix) noexcept = nullptr; // rows right, up, backward, position
    float (*camera_fov)() noexcept = nullptr;                               // 0 keeps the current fov
    void (*opened)() noexcept = nullptr;
    void (*closed)() noexcept = nullptr;
    bool (*held_camera_pose)(std::array<float, 16> &matrix) noexcept = nullptr;
};
// `screen` must live as long as the program.
void set(const Screen &screen) noexcept;
void set(const Screen &&) = delete;
[[nodiscard]] bool camera_pose(std::array<float, 16> &matrix) noexcept;
[[nodiscard]] float camera_fov() noexcept;
void opened() noexcept;
void closed() noexcept;
[[nodiscard]] bool held_camera_pose(std::array<float, 16> &matrix) noexcept;
} // namespace dingosdk::editor_screen
