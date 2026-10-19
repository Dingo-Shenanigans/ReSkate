#include "Engine/Game/Skater/first_person_fov.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dingosdk::first_person;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() { try {
    check(valid_fov_setting(0) && valid_fov_setting(40) && valid_fov_setting(120), "Preference bounds rejected");
    check(!valid_fov_setting(1) && !valid_fov_setting(-1) && !valid_fov_setting(121), "Invalid preference accepted");
    check(!valid_fov_setting(std::numeric_limits<float>::quiet_NaN()), "NaN accepted");
    check(fov_write_target(0, 55) == 55, "Reset did not restore captured camera FOV");
    check(fov_write_target(0, 135) == 135, "Camera-owned FOV wrongly clamped to override range");
    check(fov_write_target(90, 0) == 90, "Custom FOV unnecessarily needs capture");
    check(!fov_write_target(0, 0) && !fov_write_target(0, 175) && !fov_write_target(20, 55), "Unsafe reset target accepted");
    // Repeated custom/default changes keep using the original capture, never the override.
    const float captured = 65;
    check(fov_write_target(100, captured) == 100 && fov_write_target(0, captured) == 65 &&
          fov_write_target(75, captured) == 75 && fov_write_target(0, captured) == 65, "Override polluted original FOV");
    std::cout << "Native FOV sentinel, reset target and preference validation passed.\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
