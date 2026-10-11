#include "remote_collision.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>

namespace dingosdk::multiplayer {
namespace {
using namespace addr::remote_collision;
constexpr std::uintptr_t highest = memory::highest_user_address;
// For the status line, read from the console thread.
std::atomic<int> availability{-1};
std::atomic<bool> local_on{};
std::atomic<unsigned> solid_players{}, capsule_slots{};
// A body or shape of the physics world.
struct Handle {
    std::uintptr_t world{};
    std::uint32_t index = 0xffffffff, generation{};
};
static_assert(sizeof(Handle) == 16);
using WorldOf = std::uintptr_t (*)(std::uintptr_t context);
using AllocateBody = Handle *(*)(std::uintptr_t world, Handle *out);
using SetInt = void (*)(Handle *, int);
using SetFloat = void (*)(Handle *, float);
using Touch = void (*)(Handle *);
using CreateCapsule = Handle *(*)(std::uintptr_t world, Handle *out, const Handle *body, const float *a,
                                  const float *b, float radius);
using SetMask = void (*)(Handle *, const std::uint32_t *);
using SetOwner = void (*)(Handle *, const Handle *);
using SetVector = void (*)(Handle *, const float *);
using Valid = bool (*)(const Handle *);
struct Natives {
    WorldOf world{};
    AllocateBody allocate{};
    SetInt motion{};
    SetFloat body_980{};
    Touch body_aa0{}, wake{};
    CreateCapsule capsule{};
    SetMask categories{}, collides_with{};
    SetOwner owner{};
    SetVector center_of_mass{}, transform{}, linear_velocity{}, angular_velocity{};
    Valid body_valid{}, shape_valid{};
    std::uintptr_t base{};
    int state = -1; // -1 unchecked, 0 unavailable, 1 ready
};
Natives &natives() { static Natives value; return value; }
bool resolve(std::uintptr_t base) noexcept {
    auto &n = natives();
    if (n.state >= 0) return n.state == 1 && n.base == base;
    const std::array contracts{&physics_world, &allocate_body, &set_motion_type, &set_body_980, &clear_body_aa0,
                               &create_capsule, &set_categories, &set_collides_with, &set_shape_owner,
                               &set_center_of_mass, &set_transform, &set_linear_velocity, &set_angular_velocity,
                               &wake_body, &body_valid, &shape_valid};
    n.state = 1;
    for (const auto *contract : contracts) {
        std::array<unsigned char, 32> actual{};
        if (!memory::peek(base + contract->rva, actual) || actual != contract->bytes) n.state = 0;
    }
    availability.store(n.state);
    if (!n.state) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
                     "Player collision is unavailable: this game build's physics code is not the known one.");
        return false;
    }
    const auto at = [&](const game::build::Fingerprint &f) { return base + f.rva; };
    n.base = base;
    n.world = reinterpret_cast<WorldOf>(at(physics_world));
    n.allocate = reinterpret_cast<AllocateBody>(at(allocate_body));
    n.motion = reinterpret_cast<SetInt>(at(set_motion_type));
    n.body_980 = reinterpret_cast<SetFloat>(at(set_body_980));
    n.body_aa0 = reinterpret_cast<Touch>(at(clear_body_aa0));
    n.wake = reinterpret_cast<Touch>(at(wake_body));
    n.capsule = reinterpret_cast<CreateCapsule>(at(create_capsule));
    n.categories = reinterpret_cast<SetMask>(at(set_categories));
    n.collides_with = reinterpret_cast<SetMask>(at(set_collides_with));
    n.owner = reinterpret_cast<SetOwner>(at(set_shape_owner));
    n.center_of_mass = reinterpret_cast<SetVector>(at(set_center_of_mass));
    n.transform = reinterpret_cast<SetVector>(at(set_transform));
    n.linear_velocity = reinterpret_cast<SetVector>(at(set_linear_velocity));
    n.angular_velocity = reinterpret_cast<SetVector>(at(set_angular_velocity));
    n.body_valid = reinterpret_cast<Valid>(at(body_valid));
    n.shape_valid = reinterpret_cast<Valid>(at(shape_valid));
    return true;
}

// One keyframed capsule: the skater's, or the one along their skateboard.
struct Part {
    Handle body, shape;
    std::array<float, 3> last{};
    std::uint64_t last_at{};
    bool created{}, solid{};
};
struct Slot {
    std::uintptr_t world{}, context{};
    Part skater, board;
    std::uint64_t updated_at{}, retry_at{};
};
PeerStorage<Slot> &slots() { static auto *value = new PeerStorage<Slot>; return *value; }
std::atomic<bool> enabled{true};
std::uint64_t failures{};

std::uintptr_t current_world(const Natives &n) noexcept {
    __try {
        std::uintptr_t context{};
        if (!memory::peek(n.base + game_context_global, context) || !context) return 0;
        return n.world(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// Guards a handle from before the world at this address was replaced: the shape must still
// have no owner and no categories but ours, and both handles their current generation.
bool still_ours(const Natives &n, const Part &p) noexcept {
    std::uintptr_t state{}, owners{}, categories{};
    std::uint32_t owner{}, category{};
    if (!memory::peek(p.shape.world + 0x28, state) || !memory::peek(state + 0x1008, owners) ||
        !memory::peek(state + 0xfe8, categories) || !memory::peek(owners + std::uintptr_t{p.shape.index} * 4, owner) ||
        !memory::peek(categories + std::uintptr_t{p.shape.index} * 4, category) || owner != 0xffffffff ||
        (category && category != remote_proxy_categories))
        return false;
    __try {
        return n.body_valid(&p.body) && n.shape_valid(&p.shape);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// What the retail skater proxy (0x1043390) does for a remote skater's capsule, without the
// joint that drives it there: a keyframed body with no owner, inert until switched on.
bool create_part(const Natives &n, std::uintptr_t world, Part &part, const float *a, const float *b,
                 float radius) noexcept {
    __try {
        Handle body{}, shape{};
        n.allocate(world, &body);
        n.motion(&body, 2);
        n.body_980(&body, 0.01f);
        n.body_aa0(&body);
        alignas(16) const float origin[4]{};
        n.center_of_mass(&body, origin);
        n.capsule(world, &shape, &body, a, b, radius);
        const Handle none{world, 0xffffffff, 0};
        n.owner(&shape, &none);
        const std::uint32_t off = 0;
        n.categories(&shape, &off);
        n.collides_with(&shape, &off);
        if (!n.body_valid(&body) || !n.shape_valid(&shape)) return false;
        part.body = body;
        part.shape = shape;
        part.created = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool place_part(const Natives &n, Part &p, const float *transform, const float *velocity, bool solid) noexcept {
    __try {
        const std::uint32_t in = solid ? remote_proxy_categories : 0, with = solid ? remote_proxy_collides_with : 0;
        alignas(16) const float zero[4]{};
        n.categories(&p.shape, &in);
        n.collides_with(&p.shape, &with);
        if (solid) {
            n.transform(&p.body, transform);
            n.linear_velocity(&p.body, velocity);
            n.angular_velocity(&p.body, zero);
            n.wake(&p.body);
        } else {
            n.linear_velocity(&p.body, zero);
        }
        p.solid = solid;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void switch_off(const Natives &n, Part &p) noexcept {
    if (!p.created || !p.solid) return;
    if (!still_ours(n, p) || !place_part(n, p, nullptr, nullptr, false)) p = {};
}
// Places a part at `position` / `rotation` (x y z w), moving at the speed it moved since the
// last frame (so a push has the player's speed behind it; a jump is a teleport, not a hit).
void move_part(const Natives &n, Part &p, const std::array<float, 3> &position, const std::array<float, 4> &rotation,
               std::uint64_t now) noexcept {
    alignas(16) float velocity[4]{};
    if (p.last_at && now > p.last_at && now - p.last_at < 250000) {
        const float dt = static_cast<float>(now - p.last_at) * 1e-6f;
        float speed2{};
        for (int i = 0; i < 3; ++i) {
            velocity[i] = (position[i] - p.last[i]) / dt;
            speed2 += velocity[i] * velocity[i];
        }
        if (!std::isfinite(speed2) || speed2 > 30.f * 30.f) std::fill(std::begin(velocity), std::end(velocity), 0.f);
    }
    p.last = position;
    p.last_at = now;
    alignas(16) const float transform[8]{rotation[0], rotation[1], rotation[2], rotation[3],
                                         position[0], position[1], position[2], 1.f};
    if (!still_ours(n, p) || !place_part(n, p, transform, velocity, true)) p = {};
}
std::uintptr_t pointer(std::uintptr_t object, std::uintptr_t offset = 0) noexcept {
    std::uintptr_t value{};
    if (object < 0x10000 || object > highest - offset || !memory::peek(object + offset, value) ||
        value < 0x10000 || value > highest - 0x10000) return 0;
    return value;
}
} // namespace

bool local_allows_player_collision(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!entity || pointer(entity) != base + addr::engine::skater_entity_vtable) return false;
    const auto component = pointer(entity, 0x628); // the skater component (entity init 0x53fe80)
    const auto core = pointer(component, 0x70);
    std::uint8_t on{};
    const bool allows = component && pointer(component) == base + addr::engine::skater_component_vtable && core &&
                        pointer(core) == base + addr::no_bail::bail_core_vtable &&
                        memory::peek(core + core_party_collision, on) && on;
    local_on.store(allows, std::memory_order_relaxed);
    return allows;
}

void update_remote_collision(std::uintptr_t base, std::uintptr_t context, const Pose &pose, bool solid,
                             std::uint64_t now) noexcept {
    try {
        auto &s = slots().current();
        solid = solid && enabled.load(std::memory_order_relaxed);
        if (!solid && !s.skater.solid && !s.board.solid) return; // nothing to do for players out of reach
        if (!resolve(base)) return;
        const auto &n = natives();
        const auto world = current_world(n);
        if (!world) return; // kept as they are until the physics is back or replaced
        if (s.world != world || s.context != context) {
            // Another world (a level load): the old handles belong to it, never touch them. The
            // same world under another context: switch the old ones off first (guarded).
            if (s.world == world) {
                switch_off(n, s.skater);
                switch_off(n, s.board);
            }
            s = {};
            s.world = world;
            s.context = context;
        }
        s.updated_at = now;
        if (!solid) {
            switch_off(n, s.skater);
            switch_off(n, s.board);
            return;
        }
        if ((!s.skater.created || !s.board.created) && now >= s.retry_at) {
            alignas(16) const float bottom[4]{0, proxy_bottom + proxy_radius, 0, 0}, top[4]{0, proxy_top - proxy_radius, 0, 0};
            // Along the deck: the skateboard's length is its local Z (the truck positions' axis).
            alignas(16) const float back[4]{0, 0.08f, -0.3f, 0}, front[4]{0, 0.08f, 0.3f, 0};
            if ((!s.skater.created && !create_part(n, world, s.skater, bottom, top, proxy_radius)) ||
                (!s.board.created && !create_part(n, world, s.board, back, front, 0.12f))) {
                s.retry_at = now + 5000000;
                if (failures++ < 5)
                    logging::log(logging::Level::warning, logging::Channel::runtime,
                                 "Player collision: a collision capsule could not be created; trying again in 5 s.");
            }
        }
        // Upright at the skater's root, as retail's proxy stands.
        if (s.skater.created) move_part(n, s.skater, pose.root.position, {0, 0, 0, 1}, now);
        if (s.board.created) {
            // Solid under and beside its skater only (board_solid_reach).
            float apart{};
            if (!pose.board.empty())
                for (unsigned i = 0; i < 3; ++i) {
                    const float d = pose.board.front().position[i] - pose.root.position[i];
                    apart += d * d;
                }
            if (pose.board.empty() || !(apart <= board_solid_reach * board_solid_reach)) switch_off(n, s.board);
            else move_part(n, s.board, pose.board.front().position, pose.board.front().rotation, now);
        }
    } catch (...) {}
}

void clear_remote_collision(std::uintptr_t base) noexcept {
    auto &s = slots().current();
    if ((!s.skater.solid && !s.board.solid) || !resolve(base)) return;
    const auto &n = natives();
    // No physics right now: expire_remote_collision switches them off once it is back.
    if (const auto world = current_world(n); !world) return;
    else if (world != s.world) {
        s = {};
        return;
    }
    switch_off(n, s.skater);
    switch_off(n, s.board);
}

void expire_remote_collision(std::uintptr_t base, std::uint64_t now) noexcept {
    // Only while some capsule is solid: the natives are resolved then.
    auto &n = natives();
    if (n.state != 1 || n.base != base) return;
    std::uintptr_t world = 0;
    bool looked = false;
    unsigned solid{}, allocated{};
    for (auto &s : slots().slots) {
        allocated += s.skater.created || s.board.created;
        if (!s.skater.solid && !s.board.solid) continue;
        ++solid;
        if (now - s.updated_at < 250000) continue;
        if (!looked) { world = current_world(n); looked = true; }
        if (!world) continue; // kept until the physics is back, or replaced
        if (world != s.world) {
            s = {};
            continue;
        }
        switch_off(n, s.skater);
        switch_off(n, s.board);
    }
    solid_players.store(solid, std::memory_order_relaxed);
    capsule_slots.store(allocated, std::memory_order_relaxed);
}

void set_remote_collision_enabled(bool on) noexcept { enabled.store(on, std::memory_order_relaxed); }
bool remote_collision_enabled() noexcept { return enabled.load(std::memory_order_relaxed); }

std::string remote_collision_status() {
    const auto state = availability.load();
    return std::format("Player collision: {}, {}; your party collision is {} (the game's Enable Party Collision "
                       "setting, shown while the playercollision feature is on); {} solid player(s), {} slot(s) "
                       "with capsules.",
                       state < 0 ? "not used yet" : state ? "available" : "unavailable for this game build",
                       remote_collision_enabled() ? "on" : "off (mp collision on)",
                       local_on.load() ? "on" : "off", solid_players.load(), capsule_slots.load());
}
} // namespace dingosdk::multiplayer
