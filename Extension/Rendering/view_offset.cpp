#include "view_offset.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/game_views.h"
#include "Extension/Plugins/plugin_camera.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <numbers>

namespace dingosdk::view_offset {
namespace {
namespace views = addr::game_views;
using Transform = views::Transform;
using Matrix = std::array<float, 16>;
using Vec = std::array<float, 3>;
// Every register argument as it came (the id is a word the game widened; the rest are pointers).
using Fill = void (*)(std::uintptr_t description, std::uintptr_t id, std::uintptr_t settings, std::uintptr_t camera_view, std::uintptr_t beside,
                      std::uintptr_t view);
using ReplayView = void (*)(std::uintptr_t description, std::uintptr_t context);
using SetTransform = void (*)(std::uintptr_t view, const Transform* transform);
using SetFov = void (*)(std::uintptr_t view, float fov);

constexpr std::uint64_t holds_ms = 300;     // an offset asked for holds this long (asked every tick)
constexpr std::uint64_t unhook_after_ms = 1000; // and the hooks go once none has held for this long

struct State {
    // The offset asked for: the client thread writes it, the game's view building reads it.
    std::mutex mutex;
    std::string owner;
    Offset offset;
    std::uint64_t until{};
    // The hooks (client thread only, under `hooks`).
    std::mutex hooks;
    bool prepared{}, failed{}, enabled{};
    void* fill_target{};
    void* replay_target{};
    std::uint64_t idle_since{};
    std::atomic<Fill> fill{};
    std::atomic<ReplayView> replay_original{};
    std::atomic<SetTransform> set_transform{};
    std::atomic<SetFov> set_fov{};
};
// Lives until the process ends: the detours can run at any time.
State& state() {
    static auto* value = new State;
    return *value;
}

// The view the fill gave the offset to this frame, what it made of it, and the field of view it set: the replay camera's
// say comes right after on the same thread, and gives the offset to its own camera in turn.
thread_local std::uintptr_t moved_description{};
thread_local Matrix moved_world{};
thread_local Offset moved_offset{};
thread_local float moved_fov{};

std::optional<Offset> holding() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.owner.empty() || GetTickCount64() > s.until) return std::nullopt;
    return s.offset;
}
Vec row(const Matrix& m, std::size_t r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec normal(const Vec& a) {
    const float length = std::sqrt(dot(a, a));
    return length > 1e-12f ? scale(a, 1.0f / length) : a;
}
// `a` turned towards `b` by `degrees`, and `b` with it.
void turn(Vec& a, Vec& b, float degrees) {
    if (degrees == 0) return;
    const float radians = degrees * std::numbers::pi_v<float> / 180.0f;
    const float c = std::cos(radians), s = std::sin(radians);
    const Vec turned_a = add(scale(a, c), scale(b, s)), turned_b = add(scale(b, c), scale(a, -s));
    a = turned_a;
    b = turned_b;
}
// A camera's transform: finite, within 10,000 km, its axes of unit length, at right angles and right-handed.
bool camera_like(const Matrix& m) noexcept {
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(m[r * 4 + axis]) || std::abs(m[r * 4 + axis]) > 1e7f) return false;
    for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = a; b < 3; ++b)
            if (std::abs(dot(row(m, a), row(m, b)) - (a == b ? 1.0f : 0.0f)) > 0.05f) return false;
    return dot(cross(row(m, 0), row(m, 1)), row(m, 2)) > 0.5f;
}
bool same_camera(const Matrix& a, const Matrix& b) noexcept {
    for (std::size_t r = 0; r < 4; ++r)
        if (std::memcmp(a.data() + r * 4, b.data() + r * 4, 3 * sizeof(float)) != 0) return false;
    return true;
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& f) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + f.rva, bytes) && bytes == f.bytes;
}
// The description's first view given the offset's field of view, through the game's own setter (not one with a projection
// of its own). Returns what it holds after, or 0 when it was left as it was.
float give_fov(std::uintptr_t description, const Offset& offset) noexcept {
    if (offset.fov <= 0 && offset.zoom == 1.0f) return 0;
    const auto setter = state().set_fov.load(std::memory_order_acquire);
    float fov{};
    std::uint8_t own_projection{};
    if (!setter || !memory::peek(description + views::description_fov, fov) ||
        !memory::peek(description + views::description_own_projection, own_projection) || own_projection || !std::isfinite(fov) ||
        fov < 0.02f || fov > 3.1f)
        return 0;
    const float wanted = field_of_view(fov, offset);
    if (std::abs(wanted - fov) > 1e-6f) setter(description, wanted);
    return wanted;
}
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};

// The game filling a view description in: the first local player's camera, moved, handed to it in place of its own (the
// game's own transforms stay as they were), and the view's field of view after.
void fill_detour(std::uintptr_t description, std::uintptr_t id, std::uintptr_t settings, std::uintptr_t camera_view, std::uintptr_t beside,
                 std::uintptr_t view) {
    auto& s = state();
    Transform beside_copy, view_copy;
    bool copied = false;
    moved_description = 0;
    std::uint32_t player{1};
    if (description && camera_view && memory::peek(camera_view + views::camera_view_player, player) && player == 0) {
        LastError error;
        if (const auto offset = holding(); offset && memory::peek(beside, beside_copy) && memory::peek(view, view_copy) &&
                                           camera_like(view_copy.world) && camera_like(view_copy.before_offset)) {
            // The transform kept beside the view is the view's own when it is the same (then it moves with it).
            const bool beside_same = std::memcmp(&beside_copy, &view_copy, sizeof(view_copy)) == 0;
            view_copy.world = moved(view_copy.world, *offset);
            view_copy.before_offset = moved(view_copy.before_offset, *offset);
            if (beside_same) beside_copy = view_copy;
            copied = true;
            moved_description = description;
            moved_world = view_copy.world;
            moved_offset = *offset;
        }
    }
    s.fill.load(std::memory_order_acquire)(description, id, settings, camera_view, copied ? reinterpret_cast<std::uintptr_t>(&beside_copy) : beside,
                                           copied ? reinterpret_cast<std::uintptr_t>(&view_copy) : view);
    if (copied) {
        LastError error;
        moved_fov = give_fov(description, moved_offset);
    }
}
// The replay camera's say, right after: should it have set the view anew (the replay editor, and its exports), that is the
// picture's camera now, and it is given the offset in turn; so is its field of view, should it have set that anew too.
void replay_detour(std::uintptr_t description, std::uintptr_t context) {
    auto& s = state();
    s.replay_original.load(std::memory_order_acquire)(description, context);
    if (!description || description != moved_description) return;
    moved_description = 0;
    LastError error;
    Transform now{};
    if (!memory::peek(description + views::description_transform, now)) return;
    if (!same_camera(now.world, moved_world) && camera_like(now.world) && camera_like(now.before_offset)) {
        if (const auto setter = s.set_transform.load(std::memory_order_acquire)) {
            Transform moved_now = now;
            moved_now.world = moved(now.world, moved_offset);
            moved_now.before_offset = moved(now.before_offset, moved_offset);
            setter(description, &moved_now);
        }
    }
    float fov{};
    if (moved_fov > 0 && memory::peek(description + views::description_fov, fov) && std::abs(fov - moved_fov) > 1e-6f)
        (void)give_fov(description, moved_offset);
}

// Checks the functions' bytes and prepares the detours (patching nothing). Under `hooks`.
bool prepare(State& s) noexcept {
    if (s.prepared) return true;
    if (s.failed) return false;
    s.failed = true; // until prepared
    const auto base = plugin_camera::image_base().load(std::memory_order_relaxed);
    if (!base) return false;
    for (const auto* f : {&views::fill_view, &views::replay_view, &views::set_transform, &views::set_fov})
        if (!matches(base, *f)) {
            logging::log(logging::Level::warning, logging::Channel::graphics,
                         "Camera offset: the game's building of its views is not the one known at 0x{:x}; plugins' camera offsets are off.", f->rva);
            return false;
        }
    auto* const fill_target = reinterpret_cast<void*>(base + views::fill_view.rva);
    auto* const replay_target = reinterpret_cast<void*>(base + views::replay_view.rva);
    void* fill_relay{};
    void* replay_relay{};
    auto status = hook_prepare(fill_target, reinterpret_cast<void*>(&fill_detour), &fill_relay);
    if (status == HookOk) {
        status = hook_prepare(replay_target, reinterpret_cast<void*>(&replay_detour), &replay_relay);
        if (status != HookOk) (void)hook_remove(fill_target);
    }
    if (status != HookOk || !fill_relay || !replay_relay) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Camera offset: the hooks on the game's view building could not be made ({}).",
                     hook_status_string(status));
        return false;
    }
    // Published before the detours can ever be enabled.
    s.fill.store(reinterpret_cast<Fill>(fill_relay), std::memory_order_release);
    s.replay_original.store(reinterpret_cast<ReplayView>(replay_relay), std::memory_order_release);
    s.set_transform.store(reinterpret_cast<SetTransform>(base + views::set_transform.rva), std::memory_order_release);
    s.set_fov.store(reinterpret_cast<SetFov>(base + views::set_fov.rva), std::memory_order_release);
    s.fill_target = fill_target;
    s.replay_target = replay_target;
    s.prepared = true;
    s.failed = false;
    return true;
}
// Both detours on or off (the replay camera's first, so the fill's offset always has its say after it). Under `hooks`.
void switch_hooks(State& s, bool on) noexcept {
    if (!s.prepared || s.enabled == on) return;
    HookBatch batch;
    const auto first = on ? s.replay_target : s.fill_target, second = on ? s.fill_target : s.replay_target;
    const auto a = on ? hook_enable(first) : hook_disable(first);
    const auto b = on ? hook_enable(second) : hook_disable(second);
    if (a != HookOk || b != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Camera offset: the hooks could not be switched {} ({}, {}).", on ? "on" : "off",
                     hook_status_string(a), hook_status_string(b));
        if (on) {
            (void)hook_disable(s.fill_target);
            (void)hook_disable(s.replay_target);
        }
        return;
    }
    s.enabled = on;
}
} // namespace

Matrix moved(const Matrix& camera, const Offset& offset) noexcept {
    for (const auto v : offset.turn)
        if (!std::isfinite(v)) return camera;
    for (const auto v : offset.move)
        if (!std::isfinite(v)) return camera;
    Vec right = row(camera, 0), up = row(camera, 1), back = row(camera, 2);
    const auto position = add(add(add(row(camera, 3), scale(right, offset.move[0])), scale(up, offset.move[1])), scale(back, -offset.move[2]));
    // Roll about the back axis (clockwise: the right side down), then pitch about the rolled right (up: up turns towards
    // back), then yaw about the pitched up (right: back turns away from right). Right-handed: right x up = back.
    turn(right, up, -offset.turn[2]);
    turn(up, back, offset.turn[1]);
    turn(back, right, -offset.turn[0]);
    // Kept exactly orthonormal.
    back = normal(back);
    right = normal(add(right, scale(back, -dot(right, back))));
    up = cross(back, right);
    Matrix result = camera;
    for (std::size_t i = 0; i < 3; ++i) {
        result[i] = right[i];
        result[4 + i] = up[i];
        result[8 + i] = back[i];
        result[12 + i] = position[i];
    }
    return result;
}

float field_of_view(float fov, const Offset& offset) noexcept {
    float result = offset.fov > 0 && std::isfinite(offset.fov) ? offset.fov * std::numbers::pi_v<float> / 180.0f : fov;
    if (std::isfinite(offset.zoom) && offset.zoom > 0) result *= offset.zoom;
    return std::isfinite(result) ? std::clamp(result, 0.02f, 3.0f) : fov;
}

std::string set(std::string_view owner, std::optional<Offset> offset) noexcept {
    try {
        auto& s = state();
        if (!offset) {
            clear(owner);
            return {};
        }
        {
            std::lock_guard lock(s.hooks);
            if (!prepare(s)) return "the game's camera cannot be offset here (this build's view building is not the one known)";
        }
        std::lock_guard lock(s.mutex);
        const auto now = GetTickCount64();
        if (!s.owner.empty() && s.owner != owner && now <= s.until) return "another plugin's camera offset holds";
        s.owner = std::string(owner);
        s.offset = *offset;
        s.until = now + holds_ms;
        return {};
    } catch (...) {
        return "it could not be taken";
    }
}

void clear(std::string_view owner) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!owner.empty() && s.owner != owner) return;
    s.owner.clear();
    s.until = 0;
}

void tick() noexcept {
    auto& s = state();
    const bool held = holding().has_value();
    std::lock_guard lock(s.hooks);
    if (!s.prepared) return;
    const auto now = GetTickCount64();
    if (held) {
        s.idle_since = 0;
        switch_hooks(s, true);
    } else if (s.enabled) {
        if (!s.idle_since) s.idle_since = now;
        else if (now - s.idle_since > unhook_after_ms) switch_hooks(s, false);
    }
}

bool available() noexcept {
    auto& s = state();
    std::lock_guard lock(s.hooks);
    return s.prepared;
}
} // namespace dingosdk::view_offset
