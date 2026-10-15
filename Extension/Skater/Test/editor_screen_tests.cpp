#include "Extension/Skater/editor_screen.h"
#include <iostream>

namespace {
using namespace dingosdk;
int failures{}, opens{}, closes{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
bool editor_pose(std::array<float, 16> &matrix) noexcept {
    matrix[0] = 1;
    return true;
}
bool held_pose(std::array<float, 16> &matrix) noexcept {
    matrix[0] = 2;
    return true;
}
float editor_fov() noexcept { return 0.5f; }
void on_open() noexcept { ++opens; }
void on_close() noexcept { ++closes; }
} // namespace

int main() {
    std::array<float, 16> matrix{};
    editor_screen::opened();
    editor_screen::closed();
    check(!editor_screen::camera_pose(matrix) && !editor_screen::held_camera_pose(matrix) && editor_screen::camera_fov() == 0 &&
              matrix[0] == 0,
          "before a screen is set, there is no camera and the fov is kept");
    static const editor_screen::Screen screen{editor_pose, editor_fov, on_open, on_close, held_pose};
    editor_screen::set(screen);
    check(editor_screen::camera_pose(matrix) && matrix[0] == 1, "the screen's camera is used");
    check(editor_screen::held_camera_pose(matrix) && matrix[0] == 2, "and its held camera");
    check(editor_screen::camera_fov() == 0.5f, "and its fov");
    editor_screen::opened();
    editor_screen::closed();
    check(opens == 1 && closes == 1, "an open and a close reach the screen");
    if (!failures) std::cout << "editor screen tests passed\n";
    return failures ? 1 : 0;
}
