#include "native_skater_internal.h"
#include "native_pose_layout.h"
#include "native_creation_list.h"
#include "native_cosmetics.h"
#include "native_audio.h"
#include "puppet_cost.h"
#include "remote_collision.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <mutex>
#include <map>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <string_view>

namespace dingosdk::multiplayer {
using namespace native_skater_detail;
namespace {
bool code(std::uintptr_t base, std::uintptr_t address) {
    return address >= base && address < base + supported_build::game_image_size;
}
// Entity+0x628 is a cache written by the post-construction callback
// (native::component_cache_callback). The factory's component collection is available before that cache.
std::uintptr_t skater_component(std::uintptr_t base, std::uintptr_t entity) {
    return read_native_skater_component(readable, base, entity);
}
struct Source {
    std::uintptr_t blueprint{}, parent{};
};
void initialize_appearance(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity) {
    const auto getter =
        reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(base + entities::customization_component);
    const auto customization = getter(entity), local_customization = getter(local_entity);
    require(customization && local_customization && customization != local_customization &&
                ptr(customization) == ptr(local_customization) && code(base, ptr(customization)) &&
                ptr(ptr(customization, 0x18)) == entity,
            "Remote customization component unavailable.");
    // ClientSkaterSource::applyAppearance (entities::source_apply_appearance) enables the component and
    // requests default/session appearance through mode 1. No recipe pointers
    // or player association are copied, and no profile data is written.
    const std::uint8_t enabled = 1;
    const std::uint32_t appearance_mode = 1;
    require(write(customization + 0x110, &enabled, 1) && write(customization + 0x134, &appearance_mode, 4) &&
                write(customization + 0x10a, &enabled, 1) && write(customization + 0x10d, &enabled, 1),
            "Cannot request remote actor appearance.");
    if (ptr(customization, 0x120))
        reinterpret_cast<void (*)(std::uintptr_t, const void *, std::uint32_t)>(
            base + addr::engine::set_customization_flag)(customization + 0x120, &enabled, 1);
}
Source find_source(std::uintptr_t base, std::uintptr_t context, std::uintptr_t local_entity) {
    const auto partition_offset = read<std::uint32_t>(base, addr::engine::context_partition_offset);
    require(partition_offset <= 0x1000000, "Source partition offset changed.");
    const auto partition = read<std::uint32_t>(context, partition_offset);
    require(partition <= 4, "Source partition unavailable.");
    Source candidate;
    for (unsigned kind = 0; kind < 7; ++kind) {
        const auto manager =
            ptr(base, addr::engine::source_managers + (std::uintptr_t{partition} * 7 + kind) * 8);
        if (!manager)
            continue;
        require(ptr(manager) == base + addr::engine::source_manager_vtable && ptr(manager, 0x18) == context,
                "Source manager differs.");
        const auto begin = ptr(manager, 0xa0), end = ptr(manager, 0xa8);
        require(begin <= end && (end - begin) % 0x18 == 0 && (end - begin) / 0x18 <= 64,
                "Source collection exceeds bounds.");
        for (auto at = begin; at < end; at += 0x18) {
            const auto source = ptr(at, 8);
            if (ptr(source) != base + addr::engine::client_skater_source_vtable)
                continue;
            const auto data = ptr(source, 0x38), parent = ptr(source, 0x30);
            if (!data || !parent || ptr(source, 0x20) != context || ptr(parent, 0x20) != context)
                continue;
            const auto blueprint = ptr(data, 0xd0) & ~std::uintptr_t{4};
            if (!blueprint)
                continue;
            const auto name_address = ptr(blueprint, 0x18);
            std::array<char, 256> name{};
            if (!readable(name_address, name.data(), name.size()) ||
                std::find(name.begin(), name.end(), '\0') == name.end())
                continue;
            if (std::string_view(name.data()) !=
                "Gameplay/Characters/CharacterBlueprint_Physics_Skater_RSP_CAS")
                continue;
            candidate = {blueprint, parent};
            if (ptr(source, 0x68) == local_entity)
                return candidate;
        }
    }
    require(candidate.blueprint != 0, "Skater source blueprint is not loaded on this map.");
    return candidate;
}
struct CreationScope {
    std::uintptr_t base;
    NativeCreationList list;
    explicit CreationScope(std::uintptr_t image) : base(image) {}
    CreationScope(const CreationScope &) = delete;
    CreationScope &operator=(const CreationScope &) = delete;
    ~CreationScope() {
        try {
            const auto created = read_native_creation_list(readable, list);
            for (const auto page : created.pages) {
                const std::uint32_t empty = 0;
                require(write(page + 8, &empty, sizeof(empty)), "Cannot release blueprint creation page.");
                // Use the same allocator dispatch as ClientSkateboardSource.
                // These pages contain borrowed entity pointers, not ownership.
                reinterpret_cast<void (*)(void *, std::uintptr_t, std::size_t)>(
                    base + native::free_creation_page)(&list, page, 0x1f0);
            }
        } catch (const std::exception &e) {
            logging::log(logging::Level::warning, logging::Channel::runtime,
                         "Multiplayer: blueprint creation list cleanup: {}", e.what());
        }
    }
    void initialize(std::uintptr_t root, std::uintptr_t context, std::uintptr_t local_skater,
                    std::uintptr_t local_board) {
        const auto created = read_native_creation_list(readable, list);
        require(std::find(created.entities.begin(), created.entities.end(), root) != created.entities.end(),
                "Blueprint creation list omitted the remote skateboard.");
        for (const auto entity : created.entities)
            require(entity != local_skater && entity != local_board && code(base, ptr(entity)) &&
                        ptr(entity, 0x20) == context,
                    "Blueprint creation list contains an unrelated entity.");
        // Initializes the complete board blueprint (including mesh controllers),
        // with native EntityInitData, in the same order as ClientSkateboardSource.
        reinterpret_cast<void (*)(void *)>(base + native::initialize_creation_list)(&list);
        logging::log(logging::Level::debug, logging::Channel::runtime,
                     "Multiplayer: initialized {} skateboard blueprint entities.", created.entities.size());
    }
};
std::uintptr_t create_actor(std::uintptr_t base, std::uintptr_t parent, std::uintptr_t blueprint,
                            const Transform &root, NativeCreationList *creation_list = nullptr) {
    // The descriptor owns internal list state and must be constructed in place.
    // We allocate a fresh entity through the engine; no live object is memcpy-cloned.
    alignas(16) std::array<std::uint8_t, 0x190> descriptor{};
    alignas(16) const auto matrix = to_matrix(root);
    using Init = void *(*)(void *, std::uintptr_t, std::uintptr_t, const void *);
    reinterpret_cast<Init>(base + entities::descriptor_init)(descriptor.data(), 0, parent, matrix.data());
    struct DescriptorScope {
        std::uintptr_t base;
        void *data;
        ~DescriptorScope() {
            reinterpret_cast<void (*)(void *)>(base + entities::descriptor_destroy)(
                static_cast<std::uint8_t *>(data) + 0x10);
        }
    } scope{base, descriptor.data()};
    // Same local, unassociated creation mode used by ClientSkaterSource.
    const std::uint32_t id = 255;
    std::memcpy(descriptor.data() + 0x38, &id, sizeof(id));
    descriptor[0x151] = 0;
    std::memcpy(descriptor.data() + 0x158, &creation_list, sizeof(creation_list));
    std::array<std::uintptr_t, 3> result{};
    using Create = void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    reinterpret_cast<Create>(base + entities::create_entity)(result.data(), descriptor.data(), blueprint, 0, 0);
    // The result temporarily retains its sublevel, as in the original caller.
    if (result[1])
        reinterpret_cast<void (*)(std::uintptr_t)>(base + entities::release_reference)(result[1]);
    return result[0];
}
void spawn(std::uintptr_t base, const NativeFrame &local) {
    auto &r = remote();
    install(base);
    require(GetCurrentThreadId() == shared().engine_thread, "Remote spawn must run on the client thread.");
    require(read<std::uint8_t>(base, addr::engine::entity_creation_ready) != 0,
            "Native entity creation is not ready.");
    const auto tls_array = static_cast<std::uintptr_t>(__readgsqword(0x58));
    const auto tls_index = read<std::uint32_t>(base, addr::engine::tls_index);
    require(tls_index <= 4095, "Native TLS index changed.");
    const auto tls = ptr(tls_array, std::uintptr_t{tls_index} * 8);
    require(read<std::uint8_t>(tls, 0xb19) != 0 && ptr(tls, 0x550) == local.context,
            "Waiting for the native client job context.");
    const auto source = find_source(base, local.context, local.entity);
    logging::log(
        logging::Level::info, logging::Channel::runtime,
        "Multiplayer: creating a remote skater; local entity={:#x}, parent={:#x}, captured bones={}.",
        local.entity, source.parent, local.pose.skater.size());
    const auto created = create_actor(base, source.parent, source.blueprint, local.pose.root);
    require(created && created != local.entity, "Engine did not create an independent remote skater.");
    puppet_cost::forget_skater();
    r.context = local.context;
    r.parent = source.parent;
    r.local_entity = local.entity;
    r.entity = created;
    r.applied = 0;
    r.pose_driven.store(false, std::memory_order_release);
    r.native_evaluated = 0;
    r.native_skipped = 0;
    r.native_evaluation_us = 0;
    r.apply_us = 0;
    r.next_native_animation = r.next_safety_audit = r.next_entity_audit = r.next_status = 0;
    watch(watched().entity, r.entity);
    r.generation.fetch_add(1, std::memory_order_acq_rel);
    require(ptr(r.entity) == base + addr::engine::skater_entity_vtable && ptr(r.entity, 0x20) == local.context &&
                !ptr(r.entity, 0xf8),
            "Remote entity is not an unowned skater.");
    r.parent = ptr(r.entity, 0x40);
    r.component = skater_component(base, r.entity);
    require(ptr(r.component) == base + addr::engine::skater_component_vtable && !ptr(r.component, 0x70),
            "Remote skater physics was initialized unexpectedly.");
    reinterpret_cast<void (*)(std::uintptr_t, std::uint8_t)>(base + entities::physics_c7_setter)(r.component, 1);
    const std::uint8_t disabled = 1;
    require(write(r.component + 0xcb, &disabled, 1), "Cannot disable remote physics updates.");
    // ClientSkaterSource calls this initializer before applying appearance.
    // Unlike the ordinary transform setter, it runs the entity's +0x88/+0xc8
    // lifecycle callbacks and binds/initializes the owned components. Merely
    // allocating the blueprint leaves +0x628 and the animation holder empty.
    // Disable physics above, before those callbacks can create simulation state.
    using InitializePlacement = void (*)(std::uintptr_t, const void *, std::uintptr_t, std::uint8_t);
    alignas(16) const auto matrix = to_matrix(local.pose.root);
    reinterpret_cast<InitializePlacement>(base + entities::initialize_placement)(r.entity, matrix.data(), 0, 1);
    require((read<std::uint32_t>(r.entity, 0x28) & 8) != 0 && ptr(r.entity, 0x628) == r.component,
            "Remote skater initialization did not finish.");
    require(!ptr(r.entity, 0xf8) && !ptr(r.component, 0x70) && read<std::uint8_t>(r.component, 0xc7) == 1 &&
                read<std::uint8_t>(r.component, 0xcb) == 1,
            "Remote skater initialization changed its ownership or physics state.");
    logging::log(logging::Level::debug, logging::Channel::runtime,
                 "Multiplayer: remote actor initialized; entity={:#x}, component={:#x}, animation={:#x}.",
                 r.entity, r.component, ptr(r.component, 0xa0));
    initialize_appearance(base, r.entity, local.entity);
    watch(watched().component, r.component);
    r.failed = false;
    r.issue.clear();
}
void remove_board(std::uintptr_t base) {
    auto &r = remote();
    r.board_ready.store(false, std::memory_order_release);
    watch(watched().board_holder, 0);
    const auto entity = watched().board[peer_slot].exchange(0, std::memory_order_acq_rel);
    puppet_cost::forget_board();
    std::uintptr_t parent{};
    {
        std::lock_guard lock(r.mutex);
        parent = r.board_parent;
        r.board_entity = 0;
        r.board_appearance.reset();
        r.board_component = 0;
        r.board_parent = 0;
        r.board_applied = 0;
        r.board_render_pose.clear();
        r.next_board_safety_audit = r.next_board_status = 0;
        r.board_status.clear();
    }
    if (entity && shared().hooks && GetCurrentThreadId() == shared().engine_thread &&
        ptr(entity) == base + addr::engine::board_entity_vtable && ptr(entity, 0x20) == r.context &&
        ptr(entity, 0x40) == parent)
        shared().original_destroy(entity, parent);
}
std::string show_board(std::uintptr_t base, const NativeFrame &local, const Pose &pose) {
    auto &r = remote();
    try {
        {
            std::lock_guard lock(r.mutex);
            require(!r.board_failed, r.board_issue.c_str());
        }
        if (pose.board.empty()) {
            if (r.board_entity) r.next_board_create = GetTickCount64() + 2000;
            remove_board(base);
            return "Waiting for skateboard pose.";
        }
        if (!r.board_entity) {
            if (GetTickCount64() < r.next_board_create) return "Waiting to show the skateboard again.";
            const auto local_board = read_native_board(readable, base, local.entity);
            if (!local_board.entity || local_board.entity != local.board_entity)
                return "Waiting for the local skateboard blueprint.";
            const auto blueprint = read_native_board_blueprint(readable, base, local_board.entity);
            CreationScope creation(base);
            const auto entity = create_actor(base, r.parent, blueprint, pose.board.front(), &creation.list);
            require(entity && entity != local_board.entity && entity != r.entity,
                    "Engine did not create an independent skateboard.");
            r.board_entity = entity;
            watch(watched().board, entity);
            require(ptr(entity) == base + addr::engine::board_entity_vtable && ptr(entity, 0x20) == local.context,
                    "Remote skateboard entity differs.");
            r.board_parent = ptr(entity, 0x40);
            r.board_component = read_native_component(readable, entity, base + addr::engine::board_component_vtable);
            require(!ptr(r.board_component, 0x40) && !ptr(r.board_component, 0x58),
                    "Remote skateboard already has physics or a skater association.");
            // native::board_physics_create checks +0x7d before creating skateboard physics.
            // Set it before initialization; no live/local body is ever copied.
            const std::uint8_t disabled = 1;
            require(write(r.board_component + 0x7d, &disabled, 1), "Cannot disable skateboard physics.");
            creation.initialize(entity, local.context, local.entity, local_board.entity);
            alignas(16) const auto matrix = to_matrix(pose.board.front());
            using InitializePlacement = void (*)(std::uintptr_t, const void *, std::uintptr_t, std::uint8_t);
            reinterpret_cast<InitializePlacement>(base + entities::initialize_placement)(entity, matrix.data(), 0, 1);
            require((read<std::uint32_t>(entity, 0x28) & 8) != 0 && !ptr(r.board_component, 0x40) &&
                        !ptr(r.board_component, 0x58) && read<std::uint8_t>(r.board_component, 0x7d) == 1,
                    "Remote skateboard initialization changed its physics state.");
            initialize_appearance(base, entity, local_board.entity);
            // The holder and mesh controllers must have been initialized by the
            // creation batch. Enabling only the holder leaves an empty mesh list.
            auto visual = read_native_board_visual(readable, base, entity);
            require(visual.initialized, "Skateboard animation resource did not initialize.");
            // Same render-resource enable call made by native board startup
            // (native::board_startup). It also queues the request if resources are still loading.
            reinterpret_cast<void (*)(std::uintptr_t, std::uint8_t)>(base + native::enable_board_resources)(
                visual.holder, 1);
            watch(watched().board_holder, visual.holder);
            r.board_ready.store(true, std::memory_order_release);
            logging::log(logging::Level::debug, logging::Channel::runtime,
                         "Multiplayer: skateboard initialized; entity={:#x}, component={:#x}, "
                         "animation={:#x}, captured bones={}.",
                         entity, r.board_component, ptr(entity, 0xf0), pose.board.size() - 1);
        }
        if (pose.board.size() == 1) {
            {
                std::lock_guard lock(r.mutex);
                r.board_render_pose.clear();
            }
            place_board(base, r.board_entity, pose.board.front());
        }
        // Resource discovery, ownership audits and status formatting are
        // diagnostics, not pose playback. Refresh them at 4 Hz.
        const auto now = GetTickCount64();
        if (now >= r.next_board_status || r.board_status.empty()) {
            require(ptr(r.board_entity) == base + addr::engine::board_entity_vtable &&
                        ptr(r.board_entity, 0x20) == local.context &&
                        ptr(ptr(r.board_component, 0x18)) == r.board_entity && !ptr(r.board_component, 0x40) &&
                        !ptr(r.board_component, 0x58),
                    "Remote skateboard ownership or physics changed.");
            const auto visual = read_native_board_visual(readable, base, r.board_entity);
            if (!visual.initialized || !visual.enabled || !visual.meshes)
                r.board_status =
                    std::format("Skateboard: waiting for render resources ({} meshes).", visual.meshes);
            else if (r.board_applied.load())
                r.board_status =
                    std::format("Skateboard render poses applied; {} meshes loaded.", visual.meshes);
            else
                r.board_status = "Skateboard meshes loaded; waiting for render pose publication.";
            r.next_board_status = now + 250;
        }
        return r.board_status;
    } catch (const std::exception &e) {
        {
            std::lock_guard lock(r.mutex);
            r.board_failed = true;
            r.board_issue = e.what();
        }
        try {
            remove_board(base);
        } catch (...) {
        }
        return std::string("Skateboard: ") + e.what();
    }
}
} // namespace
bool show_remote(std::uintptr_t base, std::uintptr_t client, const NativeFrame &local, const Pose &pose,
                 std::string &detail) {
    (void)client;
    auto &r = remote();
    try {
        // Packets are fully validated at decode/admission and interpolation
        // preserves bone counts. Recheck only the root and shape on this hot path.
        require(local.ready && valid_transform(pose.root) && pose.skater.size() <= max_skater_bones &&
                    pose.board.size() <= max_board_bones,
                "Remote pose is unavailable.");
        if (r.entity && (r.context != local.context || r.local_entity != local.entity))
            remove_remote(base);
        if (!r.entity)
            spawn(base, local);
        require(GetCurrentThreadId() == shared().engine_thread,
                "Remote updates must run on the client thread.");
        {
            std::lock_guard lock(r.mutex);
            require(!r.failed, r.issue.c_str());
            r.target = pose;
            ++r.target_revision;
            r.pose_driven.store(!pose.skater.empty(), std::memory_order_release);
        }
        const auto now = GetTickCount64();
        if (now >= r.next_entity_audit) {
            require(ptr(r.entity) == base + addr::engine::skater_entity_vtable &&
                        ptr(r.entity, 0x20) == local.context && !ptr(r.entity, 0xf8),
                    "Remote skater ownership changed.");
            r.next_entity_audit = now + 1000;
        }
        if (pose.skater.empty())
            place_actor(base, r.entity, pose.root);
        // The board refreshes at 4 Hz; between, its stored status is used without a copy.
        const bool board_due =
            !r.board_entity || pose.board.size() <= 1 || r.board_status.empty() || now >= r.next_board_status;
        std::string board;
        if (board_due)
            board = show_board(base, local, pose);
        if (detail.empty() || now >= r.next_status) {
            detail = pose.skater.empty() ? "Remote actor created; root movement only (animation unavailable)."
                     : r.applied.load()  ? "Remote actor receiving skater poses."
                                         : "Remote actor created; waiting for its animation update.";
            detail += ' ';
            detail += board_due ? board : r.board_status;
            r.next_status = now + 250;
        }
        return true;
    } catch (const std::exception &e) {
        detail = e.what();
        return false;
    }
}
void remove_remote(std::uintptr_t base) noexcept {
    stop_remote_audio();
    clear_remote_collision(base);
    auto &r = remote();
    try {
        remove_board(base);
    } catch (...) {
    }
    watch(watched().component, 0);
    const auto entity = watched().entity[peer_slot].exchange(0, std::memory_order_acq_rel);
    r.generation.fetch_add(1, std::memory_order_acq_rel);
    puppet_cost::forget_skater();
    std::uintptr_t context{}, parent{};
    {
        std::lock_guard lock(r.mutex);
        context = r.context;
        parent = r.parent;
        r.entity = 0;
        r.component = 0;
        r.skater_appearance.reset();
        r.board_appearance.reset();
        r.rejected_appearance.reset();
        r.target = {};
        r.pose_driven.store(false, std::memory_order_release);
        r.next_native_animation = r.next_safety_audit = r.next_entity_audit = r.next_board_safety_audit =
            r.next_board_status = r.next_status = r.next_board_create = 0;
        r.failed = false;
        r.issue.clear();
        r.board_failed = false;
        r.board_issue.clear();
    }
    try {
        if (entity && shared().hooks && GetCurrentThreadId() == shared().engine_thread &&
            ptr(entity) == base + addr::engine::skater_entity_vtable && ptr(entity, 0x20) == context &&
            ptr(entity, 0x40) == parent &&
            !ptr(entity, 0xf8))
            shared().original_destroy(entity, ptr(entity, 0x40));
    } catch (...) { /* Level teardown may have already invalidated the entity. */
    }
}
void update_remote_cosmetics(std::uintptr_t base, const NativeFrame &local, const Appearance &appearance,
                             std::string &detail) {
    auto &r = remote();
    try {
        require(valid_appearance(appearance), "Peer cosmetic recipe is invalid.");
        require(r.entity && r.context == local.context && r.local_entity == local.entity &&
                    GetCurrentThreadId() == shared().engine_thread,
                "Waiting for the remote cosmetic actors.");
        std::lock_guard lock(r.mutex);
        // Each attempt checks the whole outfit against the catalog. One that was
        // rejected is retried at the next pass (a fresh actor may still be settling),
        // then every 5 s until the peer sends another; the detail keeps the rejection.
        const auto now = GetTickCount64();
        if (r.rejected_appearance && *r.rejected_appearance == appearance && now < r.next_appearance_retry)
            return;
        try {
            if (!r.skater_appearance || *r.skater_appearance != appearance.skater) {
                apply_cosmetic_recipe(base, r.entity, local.entity, appearance.skater);
                r.skater_appearance = appearance.skater;
            }
            r.rejected_appearance.reset();
            if (!r.board_entity || !local.board_entity) {
                r.board_appearance.reset();
                detail = "Cosmetics: skater recipe applied; waiting for the skateboard.";
                return;
            }
            if (!r.board_appearance || *r.board_appearance != appearance.board) {
                apply_cosmetic_recipe(base, r.board_entity, local.board_entity, appearance.board);
                r.board_appearance = appearance.board;
            }
        } catch (...) {
            const bool again = r.rejected_appearance && *r.rejected_appearance == appearance;
            r.rejected_appearance = appearance;
            r.next_appearance_retry = now + (again ? 5000 : 0);
            throw;
        }
        detail = "Cosmetics: peer skater and skateboard recipes applied.";
    } catch (const std::exception &e) {
        detail = std::string("Cosmetics: ") + e.what();
        // A player whose outfit cannot be put on is left looking like this one. Say why in the
        // log, a few times per reason: waiting for their actors is the ordinary start of every
        // player, and a reason that goes on would otherwise fill it.
        const std::string_view why = e.what();
        if (why != "Waiting for the remote cosmetic actors.") {
            static std::mutex mutex;
            static std::map<std::string, unsigned, std::less<>> seen;
            std::lock_guard lock(mutex);
            if (const auto found = seen.find(why); found != seen.end() || seen.size() < 32) {
                auto &count = found != seen.end() ? found->second : seen.emplace(why, 0U).first->second;
                if (++count <= 3 || count % 200 == 0)
                    logging::log(logging::Level::warning, logging::Channel::runtime,
                                 "Multiplayer: a player's outfit could not be shown, so they look like you ({} so far): {}",
                                 count, why);
            }
        }
    }
}
} // namespace dingosdk::multiplayer
