#pragma once
#include "native_skater.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/native_skater.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

// Remote actor state shared by native_skater.cpp and native_skater_spawn.cpp.
namespace dingosdk::multiplayer::native_skater_detail {
namespace entities = addr::skater_entities;
namespace native = addr::native_skater;
struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require(bool value, const char *reason);
bool readable(std::uintptr_t address, void *out, std::size_t size);
template <class T>
T read(std::uintptr_t object, std::uintptr_t offset = 0,
       const std::source_location &location = std::source_location::current()) {
    T value{};
    if (object < 0x10000 || object > memory::highest_user_address ||
        offset > memory::highest_user_address - object ||
        !readable(object + offset, &value, sizeof(value)))
        throw Unavailable(
            std::format("Native skater read failed: object={:#x}, offset={:#x}, bytes={}, line={}.", object,
                        offset, sizeof(value), location.line()));
    return value;
}
std::uintptr_t ptr(std::uintptr_t object, std::uintptr_t offset = 0,
                   const std::source_location &location = std::source_location::current());
bool write(std::uintptr_t address, const void *data, std::size_t size);
struct alignas(16) NativeBone {
    std::array<float, 4> scale, rotation, position;
};
static_assert(sizeof(NativeBone) == 0x30);
using AnimationUpdate = void (*)(std::uintptr_t, std::uintptr_t);
using DestroyEntity = void (*)(std::uintptr_t, std::uintptr_t);
using RenderPose = bool (*)(std::uintptr_t, std::uintptr_t);
struct HookState {
    std::uintptr_t base{};
    DWORD engine_thread{};
    bool hooks{}, install_attempted{};
    AnimationUpdate original_animation{};
    DestroyEntity original_destroy{};
    RenderPose original_render_pose{};
    std::atomic<EntityDestroyed> destroyed_listener{};
    std::atomic<AnimationEvaluated> evaluated_listener{};
    std::atomic<LocalPoseFilter> pose_filter{};
    std::atomic<RenderPosePublished> render_listener{}, style_render_listener{};
};
// Keys the per-entity hooks match, one contiguous array per kind in slot order.
// Every native skater, board and entity callback looks its key up here: a few
// cache lines up to the highest slot ever watched, not one Remote per slot.
using WatchedArray = std::array<std::atomic<std::uintptr_t>, max_remote_players>;
struct WatchedKeys {
    WatchedArray component{}, entity{}, board{}, board_holder{};
    // One past the highest slot that has stored a key. Only grows, and grows
    // before the key is stored, so a lookup never stops short of a watched key.
    std::atomic<std::size_t> bound{};
};
WatchedKeys &watched();
// Stores key for the current peer slot (0 stops watching).
void watch(WatchedArray &keys, std::uintptr_t key) noexcept;
// The slot whose key equals key, or max_remote_players.
std::size_t watching(const WatchedArray &keys, std::uintptr_t key) noexcept;
struct Remote {
    std::uintptr_t entity{}, component{}, parent{}, context{}, local_entity{};
    std::uintptr_t board_entity{}, board_component{}, board_parent{};
    bool failed{};
    std::mutex mutex;
    Pose target;
    // Bumped whenever target is replaced. The skeleton buffer last written and the
    // target revision it holds let an animation callback that did not re-evaluate
    // the native graph skip writing the same pose again.
    std::uint64_t target_revision{}, written_revision{};
    std::uintptr_t written_buffer{};
    // Bumped whenever this slot's actor is created, removed or destroyed, so state
    // cached for one actor (its sound binding) is never used for the next.
    std::atomic<std::uint64_t> generation{};
    // Frozen with entity placement, independent of newer network snapshots.
    std::vector<Transform> board_render_pose;
    std::atomic<bool> board_ready{};
    std::atomic<std::uint64_t> applied{};
    std::atomic<std::uint64_t> board_applied{};
    std::atomic<bool> pose_driven{};
    std::atomic<std::uint64_t> native_evaluated{}, native_skipped{}, native_evaluation_us{}, apply_us{};
    ULONGLONG next_native_animation{}, next_safety_audit{}, next_entity_audit{},
        next_board_safety_audit{}, next_board_status{}, next_status{};
    // A skateboard that went away is not created again before this: one creation is a heavy
    // native pass, and a player's board may come and go with every pose.
    ULONGLONG next_board_create{};
    bool board_failed{};
    std::string board_issue;
    std::string board_status;
    std::string issue;
    std::optional<CosmeticRecipe> skater_appearance, board_appearance;
    // The last appearance the native copy or catalog check rejected, retried at
    // next_appearance_retry instead of on every cosmetics pass. Client thread.
    std::optional<Appearance> rejected_appearance;
    ULONGLONG next_appearance_retry{};
};
HookState &shared();
Remote &remote();
void install(std::uintptr_t base);
void place_actor(std::uintptr_t base, std::uintptr_t entity, const Transform &root);
void place_board(std::uintptr_t base, std::uintptr_t entity, const Transform &root);
} // namespace dingosdk::multiplayer::native_skater_detail
