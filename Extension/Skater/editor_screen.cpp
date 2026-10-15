#include "editor_screen.h"
#include <atomic>

namespace dingosdk::editor_screen {
namespace {
std::atomic<const Screen *> g_screen{nullptr};
const Screen &current() noexcept {
    static constexpr Screen none{};
    const auto *screen = g_screen.load(std::memory_order_acquire);
    return screen ? *screen : none;
}
} // namespace

void set(const Screen &screen) noexcept { g_screen.store(&screen, std::memory_order_release); }

bool camera_pose(std::array<float, 16> &matrix) noexcept {
    const auto pose = current().camera_pose;
    return pose && pose(matrix);
}

float camera_fov() noexcept {
    const auto fov = current().camera_fov;
    return fov ? fov() : 0.0f;
}

void opened() noexcept {
    if (const auto notify = current().opened) notify();
}

void closed() noexcept {
    if (const auto notify = current().closed) notify();
}

bool held_camera_pose(std::array<float, 16> &matrix) noexcept {
    const auto pose = current().held_camera_pose;
    return pose && pose(matrix);
}
} // namespace dingosdk::editor_screen
