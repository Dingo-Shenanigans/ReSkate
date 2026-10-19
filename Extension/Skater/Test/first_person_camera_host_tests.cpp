#include "Extension/Skater/first_person_camera_host.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk;
void check(bool value) { if (!value) throw std::runtime_error("Camera host contract failed"); }
unsigned resets{};
int main() { try {
    const first_person::Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    auto pose = identity;
    first_person_host::compose(1, pose); check(pose == identity);
    static const first_person_host::CameraCallbacks hooks{
        [](first_person::Settings& settings, double) noexcept { settings.stabilize = false; },
        [](std::uintptr_t camera, first_person::Matrix& matrix) noexcept { matrix[12] = float(camera); },
        []() noexcept { ++resets; }};
    first_person_host::camera_callbacks.store(&hooks);
    first_person_host::compose(7, pose); check(pose[12] == 7);
    first_person::Settings saved; saved.stabilize = true;
    first_person::FrameSpring spring; spring.begin(1);
    (void)first_person_host::sample(spring, identity, saved, {}); check(saved.stabilize);
    first_person_host::reset_camera(); check(resets == 1);
    static const first_person_host::CameraCallbacks invalid{
        [](first_person::Settings& settings, double) noexcept { settings.offset[0] = 100; },
        [](std::uintptr_t, first_person::Matrix& matrix) noexcept { matrix[0] = 0; }, nullptr};
    first_person_host::camera_callbacks.store(&invalid);
    const auto before = pose; first_person_host::compose(7, pose); check(pose == before);
    first_person::FrameSpring native, addon; native.begin(2); addon.begin(2);
    check(first_person_host::sample(addon, identity, saved, {}) == native.sample(identity, saved, {}));
    static const first_person_host::CameraCallbacks turning{nullptr,nullptr,nullptr,
        [](std::uintptr_t rig,std::uintptr_t context,first_person::Matrix& matrix) noexcept {
            if(rig!=4 || context!=3) return false;matrix[12]=7;return true;
        }};
    first_person_host::camera_callbacks.store(&turning);
    auto motion=identity;
    check(!first_person_host::compose_motion(4,3,motion) && motion==identity);
    first_person_host::motion_enabled.store(true);
    check(!first_person_host::compose_motion(9,3,motion) && motion==identity);
    check(first_person_host::compose_motion(4,3,motion) && motion[12]==7);
    static const first_person_host::CameraCallbacks bad_motion{nullptr,nullptr,nullptr,
        [](std::uintptr_t,std::uintptr_t,first_person::Matrix& matrix) noexcept {matrix[0]=0;return true;}};
    first_person_host::camera_callbacks.store(&bad_motion);
    motion=identity;check(!first_person_host::compose_motion(4,3,motion) && motion==identity);
    first_person_host::motion_enabled.store(false);
    first_person_host::camera_callbacks.store(nullptr);
    first_person_host::reset_camera(); check(resets == 1);
    // A fresh render pose refines this frame, rather than advancing Spring twice.
    first_person::FrameSpring refined,reference;
    first_person::Settings settings;settings.enabled=true;settings.stabilize=false;
    settings.up=settings.down=settings.left=settings.right=70;
    for(auto* state:{&refined,&reference}){state->begin(10);(void)state->sample(identity,settings,{});state->begin(10.016);}
    auto earlier=identity;earlier[12]=.2f;
    auto fresh=identity;fresh[12]=.4f;
    (void)refined.sample(earlier,settings,{});
    check(refined.sample(fresh,settings,{})==reference.sample(fresh,settings,{}));
    // Received world motion is followed immediately, independent of relative head smoothing.
    fresh[12]=1.4f;
    refined.begin(10.032);
    const auto translated=refined.sample(fresh,settings,{1,0,0});
    check(translated[12]>1 && translated[12]<1.4f);
    std::cout << "Camera host contracts passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what(); return 1; } }
