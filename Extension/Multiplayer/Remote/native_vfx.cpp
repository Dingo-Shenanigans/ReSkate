#include "native_vfx.h"
#include "native_skater.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_vfx.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>

// Other players' skaters and skateboards are the game's own entities, and the game gives each
// the effects its outfit comes with (a costume's trail, a deck's fire) as it does this player's.
// Three things are missing for them, and are done here:
//  - The game builds their effects before their outfit is on. They are built again after it.
//  - An effect a skater's item puts on the skateboard's wheels follows the skater's own copy of
//    those joints, which only this player's skater keeps up to date. It is built on the
//    skateboard, which has the same joints, instead.
//  - Sparks, dust and puffs come from a skater touching the world, which only happens in that
//    player's game. Each game sends its contacts (effects.h) and the others play them.
namespace dingosdk::multiplayer {
namespace {
namespace vfx = addr::native_vfx;
using Address = std::uintptr_t;
bool readable(Address p, void *out, std::size_t size) {
    if (p < 0x10000 || p > memory::highest_user_address - size)
        return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(p), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> T get(Address p, std::size_t off = 0) {
    T value{};
    readable(p + off, &value, sizeof(value));
    return value;
}
Address ptr(Address p, std::size_t off = 0) { return get<Address>(p, off); }
constexpr Address set_of(Address element) noexcept { return element & ~Address{4}; }

using Register = Address (*)(Address manager, Address key, Address component);
using ImpactCall = Address (*)(Address manager, Address key, const void *impact);
using Refresh = Address (*)(Address manager, Address key);
using ThreadWorld = Address *(*)(Address *out);
using BuildEffects = Address (*)(Address manager, std::uint64_t *entity, int id, Address *settings, char flag, char queued);
using JointIndex = int (*)(Address rig, std::uint32_t joint, int);

// A material no surface names: the game then uses the level's default one.
constexpr std::uint16_t default_material = max_impact_material;
struct Registered {
    Address key{}, component{};
};
// Another player's skater and skateboard, and the effect sets of the skater's that are built on
// the skateboard.
struct Pair {
    Address skater{}, board{}, board_component{};
    std::vector<Address> moved;
    bool changed{}; // `moved` is not what the skateboard was last built with
};
struct State {
    Address base{};
    DWORD client_thread{};
    bool attempted{}, installed{};
    std::string issue;
    Register register_skater{};
    ImpactCall impact{};
    BuildEffects build_effects{};
    std::mutex mutex;
    std::vector<Impact> captured;       // the local game's, since the last drain
    std::vector<Registered> registered; // every effects component seen registering, newest last
    std::vector<Pair> pairs;
    std::array<bool, max_impact_material + 1> seen{}; // materials this game's own contacts named
    std::atomic<std::uint64_t> captured_count{}, played{}, refused{}, refreshed{}, moved{}, changes{};
    std::atomic<unsigned> materials{}; // the level's material index table, as last read
    std::string refusal;               // why the last one was refused (client thread; status reads under mutex)
};
State &state() {
    static auto *s = new State;
    return *s;
}
// One other player: their skater and skateboard entities, and each one's effects component and
// its key with the manager.
struct Remote {
    Address entity{}, key{}, component{}, board{}, board_key{}, board_component{};
    std::uint64_t generation{}, refresh_at{}, next_lookup{}, changes{};
};
Remote &remote() {
    static auto *r = new PeerStorage<Remote>;
    return r->current();
}
Address register_hook(Address manager, Address key, Address component) {
    auto &s = state();
    {
        std::lock_guard lock(s.mutex);
        std::erase_if(s.registered, [&](const Registered &r) { return r.component == component; });
        if (s.registered.size() >= 512) s.registered.erase(s.registered.begin(), s.registered.begin() + 256);
        s.registered.push_back({key, component});
    }
    return s.register_skater(manager, key, component);
}
// Only the game calls through here: this game's own skater touching the world. What is played
// for other players goes straight to the original.
Address impact_hook(Address manager, Address key, const void *native) {
    auto &s = state();
    const auto result = s.impact(manager, key, native);
    const auto error = GetLastError();
    std::array<float, 12> vectors{};
    std::uint32_t material{};
    if (readable(reinterpret_cast<Address>(native), vectors.data(), sizeof(vectors)) &&
        readable(reinterpret_cast<Address>(native) + vfx::impact_material, &material, sizeof(material))) {
        Impact impact;
        for (unsigned i = 0; i < 3; ++i) {
            impact.position[i] = vectors[i];
            impact.velocity[i] = vectors[4 + i];
            impact.normal[i] = vectors[8 + i];
        }
        impact.material = material & vfx::material_valid
                              ? static_cast<std::uint16_t>((material >> vfx::material_shift) & max_impact_material)
                              : default_material;
        impact = wire_impact(impact);
        if (valid_impact(impact)) {
            std::lock_guard lock(s.mutex);
            s.seen[impact.material] = true;
            if (s.captured.size() >= 16) s.captured.erase(s.captured.begin());
            s.captured.push_back(impact);
            ++s.captured_count;
        }
    }
    SetLastError(error);
    return result;
}
// The effects component registered with the manager under this id.
void walk(Address node, int id, Address &component, unsigned depth = 0) {
    if (!node || depth > 24 || component) return;
    if (get<int>(node, vfx::skater_node_id) == id) {
        component = ptr(node, vfx::skater_node_component);
        return;
    }
    walk(ptr(node, 8), id, component, depth + 1);
    walk(ptr(node), id, component, depth + 1);
}
// Whether an effects component belongs to this skater or skateboard entity.
bool owns(Address component, Address entity) {
    if (!component || !entity) return false;
    if (ptr(ptr(component, vfx::component_owner)) == entity) return true;
    for (std::size_t off = 8; off < vfx::component_size; off += 8)
        if (const auto q = ptr(component, off); q && (q == entity || ptr(q) == entity)) return true;
    return false;
}
int call_joint(State &s, Address rig, std::uint32_t joint) noexcept {
    __try {
        return reinterpret_cast<JointIndex>(s.base + vfx::joint_index.rva)(rig, joint, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// Whether every effect of a set sits on a joint this rig has (a skateboard's wheels).
bool set_fits(State &s, Address set, Address rig) {
    const auto items = ptr(set_of(set), vfx::set_items);
    const auto count = items ? get<std::uint32_t>(items - 4) & 0x7fffffffU : 0U;
    if (!rig || !ptr(rig, vfx::rig_skeleton) || !count || count > 64) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto joint = get<std::uint32_t>(set_of(ptr(items, i * 8ULL)), vfx::item_joint);
        if (joint == 0xffffffffU || call_joint(s, rig, joint) < 0) return false;
    }
    return true;
}
// A skater's or skateboard's effects are built from its sets. For another player's, the
// skater's wheel effects are taken out of the skater's and added to the skateboard's.
Address build_hook(Address manager, std::uint64_t *entity, int id, Address *settings, char flag, char queued) {
    auto &s = state();
    thread_local std::vector<Address> block;
    Address data{}, replaced{};
    if (settings && readable(reinterpret_cast<Address>(settings), &data, sizeof(data)) && data) {
        Address component{};
        walk(ptr(manager, vfx::manager_skaters_root), id, component);
        const auto count = get<std::uint32_t>(data - 4) & 0x7fffffffU;
        std::lock_guard lock(s.mutex);
        const auto pair = component && count <= 256
                              ? std::find_if(s.pairs.begin(), s.pairs.end(),
                                             [&](const Pair &p) { return owns(component, p.skater) || owns(component, p.board); })
                              : s.pairs.end();
        if (pair != s.pairs.end()) {
            block.assign(1, 0);
            if (owns(component, pair->skater)) {
                const auto rig = ptr(ptr(pair->board_component, vfx::component_rig_owner), vfx::rig_owner_rig);
                std::vector<Address> moved;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto element = ptr(data, i * 8ULL);
                    // Always on only: the others wait on the skater's own state.
                    if (get<std::int32_t>(set_of(element), vfx::set_trigger) == vfx::trigger_always && set_fits(s, element, rig))
                        moved.push_back(element);
                    else block.push_back(element);
                }
                if (moved != pair->moved) {
                    pair->moved = std::move(moved);
                    pair->changed = true;
                    ++s.changes;
                }
            } else {
                for (std::uint32_t i = 0; i < count; ++i) block.push_back(ptr(data, i * 8ULL));
                for (const auto element : pair->moved)
                    if (std::none_of(block.begin() + 1, block.end(), [&](Address own) { return set_of(own) == set_of(element); }))
                        block.push_back(element);
                pair->changed = false;
            }
            const auto total = static_cast<std::uint32_t>(block.size() - 1);
            if (total != count) {
                block[0] = (Address{total} << 32) | total; // capacity, then the count
                replaced = reinterpret_cast<Address>(block.data() + 1);
                ++s.moved;
            }
        }
    }
    return s.build_effects(manager, entity, id, replaced ? &replaced : settings, flag, queued);
}
template <class Original> bool attach(State &s, const vfx::Function &function, void *replacement, Original &original) {
    void *forward{};
    auto *target = reinterpret_cast<void *>(s.base + function.rva);
    if (hook_prepare(target, replacement, &forward) != HookOk || !forward) {
        s.issue = std::format("cannot prepare the hook at {:#x}", function.rva);
        return false;
    }
    original = reinterpret_cast<Original>(forward);
    if (hook_enable(target) != HookOk) {
        s.issue = std::format("cannot enable the hook at {:#x}", function.rva);
        return false;
    }
    return true;
}
bool matches(State &s, const vfx::Function &function) {
    std::array<unsigned char, 16> actual{};
    if (readable(s.base + function.rva, actual.data(), actual.size()) && actual == function.prefix) return true;
    s.issue = std::format("the game's function at {:#x} differs", function.rva);
    return false;
}
Address thread_world(State &s) noexcept {
    Address world{};
    __try {
        reinterpret_cast<ThreadWorld>(s.base + vfx::thread_world.rva)(&world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        world = 0;
    }
    return world;
}
Address manager(State &s, Address world) {
    if (!world) return 0;
    const auto m = world + get<std::uint32_t>(s.base + vfx::manager_offset);
    return get<std::uint8_t>(m, vfx::manager_ready) ? m : 0;
}
// Whether the game can be handed this material: its index table is read without a bound. The
// table's length sits before it, as with the game's other arrays; a material this game's own
// contacts have named is known good whatever that says.
bool material_known(State &s, Address world, std::uint16_t material) {
    const auto object = world + get<std::uint32_t>(s.base + vfx::materials_offset);
    if (!get<std::uint8_t>(object, vfx::materials_ready)) return false; // the game would crash on it
    const auto grid = ptr(object, vfx::materials_grid);
    if (!grid) return false;
    if (material == default_material) return true;
    const auto table = ptr(ptr(grid, vfx::grid_data) & ~Address{4}, vfx::grid_index_table);
    const auto count = table ? get<std::uint32_t>(table - 4) & 0x7fffffffU : 0U;
    s.materials.store(count);
    if (count <= max_impact_material && material < count) return true;
    std::lock_guard lock(s.mutex);
    return s.seen[material];
}
// The current slot's skater's and skateboard's keys with the manager, from the components
// that registered for them.
Remote &resolve(State &s) {
    auto &r = remote();
    const auto entity = remote_skater_entity(), board = remote_board_entity();
    const auto generation = remote_skater_generation();
    if (!entity) {
        r.key = r.board_key = r.component = r.board_component = 0;
        return r;
    }
    const auto now = GetTickCount64();
    if (r.entity == entity && r.board == board && r.generation == generation &&
        ((r.key && (r.board_key || !board)) || now < r.next_lookup))
        return r;
    r.entity = entity;
    r.board = board;
    r.generation = generation;
    r.key = r.board_key = r.component = r.board_component = 0;
    r.next_lookup = now + 500;
    std::lock_guard lock(s.mutex);
    for (auto found = s.registered.rbegin(); found != s.registered.rend(); ++found) {
        if (!found->key || ptr(found->component) != s.base + vfx::component_vtable) continue;
        if (!r.key && owns(found->component, entity)) {
            r.key = found->key;
            r.component = found->component;
        } else if (!r.board_key && board && owns(found->component, board)) {
            r.board_key = found->key;
            r.board_component = found->component;
        }
    }
    if (r.key && r.board_key) {
        std::erase_if(s.pairs, [&](const Pair &p) { return p.skater == entity || p.board == board; });
        if (s.pairs.size() >= 256) s.pairs.erase(s.pairs.begin());
        s.pairs.push_back({entity, board, r.board_component, {}, false});
    }
    return r;
}
bool call_impact(State &s, Address manager, Address key, const void *native) noexcept {
    __try {
        s.impact(manager, key, native);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_refresh(State &s, Address manager, Address key) noexcept {
    __try {
        reinterpret_cast<Refresh>(s.base + vfx::refresh.rva)(manager, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void refuse(State &s, const char *why) {
    ++s.refused;
    std::lock_guard lock(s.mutex);
    s.refusal = why;
}
} // namespace

void prepare_effects(std::uintptr_t base) noexcept {
    auto &s = state();
    if (s.attempted) return;
    s.attempted = true;
    s.base = base;
    s.client_thread = GetCurrentThreadId();
    try {
        s.installed = matches(s, vfx::register_skater) && matches(s, vfx::impact) && matches(s, vfx::refresh) &&
                      matches(s, vfx::thread_world) && matches(s, vfx::build_effects) && matches(s, vfx::joint_index) &&
                      attach(s, vfx::register_skater, &register_hook, s.register_skater) &&
                      attach(s, vfx::build_effects, &build_hook, s.build_effects) &&
                      attach(s, vfx::impact, &impact_hook, s.impact);
    } catch (...) {
        s.issue = "the hooks could not be installed";
    }
    if (!s.installed)
        logging::log(logging::Level::warning, logging::Channel::runtime,
                     "Multiplayer: skater effects are not shared: {}.", s.issue);
}
std::vector<Impact> drain_impacts() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    auto out = std::exchange(s.captured, {});
    if (out.size() > max_impacts) out.erase(out.begin(), out.end() - max_impacts);
    return out;
}
void play_impact(const Impact &impact) noexcept {
    auto &s = state();
    if (!s.installed || GetCurrentThreadId() != s.client_thread || !valid_impact(impact)) return;
    try {
        const auto world = thread_world(s);
        const auto m = manager(s, world);
        if (!m) return refuse(s, "the game's effects are not running on this thread");
        if (!material_known(s, world, impact.material)) return refuse(s, "a surface this level does not have");
        const auto key = resolve(s).key;
        if (!key) return refuse(s, "the player's skater has no effects of its own yet");
        alignas(16) std::array<std::uint8_t, vfx::impact_size> native{};
        const auto vector = [&](std::size_t at, const std::array<float, 3> &v) {
            std::memcpy(native.data() + at, v.data(), sizeof(v));
        };
        vector(0, impact.position);
        vector(vfx::impact_velocity, impact.velocity);
        vector(vfx::impact_normal, impact.normal);
        const std::uint32_t material =
            impact.material == default_material
                ? 0U
                : (std::uint32_t{impact.material} << vfx::material_shift) | vfx::material_valid;
        const std::array<std::uint32_t, 4> tail{material, material, 0U, std::bit_cast<std::uint32_t>(10.f)};
        std::memcpy(native.data() + vfx::impact_material, tail.data(), sizeof(tail));
        if (call_impact(s, m, key, native.data())) ++s.played;
        else refuse(s, "the game faulted playing one");
    } catch (...) {
    }
}
void note_remote_outfit(std::uint64_t now) noexcept { remote().refresh_at = now + 500000; }
void tick_remote_effects(std::uint64_t now) noexcept {
    auto &s = state();
    auto &r = remote();
    if (!s.installed || GetCurrentThreadId() != s.client_thread) return;
    const auto changes = s.changes.load();
    const bool due = r.refresh_at && now >= r.refresh_at;
    if (!due && changes == r.changes) return;
    try {
        const auto m = manager(s, thread_world(s));
        resolve(s);
        if (!m || !r.key || (r.board && !r.board_key)) {
            if (due) r.refresh_at = now + 500000; // not registered yet
            return;
        }
        r.changes = changes;
        // The skater first: what it hands over is there when the skateboard is built.
        bool board = due;
        if (due) {
            r.refresh_at = 0;
            if (call_refresh(s, m, r.key)) ++s.refreshed;
        } else {
            // The game built their skater again by itself and its wheel effects changed.
            std::lock_guard lock(s.mutex);
            board = std::any_of(s.pairs.begin(), s.pairs.end(), [&](const Pair &p) { return p.skater == r.entity && p.changed; });
        }
        if (board && r.board_key && call_refresh(s, m, r.board_key)) ++s.refreshed;
    } catch (...) {
    }
}
void reset_effects() noexcept {
    auto &s = state();
    each_peer([] { remote() = {}; });
    std::lock_guard lock(s.mutex);
    s.captured.clear();
    s.pairs.clear();
    s.refusal.clear();
}
std::string native_effects_status() {
    auto &s = state();
    if (!s.attempted) return "Effects: not started (join or host a session, or run `mp echo`).";
    if (!s.installed) return "Effects: not shared: " + s.issue + ".";
    std::lock_guard lock(s.mutex);
    return std::format("Effects: {} of your contacts captured; {} played on other players, {} refused{}{}; "
                       "{} skaters and skateboards built again after their outfit, {} with wheel effects moved to the "
                       "skateboard; {} players' skaters known.",
                       s.captured_count.load(), s.played.load(), s.refused.load(),
                       s.refusal.empty() ? "" : " (last: ", s.refusal.empty() ? std::string{} : s.refusal + ")",
                       s.refreshed.load(), s.moved.load(), s.pairs.size());
}
} // namespace dingosdk::multiplayer
