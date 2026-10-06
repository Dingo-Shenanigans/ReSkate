#include "native_skater_internal.h"
#include "native_pose_layout.h"
#include "board_debris.h"
#include "puppet_cost.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>
#include <atomic>
#include <mutex>

namespace dingosdk::multiplayer {
using namespace native_skater_detail;
namespace native_skater_detail {
void require(bool value, const char *reason) {
    if (!value)
        throw Unavailable(reason);
}
bool readable(std::uintptr_t address, void *out, std::size_t size) {
    if (address < 0x10000 || size > memory::highest_user_address ||
        address > memory::highest_user_address - size)
        return false;
    // These are same-process animation buffers. Avoid a kernel transition for
    // every field and 19 KiB pose transfer while retaining an access-violation
    // boundary for streamed/destroyed reverse-engineered objects.
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::uintptr_t ptr(std::uintptr_t object, std::uintptr_t offset,
                   const std::source_location &location) {
    return read<std::uintptr_t>(object, offset, location);
}
bool write(std::uintptr_t address, const void *data, std::size_t size) {
    if (address < 0x10000 || size > memory::highest_user_address ||
        address > memory::highest_user_address - size)
        return false;
    __try {
        std::memcpy(reinterpret_cast<void *>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
HookState &shared() {
    static auto *s = new HookState;
    return *s;
}
Remote &remote() {
    static auto *r = new PeerStorage<Remote>;
    return r->current();
}
WatchedKeys &watched() {
    static auto *w = new WatchedKeys;
    return *w;
}
void watch(WatchedArray &keys, std::uintptr_t key) noexcept {
    if (key) {
        auto &bound = watched().bound;
        const auto needed = peer_slot + 1;
        for (auto current = bound.load(std::memory_order_acquire);
             current < needed && !bound.compare_exchange_weak(current, needed, std::memory_order_acq_rel);) {
        }
    }
    keys[peer_slot].store(key, std::memory_order_release);
}
std::size_t watching(const WatchedArray &keys, std::uintptr_t key) noexcept {
    if (!key)
        return max_remote_players;
    const auto bound = std::min(watched().bound.load(std::memory_order_acquire), max_remote_players);
    for (std::size_t slot = 0; slot < bound; ++slot)
        if (keys[slot].load(std::memory_order_acquire) == key)
            return slot;
    return max_remote_players;
}
void place_actor(std::uintptr_t base, std::uintptr_t entity, const Transform &root) {
    alignas(16) const auto matrix = to_matrix(root);
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + entities::place_entity)(entity, matrix.data());
}
void place_board(std::uintptr_t base, std::uintptr_t entity, const Transform &root) {
    auto &r = remote();
    const auto holder = ptr(entity, 0xf0);
    require(holder && ptr(holder) == base + addr::engine::board_holder_vtable && ptr(holder, 0xb8),
            "Skateboard animation transform controller unavailable.");
    const auto parent = ptr(entity, 0x40);
    require(entity == r.board_entity && parent == r.board_parent && parent != r.parent &&
                ptr(holder, 0x30) == parent && ptr(parent, 0x20) == r.context,
            "Skateboard blueprint transform ownership changed.");
    const auto transform = ptr(parent, 0x18);
    require(transform && ptr(transform) == base + native::blueprint_transform_vtable &&
                transform != ptr(r.parent, 0x18) && !read<std::uint8_t>(transform, 0xa5),
            "Skateboard blueprint world transform unavailable.");
    alignas(16) const auto matrix = to_matrix(root);
    // Match ClientSkateboardEntity's world-transform event (native::board_world_transform_event):
    // update its blueprint transform BEFORE notifying entity placement.
    // The placement callback (native::board_placement_callback) publishes inverse(parentWorld) *
    // entityWorld to the renderer. An animation-driven/stale parent gives
    // that render transform a second board-origin translation even though
    // the copied world-space bones are correct. No bone/position adjustment.
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + native::set_blueprint_transform)(
        transform, matrix.data());
    // Native source setter places the entity and then updates the rig input.
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + native::place_board)(entity, matrix.data());
}
} // namespace native_skater_detail
namespace {
template <std::size_t N>
void prefix(std::uintptr_t base, std::uintptr_t rva, const std::array<std::uint8_t, N> &expected) {
    std::array<std::uint8_t, 32> bytes{};
    require(expected.size() <= bytes.size() && readable(base + rva, bytes.data(), expected.size()) &&
                std::equal(expected.begin(), expected.end(), bytes.begin()),
            "Multiplayer native function fingerprint changed.");
}
std::array<float, 16> root_matrix(std::uintptr_t entity) {
    const auto collection = ptr(entity, 0x70);
    require(ptr(collection) == entity, "Skater transform owner differs.");
    const auto first = read<std::uint8_t>(collection, 9), extra = read<std::uint8_t>(collection, 10);
    require(first <= 128 && extra <= 32, "Skater transform layout exceeds bounds.");
    return read<std::array<float, 16>>(collection, 0x10 + (std::uintptr_t{first} + 2 * extra) * 0x20);
}
NativePoseLayout skeleton(std::uintptr_t base, std::uintptr_t holder, std::size_t bound) {
    return read_native_pose_layout(readable, base, holder, bound);
}
std::vector<Transform> capture_skeleton(std::uintptr_t base, std::uintptr_t holder, std::size_t bound) {
    const auto s = skeleton(base, holder, bound);
    if (!s.buffer)
        return {};
    std::vector<NativeBone> bones(s.count);
    require(readable(s.buffer, bones.data(), bones.size() * sizeof(NativeBone)),
            "Animation pose not readable.");
    std::vector<Transform> result;
    result.reserve(s.count);
    for (const auto &bone : bones) {
        Transform t{{bone.position[0], bone.position[1], bone.position[2]},
                    bone.rotation,
                    {bone.scale[0], bone.scale[1], bone.scale[2]}};
        require(valid_transform(t), "Animation pose is not ready.");
        result.push_back(t);
    }
    return result;
}
// Stores each received bone into the native pose in place: scale.xyz, rotation and
// position.xyz. The destination's w lanes are left alone (native board poses can
// carry non-float metadata in position.w), so the buffer is never read back first.
// Packet decode and playback interpolation have already normalized the quaternion.
bool write_bones(std::uintptr_t address, const Transform *pose, std::size_t count) noexcept {
    if (address < 0x10000 || count > (memory::highest_user_address - address) / sizeof(NativeBone))
        return false;
    __try {
        auto *bone = reinterpret_cast<std::uint8_t *>(address);
        for (std::size_t i = 0; i < count; ++i, bone += sizeof(NativeBone)) {
            std::memcpy(bone + offsetof(NativeBone, scale), pose[i].scale.data(), sizeof(pose[i].scale));
            std::memcpy(bone + offsetof(NativeBone, rotation), pose[i].rotation.data(), sizeof(pose[i].rotation));
            std::memcpy(bone + offsetof(NativeBone, position), pose[i].position.data(), sizeof(pose[i].position));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Returns the buffer that holds the pose now, or 0 while resources are streaming.
// `unchanged` is a buffer already holding exactly this pose: when the skeleton still
// uses it, nothing is written.
std::uintptr_t apply_skeleton(std::uintptr_t base, std::uintptr_t holder, std::span<const Transform> pose,
                              std::size_t bound, std::uintptr_t unchanged = 0) {
    if (pose.empty())
        return 0;
    const auto s = skeleton(base, holder, bound);
    if (!s.buffer)
        return 0; // Mesh/animation resources may still be streaming.
    require(s.count == pose.size(), "Remote skeleton does not match the received pose.");
    if (s.buffer != unchanged)
        require(write_bones(s.buffer, pose.data(), pose.size()), "Remote animation pose write failed.");
    return s.buffer;
}
struct HiddenJoint {
    std::mutex mutex;
    std::uintptr_t component{};
    std::uint16_t joint{};
    std::array<float, 3> scale{1, 1, 1};
};
HiddenJoint &hidden_joint() {
    static auto *value = new HiddenJoint;
    return *value;
}
// ---- Board-break pieces: a bone pose per piece, written when its skeleton is published. ----
struct PiecePose {
    std::uintptr_t holder{};
    std::vector<Transform> bones;
    PieceStatus status;
};
struct PieceRegistry {
    std::mutex mutex;
    std::vector<PiecePose> pieces;
    std::atomic<std::size_t> count{};
};
PieceRegistry &piece_registry() {
    static auto *value = new PieceRegistry;
    return *value;
}
// Bones 0 and 1 (the rig root and the board's world placement) stay with the engine; the deck,
// the trucks and the wheels (2..11) come from the piece.
constexpr std::size_t piece_first_bone = 2, piece_last_bone = 12;
void apply_piece_pose(std::uintptr_t animation_interface, std::uintptr_t render_data) {
    auto &registry = piece_registry();
    if (!registry.count.load(std::memory_order_acquire))
        return;
    const auto error = GetLastError();
    std::unique_lock lock(registry.mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        const auto holder = animation_interface - 0xc0;
        for (auto &piece : registry.pieces) {
            if (piece.holder != holder)
                continue;
            auto &status = piece.status;
            ++status.calls;
            try {
                const auto pose = skeleton(shared().base, holder, max_board_bones - 1);
                const auto published_buffer = ptr(render_data, 0x10);
                status.count = pose.count;
                status.same_buffer = pose.buffer && published_buffer == pose.buffer;
                if (!pose.buffer)
                    ++status.no_layout;
                else if (!status.same_buffer)
                    ++status.wrong_buffer;
                else if (pose.count != piece.bones.size() || pose.count < piece_last_bone)
                    ++status.wrong_count;
                else if (write_bones(pose.buffer + piece_first_bone * sizeof(NativeBone),
                                     piece.bones.data() + piece_first_bone, piece_last_bone - piece_first_bone)) {
                    ++status.written;
                    NativeBone check{};
                    if (readable(pose.buffer + 3 * sizeof(NativeBone), &check, sizeof(check)))
                        status.deck = {check.position[0], check.position[1], check.position[2]};
                }
            } catch (...) {
                ++status.failed;
            }
            break;
        }
    }
    SetLastError(error);
}
void apply_render_pose(std::uintptr_t animation_interface, std::uintptr_t render_data) {
    auto &r = remote();
    const auto holder = watched().board_holder[peer_slot].load(std::memory_order_acquire);
    if (!holder || animation_interface != holder + 0xc0)
        return;
    const auto error = GetLastError();
    // Placement can cause nested engine notifications while animation_hook
    // owns the lock. The normal render publication will apply the frozen pose.
    std::unique_lock lock(r.mutex, std::try_to_lock);
    if (lock.owns_lock() && !r.failed && !r.board_failed && r.board_ready.load() &&
        holder == watched().board_holder[peer_slot].load() && !r.board_render_pose.empty()) {
        try {
            const auto now = GetTickCount64();
            if (now >= r.next_board_safety_audit) {
                require(r.board_entity && watched().board[peer_slot].load() == r.board_entity &&
                            ptr(r.board_entity, 0xf0) == holder &&
                            ptr(holder) == shared().base + addr::engine::board_holder_vtable &&
                            ptr(ptr(r.board_component, 0x18)) == r.board_entity &&
                            !ptr(r.board_component, 0x40) && !ptr(r.board_component, 0x58) &&
                            read<std::uint8_t>(r.board_component, 0x7d) == 1,
                        "Skateboard render pose ownership changed.");
                r.next_board_safety_audit = now + 1000;
            }
            const auto pose = skeleton(shared().base, holder, max_board_bones - 1);
            const auto published_buffer = ptr(render_data, 0x10);
            if (pose.buffer && published_buffer) {
                // addr::engine::board_render_pose publishes rig+0xd0 at RenderPoseData+0x10.
                // Verify that this is the buffer we are about to supply.
                require(published_buffer == pose.buffer,
                        "Skateboard renderer uses a different animation buffer.");
                // The board rig re-evaluates every frame: always written.
                if (apply_skeleton(shared().base, holder, r.board_render_pose, max_board_bones - 1)) {
                    if (++r.board_applied == 1)
                        logging::log(logging::Level::info, logging::Channel::runtime,
                                     "Multiplayer: skateboard pose published to renderer; holder={:#x}, "
                                     "buffer={:#x}, bones={}.",
                                     holder, pose.buffer, pose.count);
                }
            }
        } catch (const std::exception &e) {
            r.board_failed = true;
            r.board_issue = e.what();
        }
    }
    SetLastError(error);
}
// `evaluated`: the native graph ran in this callback and wrote its own pose.
void apply_animation(std::uintptr_t component, bool evaluated) {
    auto &r = remote();
    if (component != watched().component[peer_slot].load(std::memory_order_acquire))
        return;
    const auto error = GetLastError();
    try {
        std::lock_guard lock(r.mutex);
        if (r.failed || component != r.component)
            return;
        const auto now = GetTickCount64();
        if (now >= r.next_safety_audit) {
            require(ptr(component) == shared().base + addr::engine::skater_component_vtable &&
                        ptr(ptr(component, 0x18)) == r.entity,
                    "Remote skater animation ownership changed.");
            // Retain a slow safety audit without repeating stable pointer walks
            // on every animation callback.
            require(!ptr(component, 0x70), "Remote skater unexpectedly acquired physics.");
            r.next_safety_audit = now + 1000;
        }
        // Apply placement and bones from the same buffered frame. Publishing
        // placement again after the client tick otherwise pairs a new board
        // rotation with the previous animation pose during fast tricks.
        if (!r.target.skater.empty())
            place_actor(shared().base, r.entity, r.target.root);
        // A target the session has not replaced since the last write (a far player
        // between samples) is still in the buffer, unless the native graph ran.
        const auto unchanged = !evaluated && r.written_revision == r.target_revision ? r.written_buffer : 0;
        // 0 while resources stream: the next buffer is written whatever its address.
        r.written_buffer = apply_skeleton(shared().base, ptr(component, 0xa0), r.target.skater,
                                          max_skater_bones, unchanged);
        if (r.written_buffer) {
            ++r.applied;
            r.written_revision = r.target_revision;
        }
        // The board is a separate visual entity, with its own 16-bone output.
        // Keep board failures separate so skater playback continues.
        if (r.target.board.size() <= 1)
            r.board_render_pose.clear();
        if (r.board_ready.load(std::memory_order_acquire) && !r.board_failed &&
            watched().board[peer_slot].load(std::memory_order_acquire) == r.board_entity && r.board_entity &&
            r.target.board.size() > 1) {
            try {
                if (now >= r.next_board_safety_audit) {
                    require(ptr(r.board_entity) == shared().base + addr::engine::board_entity_vtable &&
                                ptr(ptr(r.board_component, 0x18)) == r.board_entity &&
                                !ptr(r.board_component, 0x40) && !ptr(r.board_component, 0x58) &&
                                read<std::uint8_t>(r.board_component, 0x7d) == 1,
                            "Remote skateboard ownership or physics changed.");
                    r.next_board_safety_audit = now + 1000;
                }
                place_board(shared().base, r.board_entity, r.target.board.front());
                // The board rig evaluates separately from the skater rig.
                // Supply its bones at render publication, after evaluation.
                r.board_render_pose.assign(r.target.board.begin() + 1, r.target.board.end());
            } catch (const std::exception &e) {
                r.board_failed = true;
                r.board_issue = e.what();
            }
        }
    } catch (const std::exception &e) {
        std::lock_guard lock(r.mutex);
        r.failed = true;
        r.issue = e.what();
    }
    SetLastError(error);
}
void observe_destruction(std::uintptr_t entity) {
    auto &r = remote();
    if (entity == watched().board[peer_slot].load(std::memory_order_acquire)) {
        r.board_ready.store(false, std::memory_order_release);
        watch(watched().board_holder, 0);
        watch(watched().board, 0);
        puppet_cost::forget_board();
        std::lock_guard lock(r.mutex);
        r.board_entity = 0;
        r.board_component = 0;
        r.board_appearance.reset();
        r.board_render_pose.clear();
        r.board_status.clear();
        r.next_board_safety_audit = r.next_board_status = 0;
    }
    if (entity == watched().entity[peer_slot].load(std::memory_order_acquire)) {
        watch(watched().entity, 0);
        watch(watched().component, 0);
        r.generation.fetch_add(1, std::memory_order_acq_rel);
        puppet_cost::forget_skater();
        std::lock_guard lock(r.mutex);
        r.entity = 0;
        r.component = 0;
        r.skater_appearance.reset();
        r.rejected_appearance.reset();
        r.target = {};
        ++r.target_revision;
        r.written_buffer = 0;
        r.pose_driven.store(false, std::memory_order_release);
        r.next_native_animation = r.next_safety_audit = r.next_entity_audit = r.next_status = 0;
    }
}
bool render_pose_hook(std::uintptr_t animation_interface, std::uintptr_t render_data) {
    const bool result = shared().original_render_pose(animation_interface, render_data);
    if (const auto listener = shared().render_listener.load(std::memory_order_acquire); result && listener)
        listener(animation_interface);
    // A board holder's animation interface is holder + 0xc0.
    if (const auto slot = result && animation_interface > 0xc0
                              ? watching(watched().board_holder, animation_interface - 0xc0)
                              : max_remote_players;
        slot < max_remote_players) {
        const PeerScope scope(slot);
        apply_render_pose(animation_interface, render_data);
    } else if (result && animation_interface > 0xc0) {
        apply_piece_pose(animation_interface, render_data);
    }
    return result;
}
void animation_hook(std::uintptr_t component, std::uintptr_t update) {
    // The received skeleton replaces the complete native result. Keep the
    // remote graph alive at network rate for resource/lifecycle maintenance,
    // while publishing the network pose on every animation callback.
    if (const auto slot = watching(watched().component, component); slot < max_remote_players) {
        const PeerScope scope(slot);
        auto &r = remote();
        const auto now = GetTickCount64();
        const bool evaluate = !r.pose_driven.load(std::memory_order_acquire) || !r.applied.load() ||
                              now >= r.next_native_animation;
        if (evaluate) {
            const auto started = now_us();
            shared().original_animation(component, update);
            r.native_evaluation_us.fetch_add(now_us() - started, std::memory_order_relaxed);
            r.native_evaluated.fetch_add(1, std::memory_order_relaxed);
            // Each slot keeps its own phase of the 50 ms interval, so the remote
            // skaters' native evaluations spread over the frames of an interval
            // instead of all landing in the same one.
            constexpr ULONGLONG interval = 50;
            const auto phase = slot * 17 % interval;
            r.next_native_animation = (now + phase) / interval * interval + interval - phase;
        } else {
            r.native_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        const auto started = now_us();
        apply_animation(component, evaluate);
        r.apply_us.fetch_add(now_us() - started, std::memory_order_relaxed);
        return;
    }
    shared().original_animation(component, update);
    if (const auto listener = shared().evaluated_listener.load(std::memory_order_acquire))
        listener(component);
}
void destroy_hook(std::uintptr_t entity, std::uintptr_t owner) {
    // Other SDK-created actors share this hook; they must forget the entity
    // before the engine frees it.
    if (const auto listener = shared().destroyed_listener.load(std::memory_order_acquire))
        listener(entity);
    // The first slot watching it as either skater or board.
    if (const auto slot = std::min(watching(watched().entity, entity), watching(watched().board, entity));
        slot < max_remote_players) {
        const PeerScope scope(slot);
        observe_destruction(entity);
    }
    shared().original_destroy(entity, owner);
}
} // namespace
namespace native_skater_detail {
void install(std::uintptr_t base) {
    if (shared().hooks) {
        require(shared().base == base, "Native image changed.");
        return;
    }
    require(!shared().install_attempted,
            "Multiplayer hook installation failed; restart ReSkate before retrying.");
    prefix(base, entities::descriptor_init, entities::descriptor_init_prefix);
    prefix(base, entities::create_entity, entities::create_entity_prefix);
    prefix(base, entities::descriptor_destroy, entities::descriptor_destroy_prefix);
    prefix(base, entities::destroy_entity, entities::destroy_entity_prefix);
    prefix(base, native::animation_update, native::animation_update_prefix);
    prefix(base, entities::place_entity, entities::place_entity_prefix);
    prefix(base, native::set_blueprint_transform, native::set_blueprint_transform_prefix);
    prefix(base, native::place_board, native::place_board_prefix);
    prefix(base, native::place_board_rig_call, native::place_board_rig_call_prefix);
    prefix(base, entities::initialize_placement, entities::initialize_placement_prefix);
    prefix(base, entities::physics_c7_setter, entities::physics_c7_setter_prefix);
    prefix(base, entities::release_reference, entities::release_reference_prefix);
    prefix(base, entities::customization_component, entities::customization_component_prefix);
    prefix(base, addr::engine::set_customization_flag, entities::set_customization_flag_prefix);
    prefix(base, native::entity_helper, native::entity_helper_prefix);
    prefix(base, native::enable_board_resources, native::enable_board_resources_prefix);
    prefix(base, native::initialize_creation_list, native::initialize_creation_list_prefix);
    prefix(base, native::free_creation_page, native::free_creation_page_prefix);
    prefix(base, addr::engine::board_render_pose, native::board_render_pose_prefix);
    require(ptr(base + addr::engine::board_entity_vtable, 0x158) == base + entities::place_entity &&
                ptr(base + addr::engine::board_holder_vtable, 0x88) ==
                    base + native::board_holder_resource_methods[0] &&
                ptr(base + addr::engine::board_holder_vtable, 0xc8) ==
                    base + native::board_holder_resource_methods[1] &&
                ptr(base + addr::engine::board_holder_vtable, 0xf8) == base + native::enable_board_resources,
            "Skateboard animation resource lifecycle differs.");
    require(ptr(base + native::board_render_pose_interface_vtable, 0x10) == base + addr::engine::board_render_pose,
            "Skateboard render pose interface differs.");
    shared().install_attempted = true;
    shared().base = base;
    shared().engine_thread = GetCurrentThreadId();
    void *original{};
    auto *animation = reinterpret_cast<void *>(base + native::animation_update);
    auto *destruction = reinterpret_cast<void *>(base + entities::destroy_entity);
    auto *render_pose = reinterpret_cast<void *>(base + addr::engine::board_render_pose);
    require(hook_prepare(animation, reinterpret_cast<void *>(&animation_hook), &original) == HookOk,
            "Cannot prepare remote animation hook.");
    shared().original_animation = reinterpret_cast<AnimationUpdate>(original);
    if (hook_prepare(destruction, reinterpret_cast<void *>(&destroy_hook), &original) != HookOk) {
        hook_remove(animation);
        throw Unavailable("Cannot prepare remote cleanup hook.");
    }
    shared().original_destroy = reinterpret_cast<DestroyEntity>(original);
    if (hook_prepare(render_pose, reinterpret_cast<void *>(&render_pose_hook), &original) != HookOk) {
        hook_remove(destruction);
        hook_remove(animation);
        throw Unavailable("Cannot prepare skateboard render pose hook.");
    }
    shared().original_render_pose = reinterpret_cast<RenderPose>(original);
    // The far-player component update hooks (puppet_cost.cpp) join the same transaction. A
    // mismatch there only leaves that saving off; it never blocks the actor hooks.
    const auto savings = puppet_cost::prepare_hooks(base);
    const bool queued = hook_queue_enable(animation) == HookOk && hook_queue_enable(destruction) == HookOk &&
                        hook_queue_enable(render_pose) == HookOk;
    std::size_t saving_hooks = 0;
    for (std::size_t i = 0; queued && i < savings.count; ++i)
        saving_hooks += hook_queue_enable(savings.targets[i]) == HookOk;
    // One transaction (one suspension of every game thread) for all of them.
    if (!queued || hook_apply_queued() != HookOk) {
        // Preserve forwarding trampolines on uncertain enable results. No
        // remote actor exists yet and these handlers remain inert.
        puppet_cost::hooks_applied(savings, false);
        throw Unavailable("Cannot enable remote animation hooks.");
    }
    puppet_cost::hooks_applied(savings, saving_hooks == savings.count);
    shared().hooks = true;
}
} // namespace native_skater_detail
NativeFrame capture_local(std::uintptr_t base, std::uintptr_t client, bool capture_pose) {
    struct LocalCache {
        std::uintptr_t base{}, client{}, context{}, player{}, entity{}, parent{}, component{}, board_entity{};
    };
    static LocalCache cache;
    NativeFrame frame;
    try {
        require(ptr(client) == base + addr::engine::client_vtable && read<std::uint32_t>(client, 0xc0) == 1,
                "Load a local Hosted map first.");
        frame.context = ptr(client, 8);
        if (!capture_pose && cache.base == base && cache.client == client && cache.context == frame.context &&
            cache.entity && ptr(cache.entity, 0xf8) == cache.player && ptr(cache.entity, 0x20) == frame.context) {
            frame.entity = cache.entity;
            frame.parent = cache.parent;
            frame.board_entity = cache.board_entity;
            frame.pose.root = from_matrix(root_matrix(frame.entity));
            frame.detail = "Local transform ready.";
            frame.ready = true;
            return frame;
        }
        const auto offset = read<std::uint32_t>(base, addr::engine::context_player_manager_offset);
        require(offset <= 0x1000000, "Player manager offset changed.");
        const auto manager = ptr(frame.context, offset);
        require(ptr(manager) == base + addr::engine::local_player_manager_vtable, "Local player manager unavailable.");
        const auto begin = ptr(manager, 0x4c8), end = ptr(manager, 0x4d0);
        require(end >= begin && end - begin == 8, "Expected one local player.");
        const auto player = ptr(begin);
        require(ptr(player) == base + addr::engine::local_player_vtable && read<std::uint8_t>(player, 0x45) == 1 &&
                    !read<std::uint8_t>(player, 0x44),
                "Local human player unavailable.");
        frame.entity = ptr(player, 0xb8);
        require(ptr(frame.entity) == base + addr::engine::skater_entity_vtable && ptr(frame.entity, 0xf8) == player &&
                    ptr(frame.entity, 0x20) == frame.context,
                "Local skater ownership changed.");
        frame.parent = ptr(frame.entity, 0x40);
        frame.pose.root = from_matrix(root_matrix(frame.entity));
        const auto component = ptr(frame.entity, 0x628);
        require(ptr(component) == base + addr::engine::skater_component_vtable,
                "Local animation component unavailable.");
        frame.detail = capture_pose ? "Waiting for skater animation buffers." : "Local transform ready.";
        // A root snapshot remains useful every callback, but the complete rig
        // is only consumed by a 20 TPS pose packet. Avoid reading and converting
        // 395 bones on callbacks that cannot send one.
        if (capture_pose) {
            try {
                frame.pose.skater = capture_skeleton(base, ptr(component, 0xa0), max_skater_bones);
                auto &hidden = hidden_joint();
                std::lock_guard hidden_lock(hidden.mutex);
                if (hidden.component == component && hidden.joint < frame.pose.skater.size())
                    frame.pose.skater[hidden.joint].scale = hidden.scale;
                frame.detail =
                    frame.pose.skater.empty() ? "Waiting for skater animation buffers." : "Local pose ready.";
            } catch (const std::exception &e) {
                frame.pose.skater.clear();
                frame.detail = e.what();
            }
        }
        try {
            const auto board = read_native_board(readable, base, frame.entity);
            frame.board_entity = board.entity;
            if (capture_pose && board.entity) {
                frame.pose.board.push_back(from_matrix(root_matrix(board.entity)));
                const auto bones = capture_skeleton(base, board.holder, max_board_bones - 1);
                frame.pose.board.insert(frame.pose.board.end(), bones.begin(), bones.end());
            }
        } catch (const std::exception &e) {
            frame.board_entity = 0;
            frame.pose.board.clear();
            frame.detail += std::string(" Skateboard capture: ") + e.what();
        }
        cache = {base, client, frame.context, player, frame.entity, frame.parent, component,
                 frame.board_entity};
        frame.ready = true;
    } catch (const std::exception &e) {
        cache = {};
        frame.detail = e.what();
    }
    return frame;
}
NativeFrame capture_local(std::uintptr_t base, std::uintptr_t client) {
    return capture_local(base, client, true);
}
void debris_set_pose(std::uintptr_t holder, std::span<const Transform> bones) {
    auto &registry = piece_registry();
    std::lock_guard lock(registry.mutex);
    for (auto &piece : registry.pieces)
        if (piece.holder == holder) {
            piece.bones.assign(bones.begin(), bones.end());
            return;
        }
    registry.pieces.push_back({holder, {bones.begin(), bones.end()}});
    registry.count.store(registry.pieces.size(), std::memory_order_release);
}
PieceStatus debris_pose_status(std::uintptr_t holder) {
    auto &registry = piece_registry();
    std::lock_guard lock(registry.mutex);
    for (const auto &piece : registry.pieces)
        if (piece.holder == holder)
            return piece.status;
    return {};
}
void debris_clear_pose(std::uintptr_t holder) noexcept {
    auto &registry = piece_registry();
    std::lock_guard lock(registry.mutex);
    std::erase_if(registry.pieces, [holder](const PiecePose &piece) { return piece.holder == holder; });
    registry.count.store(registry.pieces.size(), std::memory_order_release);
}
std::uint64_t remote_pose_updates() noexcept { return remote().applied.load(std::memory_order_relaxed); }
std::uint64_t remote_board_pose_updates() noexcept {
    return remote().board_applied.load(std::memory_order_relaxed);
}
NativeAnimationStats remote_animation_stats() noexcept {
    auto &r = remote();
    return {r.native_evaluated.load(std::memory_order_relaxed),
            r.native_skipped.load(std::memory_order_relaxed),
            r.native_evaluation_us.load(std::memory_order_relaxed),
            r.apply_us.load(std::memory_order_relaxed)};
}
std::uintptr_t remote_skater_entity() noexcept {
    return watched().entity[peer_slot].load(std::memory_order_acquire);
}
std::uintptr_t remote_board_entity() noexcept {
    return watched().board[peer_slot].load(std::memory_order_acquire);
}
std::uint64_t remote_skater_generation() noexcept {
    return remote().generation.load(std::memory_order_acquire);
}
bool install_entity_hooks(std::uintptr_t base, std::string &detail) noexcept {
    try {
        install(base);
        require(GetCurrentThreadId() == shared().engine_thread, "Native entity hooks belong to another thread.");
        return true;
    } catch (const std::exception &e) {
        detail = e.what();
        return false;
    }
}
void set_entity_destroyed_listener(EntityDestroyed listener) noexcept {
    shared().destroyed_listener.store(listener, std::memory_order_release);
}
void set_local_hidden_joint(std::uintptr_t component, std::uint16_t joint,
                            const std::array<float, 3> &scale) noexcept {
    auto &hidden = hidden_joint();
    std::lock_guard lock(hidden.mutex);
    hidden.component = component;
    hidden.joint = joint;
    hidden.scale = scale;
}
void set_render_pose_listener(RenderPosePublished listener) noexcept {
    shared().render_listener.store(listener, std::memory_order_release);
}
void set_animation_evaluated_listener(AnimationEvaluated listener) noexcept {
    shared().evaluated_listener.store(listener, std::memory_order_release);
}
} // namespace dingosdk::multiplayer
