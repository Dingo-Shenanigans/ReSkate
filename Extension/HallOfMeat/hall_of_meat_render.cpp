#include "hall_of_meat_render.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_render.h"
#include "Engine/Game/Rendering/draw_packet.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>

namespace dingosdk::hall_of_meat {
namespace {
namespace build = addr::skater_render;
using DrawPacket = void (*)(std::uintptr_t object, std::uintptr_t packet);
using RenderView = void (*)(std::uintptr_t blackboard, std::uintptr_t current, std::uintptr_t previous, std::uint8_t jitter);
constexpr std::uint32_t no_index = UINT32_MAX;
constexpr std::uint32_t most_bones = 1024;
constexpr std::uint64_t lease_ms = 500;  // the client tick's indices, while ticks come and a bail shows
constexpr std::uint64_t fresh_ms = 250;  // a picture's parts, while the renderer draws
constexpr float camera_reach = 10.0f;    // metres from the client's camera: the main view's, not a mirror's

struct State {
    std::atomic<bool> ready{}; // the hooks prepared
    bool hooked{};             // and enabled (hook_render): client thread only
    std::uintptr_t base{};
    DrawPacket draw_original{};
    RenderView view_original{};
    std::array<std::atomic<std::uint32_t>, build::render_modes> indices{no_index, no_index};
    std::atomic<std::uint64_t> indices_until{};
    std::atomic<bool> found_logged{};
    std::mutex lock; // guards the picture's parts
    std::vector<game::LinearTransform> skin;
    std::uint64_t skin_at{};
    std::array<float, 16> camera{};
    float vertical_fov{};
    std::uint64_t camera_at{};
};
State& state() { static auto* value = new State; return *value; }

// A hook must not leave the game a different last error.
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
std::uintptr_t pointer_at(std::uintptr_t address) noexcept {
    std::uintptr_t value{};
    return memory::peek(address, value) ? value : 0;
}

// The record a handle names in a manager's paged pool.
std::uintptr_t record_of(std::uintptr_t manager, const build::RenderManager& kind, std::uint32_t handle) noexcept {
    if (handle < build::first_handle || handle > 1'000'000) return 0;
    const std::uint64_t slot = std::uint64_t{handle - build::first_handle} + 16;
    const auto high = std::bit_width(slot) - 1; // 4 and up
    const auto page = pointer_at(manager + build::manager_pool_offset + 8 + 8 * (high - 4));
    return page ? page + static_cast<std::uintptr_t>(slot - (std::uint64_t{1} << high)) * kind.record_size : 0;
}
// The render index of the entity's render object for one mode, through the managers' handles.
std::uint32_t render_index(std::uintptr_t base, std::uintptr_t entity, std::size_t mode) noexcept {
    std::uint32_t handle{};
    if (!memory::peek(entity + build::entity_render_handles_offset + mode * build::entity_render_handle_stride, handle))
        return no_index;
    for (const auto& kind : build::handle_chain) {
        const auto manager = pointer_at(base + kind.global);
        if (!manager || pointer_at(manager) != base + kind.vtable) return no_index;
        const auto record = record_of(manager, kind, handle);
        if (!record || !memory::peek(record + kind.handle_offset, handle)) return no_index;
    }
    return handle >= build::first_handle ? handle - build::first_handle : no_index;
}

// Render threads, after each skinned draw packet: the local skater's visible one, as skinning
// matrices in the world.
void observe_packet(std::uintptr_t object, std::uintptr_t packet) {
    auto& s = state();
    std::uint32_t index{}, count{}, locked{};
    std::uint8_t visible{}, relative{};
    if (GetTickCount64() > s.indices_until.load(std::memory_order_acquire) ||
        pointer_at(object) != s.base + build::render_object_vtable || !memory::peek(object + build::render_index_offset, index) ||
        (index != s.indices[0].load(std::memory_order_acquire) && index != s.indices[1].load(std::memory_order_acquire)))
        return;
    if (!memory::peek(packet + build::packet_visible_offset, visible) || !visible ||
        !memory::peek(packet + build::packet_bone_count_offset, count) || !count || count > most_bones ||
        !memory::peek(packet + build::packet_relative_offset, relative) ||
        !memory::peek(object + build::render_object_lock_offset, locked) || locked)
        return;
    const auto bones = pointer_at(packet + build::packet_bones_offset), root_at = pointer_at(packet + build::packet_root_offset);
    std::vector<draw_packet::PackedBone> packed(count);
    std::array<float, 4> root{};
    game::LinearTransform actor{};
    if (!bones || !memory::peek_bytes(bones, packed.data(), packed.size() * sizeof(draw_packet::PackedBone)) ||
        (root_at && !memory::peek(root_at, root)) || !memory::peek(packet + build::packet_actor_offset, actor))
        return;
    const auto placed = draw_packet::placement(root, relative != 0, actor);
    std::vector<game::LinearTransform> skin(count);
    for (std::size_t bone = 0; bone < count; ++bone)
        if (!draw_packet::skin(packed[bone], placed, skin[bone])) return;
    {
        std::lock_guard guard(s.lock);
        s.skin = std::move(skin);
        s.skin_at = GetTickCount64();
    }
    if (!s.found_logged.exchange(true))
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Hall of Meat: the renderer draws the local skater (render object {}, {} bones).", index, count);
}
void draw_packet_hook(std::uintptr_t object, std::uintptr_t packet) {
    auto& s = state();
    s.draw_original(object, packet);
    LastError error;
    try {
        observe_packet(object, packet);
    } catch (...) { /* A lost picture is never worth the renderer. */ }
}

// The camera of the main picture: a sound world matrix, close to the client's own camera.
void observe_view(std::uintptr_t view) {
    auto& s = state();
    if (GetTickCount64() > s.indices_until.load(std::memory_order_acquire)) return;
    std::uint32_t kind{};
    std::array<std::byte, build::view_size> data{};
    if (!memory::peek(view - build::view_kind_back, kind) || kind != build::main_view_kind ||
        !memory::peek_bytes(view, data.data(), data.size()))
        return;
    std::array<float, 16> camera{};
    float radians{};
    std::memcpy(camera.data(), data.data() + build::view_camera_offset, sizeof(camera));
    std::memcpy(&radians, data.data() + build::view_fov_offset, sizeof(radians));
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(camera[row * 4 + axis]) || std::abs(camera[row * 4 + axis]) > 1e7f) return;
        camera[row * 4 + 3] = row == 3 ? 1.0f : 0.0f;
    }
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t other = row; other < 3; ++other) {
            float dot{};
            for (std::size_t axis = 0; axis < 3; ++axis) dot += camera[row * 4 + axis] * camera[other * 4 + axis];
            if (std::abs(dot - (row == other ? 1.0f : 0.0f)) > 0.05f) return;
        }
    const float fov = radians * 180.0f / std::numbers::pi_v<float>;
    const auto client = latest_game_view();
    if (!std::isfinite(fov) || fov <= 1 || fov >= 175 || !client) return;
    float distance{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float delta = camera[12 + axis] - client->world[12 + axis];
        distance += delta * delta;
    }
    if (!(distance <= camera_reach * camera_reach)) return;
    std::lock_guard guard(s.lock);
    s.camera = camera;
    s.vertical_fov = fov;
    s.camera_at = GetTickCount64();
}
void render_view_hook(std::uintptr_t blackboard, std::uintptr_t current, std::uintptr_t previous, std::uint8_t jitter) {
    auto& s = state();
    s.view_original(blackboard, current, previous, jitter);
    LastError error;
    try {
        observe_view(current);
    } catch (...) { /* A lost camera is never worth the renderer. */ }
}
std::array<void*, 2> render_targets(std::uintptr_t base) noexcept {
    return {reinterpret_cast<void*>(base + build::draw_packet_contract.rva),
        reinterpret_cast<void*>(base + build::render_view_contract.rva)};
}
}

bool start_render(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return s.base == base;
    for (const auto& contract : build::contracts) {
        std::array<unsigned char, 32> actual{};
        if (!memory::peek(base + contract.rva, actual) || actual != contract.bytes) {
            logging::log(logging::Level::warning, logging::Channel::graphics,
                "Hall of Meat cannot see where the skater is drawn: the native contract at 0x{:x} did not match.", contract.rva);
            return false;
        }
    }
    s.base = base;
    const auto targets = render_targets(base);
    const std::array replacements{reinterpret_cast<void*>(&draw_packet_hook), reinterpret_cast<void*>(&render_view_hook)};
    std::array<void*, 2> originals{};
    auto status = HookOk;
    std::size_t prepared{};
    for (; prepared < targets.size(); ++prepared) {
        status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
        if (status != HookOk) break;
        if (!originals[prepared]) {
            ++prepared;
            status = HookUnsupportedFunction;
            break;
        }
    }
    if (status == HookOk) {
        // Both relays are published before either target is enabled (hook_render).
        s.draw_original = reinterpret_cast<DrawPacket>(originals[0]);
        s.view_original = reinterpret_cast<RenderView>(originals[1]);
        s.ready.store(true, std::memory_order_release);
        return true;
    }
    logging::log(logging::Level::warning, logging::Channel::graphics,
        "Hall of Meat cannot see where the skater is drawn: hooking the renderer failed (status {}).", static_cast<LONG>(status));
    while (prepared) (void)hook_remove(targets[--prepared]);
    return false;
}

bool hook_render(bool on) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return !on; // nothing to hook
    if (s.hooked == on) return true;
    const auto targets = render_targets(s.base);
    auto status = HookOk;
    std::size_t changed{};
    for (; changed < targets.size(); ++changed) {
        status = on ? hook_enable(targets[changed]) : hook_disable(targets[changed]);
        if (status != HookOk) break;
    }
    if (status == HookOk) {
        s.hooked = on;
        return true;
    }
    logging::log(logging::Level::warning, logging::Channel::graphics, "Hall of Meat could not {} its renderer hooks (status {}).",
        on ? "enable" : "disable", static_cast<LONG>(status));
    // Both or neither.
    while (changed) {
        const auto target = targets[--changed];
        (void)(on ? hook_disable(target) : hook_enable(target));
    }
    return false;
}

void follow_render(bool wanted) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    NoBailSkater skater;
    if (!wanted || !no_bail_skater(skater)) {
        s.indices_until.store(0, std::memory_order_release);
        return;
    }
    for (std::size_t mode = 0; mode < build::render_modes; ++mode)
        s.indices[mode].store(render_index(s.base, skater.entity, mode), std::memory_order_release);
    s.indices_until.store(GetTickCount64() + lease_ms, std::memory_order_release);
}

bool latest_picture(Picture& picture) noexcept {
    auto& s = state();
    const auto now = GetTickCount64();
    std::lock_guard guard(s.lock);
    if (!s.skin_at || !s.camera_at || now - s.skin_at > fresh_ms || now - s.camera_at > fresh_ms) return false;
    try {
        picture.skin = s.skin;
    } catch (...) {
        return false;
    }
    picture.camera = s.camera;
    picture.vertical_fov = s.vertical_fov;
    return true;
}
}
