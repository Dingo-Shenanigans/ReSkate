#pragma once
#include "Engine/Game/Skater/first_person_spring.h"
#include "Extension/Skater/free_flight.h"
#include <atomic>
#include <cstdint>

namespace dingosdk::first_person_host {
// Process-lifetime source integration only: callbacks must not unload, block,
// write engine memory, or acquire the engine state mutex. Runs on camera thread.
// All arguments are transient; saved native settings are never modified.
struct CameraCallbacks {
    void (*sample_settings)(first_person::Settings&, double) noexcept{};
    void (*compose)(std::uintptr_t, first_person::Matrix&) noexcept{};
    void (*reset)() noexcept{};
    bool (*motion)(std::uintptr_t, std::uintptr_t, first_person::Matrix&) noexcept{};
};
inline std::atomic<const CameraCallbacks*> camera_callbacks{};
inline std::atomic<bool> motion_enabled{};
inline first_person::Matrix sample(first_person::FrameSpring& spring,
    const first_person::Matrix& head, const first_person::Settings& saved,
    const first_person::Vec3& origin) {
    auto effective = saved;
    if (const auto* hooks = camera_callbacks.load(std::memory_order_acquire)) {
        if (hooks->sample_settings) hooks->sample_settings(effective, spring.frame_time);
        if (!first_person::valid(effective)) effective = saved;
    }
    return spring.sample(head, effective, origin);
}
inline void compose(std::uintptr_t camera, first_person::Matrix& pose) noexcept {
    if (const auto* hooks = camera_callbacks.load(std::memory_order_acquire)) {
        if (!hooks->compose) return;
        auto candidate = pose;
        hooks->compose(camera, candidate);
        if (!valid_flight_transform(candidate)) return;
        pose = candidate;
    }
}
inline bool compose_motion(std::uintptr_t rig,std::uintptr_t context,first_person::Matrix& pose) noexcept {
    if(!motion_enabled.load(std::memory_order_acquire)) return false;
    if(const auto* hooks=camera_callbacks.load(std::memory_order_acquire)) {
        if(!hooks->motion) return false;
        auto candidate=pose;
        if(!hooks->motion(rig,context,candidate) || !valid_flight_transform(candidate)) return false;
        pose=candidate;return true;
    }
    return false;
}
inline void reset_camera() noexcept {
    if (const auto* hooks = camera_callbacks.load(std::memory_order_acquire))
        if (hooks->reset) hooks->reset();
}
}
