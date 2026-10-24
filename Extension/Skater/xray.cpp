#include "xray.h"
#include "xray_render.h"
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace dingosdk::xray {
namespace {
// The picture is followed this long after the last frame that drew the x-ray.
constexpr std::uint64_t follow_ms = 1000;
struct State {
    std::atomic<bool> ready{};
    std::mutex mutex;
    std::set<std::string, std::less<>> owners; // client thread
    std::set<std::string, std::less<>> hiders; // client thread: plugins hiding the local skater
    bool hooked{};                             // client thread
    std::atomic<std::uint64_t> drawn_at{};     // render thread -> client tick
};
State& state() {
    static auto* value = new State; // the renderer's hooks may call in until exit
    return *value;
}
}

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return true;
    if (!start_render(base)) return false; // logged
    s.ready.store(true, std::memory_order_release);
    return true;
}
bool available() noexcept { return state().ready.load(std::memory_order_acquire); }

void prepare(std::string_view owner, bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (on) {
        s.owners.emplace(owner);
        prepare_skeleton(); // once, in the background
    } else if (const auto found = s.owners.find(owner); found != s.owners.end()) {
        s.owners.erase(found);
    }
}
bool hide(std::string_view owner, bool on) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!on) {
        if (const auto found = s.hiders.find(owner); found != s.hiders.end()) s.hiders.erase(found);
        return true;
    }
    if (!s.ready.load(std::memory_order_acquire)) return false;
    s.hiders.emplace(owner);
    return true;
}

void tick() noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        bool xray{}, hiding{};
        {
            std::lock_guard lock(s.mutex);
            xray = !s.owners.empty();
            hiding = !s.hiders.empty();
        }
        const bool drawn = GetTickCount64() - s.drawn_at.load(std::memory_order_acquire) < follow_ms;
        follow_render((xray && drawn) || hiding);
        hide_skater(hiding);
        const bool wanted = xray || hiding;
        if (wanted != s.hooked && hook_render(wanted)) s.hooked = wanted;
    } catch (...) {
    }
}

bool latest(Pose& pose) {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return false;
    s.drawn_at.store(GetTickCount64(), std::memory_order_release);
    auto mesh = skeleton_mesh();
    Picture picture;
    PosedSkeleton posed;
    if (!mesh || !latest_picture(picture) || !pose_skeleton(*mesh, picture.skin, posed)) return false;
    pose.camera = picture.camera;
    pose.vertical_fov = picture.vertical_fov;
    pose.positions = std::move(posed.positions);
    pose.normals = std::move(posed.normals);
    pose.mesh = std::move(mesh);
    return true;
}
}
