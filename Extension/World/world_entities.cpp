#include "world_entities.h"
#include "world_entities_contract.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ai_skaters.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

// The native calls are SmokeyDawg58's (from his fork's world objects): a bus under the
// local skater's ClientSkaterSource, a creation descriptor at the transform, CreateEntity from the
// blueprint, InitializePlacement; MoveSpatialEntity to move it, DestroyEntity to take it away.
namespace dingosdk::world_entities {
namespace {
namespace e = addr::skater_entities;
namespace b = addr::ai_skaters;
template<class T> T get(std::uintptr_t p) { T v{}; memory::peek(p, v); return v; }
std::uintptr_t ptr(std::uintptr_t p, std::uintptr_t offset = 0) { return get<std::uintptr_t>(p + offset); }
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<std::size_t N> void prefix(std::uintptr_t base, std::uintptr_t rva, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> actual{};
    require(memory::peek_bytes(base + rva, actual.data(), N) && actual == expected, "World object entity ABI differs.");
}

struct Wanted {
    std::string asset;
    Matrix matrix{};
    bool hidden{};
    std::optional<Attach> attach;
    bool solid{true};
};
struct Live {
    std::uintptr_t entity{}, owner{}, context{};
    std::string asset;
    Matrix matrix{};
    std::uint64_t moved{};
    // Its physics bodies once found, whether they are ghosted, and when to look for them again.
    std::vector<Body> bodies;
    bool looked{}, ghosted{};
    std::uint64_t made{}, next_look{};
};
struct Asset {
    std::uintptr_t asset{};
    std::uint16_t domain{};
    std::uint64_t retry{};
    bool looked{};
};
using Key = std::pair<std::string, std::uint32_t>;
struct State {
    detail::WorldObjectLifetime lifetime;
    std::mutex mutex;
    std::map<Key, Wanted> wanted;
    std::map<Key, Live> live;
    std::map<std::string, Asset> assets;
    std::uintptr_t context{};
    bool verified{};
    std::string error, waiting;
    std::map<Key, std::uint64_t> lost; // client thread: attached objects whose joint is not known, since when
};
State& state() { static auto* s = new State; return *s; }
std::atomic<JointSource> joint_source{};
Physics& physics() { static auto* value = new Physics; return *value; }
std::mutex test_mutex;
std::string test_request; // under test_mutex
std::atomic<bool> physics_set{};

// An object's physics bodies: in its entity and what it points to (two levels, objects with a
// vtable in the game image only), the physics world's address followed by a body the world says is
// valid. All of them must have one owner (a world object's own), and be few.
std::vector<Body> find_bodies(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t world) {
    const auto& p = physics();
    std::vector<Body> found;
    std::vector<std::pair<std::uintptr_t, unsigned>> queue{{entity, 0}};
    std::set<std::uintptr_t> seen{entity};
    const auto game_object = [&](std::uintptr_t at) {
        const auto vtable = ptr(at);
        return vtable >= base && vtable < base + 0x10000000;
    };
    for (std::size_t next = 0; next < queue.size() && queue.size() < 512; ++next) {
        const auto [node, depth] = queue[next];
        std::array<std::uintptr_t, 0x80> words{};
        if (!memory::peek(node, words)) continue;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto value = words[i];
            if (value == world && i + 1 < words.size()) {
                const Body body{world, words[i + 1]};
                if (static_cast<std::uint32_t>(body[1]) < (1u << 24) && p.valid(body) &&
                    std::find(found.begin(), found.end(), body) == found.end())
                    found.push_back(body);
                continue;
            }
            if (depth < 2 && value > 0x10000 && value < 0x7ff000000000 && !seen.contains(value) && game_object(value)) {
                seen.insert(value);
                queue.emplace_back(value, depth + 1);
            }
        }
    }
    if (found.empty() || found.size() > 16) return {};
    const auto owner = p.owner(found.front());
    for (const auto& body : found)
        if (p.owner(body) != owner) return {};
    return found;
}

// `offset` (in a joint's frame) placed on the joint: each row taken through the joint's axes.
Matrix on_joint(const Matrix& joint, const Matrix& offset) {
    Matrix m{};
    for (int row = 0; row < 4; ++row) {
        for (int k = 0; k < 3; ++k) {
            float v = row == 3 ? joint[12 + k] : 0.0f;
            for (int j = 0; j < 3; ++j) v += offset[row * 4 + j] * joint[j * 4 + k];
            m[row * 4 + k] = v;
        }
    }
    m[15] = 1.0f;
    return m;
}
// The joint stood level at its position: up is world up, and forward its own forward axis laid flat
// (its up axis laid flat when the forward axis points up or down). Rows right (fz, 0, -fx), up,
// forward, as the skater's own frame is (heading 0 faces +Z, right +X).
Matrix level(const Matrix& joint) {
    float fx = joint[8], fz = joint[10];
    if (std::hypot(fx, fz) < 0.2f * std::hypot(joint[8], joint[9], joint[10])) fx = joint[4], fz = joint[6];
    const float length = std::hypot(fx, fz);
    if (!(length > 1e-6f)) fx = 0, fz = 1;
    else fx /= length, fz /= length;
    return {fz, 0, -fx, 0, 0, 1, 0, 0, fx, 0, fz, 0, joint[12], joint[13], joint[14], 1};
}

void note(State& s, std::string issue) {
    if (s.waiting == issue) return;
    s.waiting = std::move(issue);
    logging::log(logging::Level::debug, logging::Channel::runtime, "World objects: {}", s.waiting);
}
bool finite(const Matrix& m) {
    for (const auto v : m)
        if (!std::isfinite(v)) return false;
    for (int axis = 0; axis < 3; ++axis) {
        const float length = std::sqrt(m[axis * 4] * m[axis * 4] + m[axis * 4 + 1] * m[axis * 4 + 1] + m[axis * 4 + 2] * m[axis * 4 + 2]);
        if (length > 200.f) return false;
    }
    return std::fabs(m[12]) < 100000.f && std::fabs(m[13]) < 100000.f && std::fabs(m[14]) < 100000.f;
}
std::uintptr_t parent_bus(std::uintptr_t base, std::uintptr_t context) {
    return detail::object_parent_bus(base, context, [](auto at) { return ptr(at); },
        [](auto at) { return get<std::uint32_t>(at); }, [](auto at) { return get<std::uint8_t>(at); },
        [](auto at) -> std::string {
            std::array<char, 256> text{};
            if (!at || !memory::peek_bytes(at, text.data(), text.size()) || std::find(text.begin(), text.end(), '\0') == text.end()) return {};
            return text.data();
        });
}
bool intact(const Live& l) {
    const auto collection = ptr(l.entity, 0x70);
    return ptr(l.entity, 0x20) == l.context && ptr(l.entity, 0x40) == l.owner && collection && ptr(collection) == l.entity;
}
Live create(std::uintptr_t base, std::uintptr_t parent, std::uintptr_t asset, const Matrix& matrix) {
    using CreateBus = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t, std::uintptr_t);
    const auto bus = reinterpret_cast<CreateBus>(base + b::create_bus)(ptr(ptr(parent, 8), 0x48), parent, 0, 0, 0, 0);
    require(bus != 0, "World object entity bus unavailable.");
    reinterpret_cast<int (*)(std::uintptr_t)>(base + b::acquire_bus)(bus);
    struct BusScope {
        std::uintptr_t base, bus;
        ~BusScope() { reinterpret_cast<void (*)(std::uintptr_t)>(base + e::release_reference)(bus); }
    } bus_scope{base, bus};
    alignas(16) std::array<unsigned char, 0x190> descriptor{};
    reinterpret_cast<void* (*)(void*, std::uintptr_t, std::uintptr_t, const void*)>(base + e::descriptor_init)(descriptor.data(), 0, bus, matrix.data());
    struct Scope {
        std::uintptr_t base;
        void* data;
        ~Scope() { reinterpret_cast<void (*)(void*)>(base + e::descriptor_destroy)(static_cast<unsigned char*>(data) + 0x10); }
    } scope{base, descriptor.data()};
    const std::uint32_t id = 255;
    std::memcpy(descriptor.data() + 0x38, &id, 4);
    descriptor[0x151] = 0;
    std::array<std::uintptr_t, 3> result{};
    reinterpret_cast<void* (*)(void*, void*, std::uintptr_t, std::uintptr_t, std::uintptr_t)>(base + e::create_entity)(result.data(), descriptor.data(), asset, 0, 0);
    if (result[1]) reinterpret_cast<void (*)(std::uintptr_t)>(base + e::release_reference)(result[1]);
    require(result[0] != 0, "A world object blueprint did not create an entity.");
    Live live{result[0], ptr(result[0], 0x40), ptr(result[0], 0x20)};
    reinterpret_cast<void (*)(std::uintptr_t, const void*, std::uintptr_t, std::uint8_t)>(base + e::initialize_placement)(live.entity, matrix.data(), 0, 1);
    return live;
}
void move(std::uintptr_t base, const Live& l, const Matrix& matrix) {
    reinterpret_cast<void (*)(std::uintptr_t, const void*, std::uint8_t, std::uint8_t)>(base + e::move_spatial_entity)(l.entity, matrix.data(), 1, 1);
}
void destroy(std::uintptr_t base, const Live& l) {
    // No lock held: the shared destroy hook calls entity_destroyed back.
    if (ptr(l.entity, 0x20) == l.context && ptr(l.entity, 0x40) == l.owner)
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + e::destroy_entity)(l.entity, l.owner);
}
void destroy_all(std::uintptr_t base) {
    auto& s = state();
    std::map<Key, Live> old;
    {
        std::lock_guard lock(s.mutex);
        old.swap(s.live);
    }
    for (const auto& [key, l] : old) destroy(base, l);
}
// A blueprint by name: in a domain already known (validated each time), else looked for in every
// loaded domain at most once a second. UI and world bundles live in child domains, so the skater's
// parent domains alone cannot find a mod's objects.
std::uintptr_t resolve(std::uintptr_t base, Asset& a, const std::string& name, std::uint64_t now, unsigned& lookups) {
    const auto find = game::native_data().find_asset;
    if (!find) return 0;
    const auto read = [](auto at) { return ptr(at); };
    if (a.asset) {
        if (detail::object_resident(a.asset, find(a.domain, name.c_str()), base, read)) return a.asset;
        a.asset = 0;
    }
    if (now < a.retry || lookups == 0) return 0;
    --lookups;
    a.retry = now + 1000;
    a.looked = true;
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
        if (!ptr(base + addr::engine::domain_owners, std::uintptr_t(domain) * 8)) continue;
        const auto asset = find(domain, name.c_str());
        if (detail::object_blueprint(asset, base, read)) {
            a.asset = asset;
            a.domain = domain;
            return asset;
        }
    }
    return 0;
}
} // namespace

// ---- the blueprints loaded -------------------------------------------------------------------
// Each asset domain (engine::domain_owners) keeps its named assets in a registry (domain + 0x48),
// with registries of its own at +0x58..+0x60 (and theirs): an EASTL hash map of the name's hash to
// an entry (buckets at +0xf8, their count at +0x100, a node {hash, entry, next}), behind the
// critical section at +0x90 (the game's own lookup, 0x41b5800 -> 0x3db78d0 / 0x3db7f60). An entry
// starts with the tagged asset (bits 1 and 2 are tags; bit 2 set: one more pointer to follow,
// 0x3db9060), then the asset's GUID and the name's hash. An asset's name is at +0x18; it is taken
// only when it hashes to the node's key, as the game hashes names (5381, x33, xor, A-Z folded).
// Read in a running game on 2026-10-21: 57 registries, 2930 world object blueprints.
namespace {
std::uint64_t asset_name_hash(std::string_view name) {
    std::uint64_t hash = 0x1505;
    for (const unsigned char c : name) hash = (std::uint64_t{c} + (c >= 'A' && c <= 'Z' ? 0x20 : 0)) ^ (hash * 33);
    return hash;
}
bool prefixed(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto a = static_cast<unsigned char>(name[i]), b = static_cast<unsigned char>(prefix[i]);
        if ((a >= 'A' && a <= 'Z' ? a + 32 : a) != (b >= 'A' && b <= 'Z' ? b + 32 : b)) return false;
    }
    return true;
}
std::optional<std::string> asset_name(std::uintptr_t asset, std::uint64_t key) {
    const auto at = ptr(asset + 0x18);
    if (!at) return std::nullopt;
    std::array<char, 257> text{};
    if (!memory::peek_bytes(at, text.data(), text.size() - 1)) {
        // A short name near the end of readable memory: read it in smaller pieces.
        std::size_t length = 0;
        for (; length < 256 && memory::peek(at + length, text[length]) && text[length]; ++length) {}
        text[length] = 0;
    }
    const std::string name(text.data(), strnlen(text.data(), 256));
    if (name.empty() || name.size() >= 256 || asset_name_hash(name) != key) return std::nullopt;
    return name;
}
// The mesh box of a world object blueprint's asset (Engine/Game/Build/20260929/skater_entities.h).
std::optional<Box> asset_bounds(std::uintptr_t base, std::uintptr_t asset) {
    const auto data = asset + e::object_data_offset;
    if (ptr(data) != base + e::object_data_vtable) return std::nullopt;
    const auto mesh = ptr(data + e::object_data_mesh_offset);
    if (!mesh || ptr(mesh) != base + e::mesh_asset_vtable) return std::nullopt;
    const auto loaded = ptr(mesh + e::mesh_asset_loaded_offset);
    if (!loaded || ptr(loaded) != base + e::loaded_mesh_vtable) return std::nullopt;
    const auto at = ptr(loaded + e::loaded_mesh_bounds_offset);
    std::array<float, 8> v{};
    if (!at || !memory::peek(at, v)) return std::nullopt;
    Box box{{{v[0], v[1], v[2]}, {v[4], v[5], v[6]}}};
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(box[0][i]) || !std::isfinite(box[1][i]) || box[1][i] < box[0][i] || box[1][i] - box[0][i] > 2000) return std::nullopt;
    return box;
}
void walk_registry(std::uintptr_t base, std::uintptr_t registry, std::string_view prefix, bool meshes_only, std::set<std::string>& found,
                   std::set<std::uintptr_t>& walked, unsigned depth = 0) {
    if (!registry || depth > 16 || walked.size() >= 4096 || !walked.insert(registry).second) return;
    // Its own registries first, outside its lock (the game takes each one's lock on its own).
    if (const auto first = ptr(registry + 0x58), last = ptr(registry + 0x60); first && last >= first && last - first <= 8 * 4096)
        for (auto at = first; at < last; at += 8)
            if (const auto child = ptr(at)) walk_registry(base, child, prefix, meshes_only, found, walked, depth + 1);
    const auto buckets = ptr(registry + 0xf8);
    const auto count = get<std::uint32_t>(registry + 0x100);
    if (!buckets || !count || count > (1u << 22)) return;
    auto* section = reinterpret_cast<CRITICAL_SECTION*>(registry + 0x90);
    EnterCriticalSection(section);
    struct Leave { CRITICAL_SECTION* section; ~Leave() { LeaveCriticalSection(section); } } leave{section};
    const auto sentinel = ptr(buckets + std::uintptr_t{count} * 8);
    const auto read = [](auto at) { return ptr(at); };
    std::size_t steps = 0;
    for (std::uint32_t bucket = 0; bucket < count; ++bucket) {
        for (auto node = ptr(buckets + std::uintptr_t{bucket} * 8); node && node != sentinel && ++steps < (1u << 22); node = ptr(node + 16)) {
            const auto entry = ptr(node + 8);
            const auto tagged = entry ? ptr(entry) : 0;
            if (!tagged) continue;
            auto asset = tagged & ~std::uintptr_t{6};
            if (tagged & 4) asset = ptr(asset);
            if (!detail::object_blueprint(asset, base, read) || (meshes_only && !asset_bounds(base, asset))) continue;
            if (auto name = asset_name(asset, get<std::uint64_t>(node)); name && prefixed(*name, prefix)) found.insert(std::move(*name));
        }
    }
}
struct Listing {
    std::mutex mutex;
    std::uint64_t at{};
    std::string prefix;
    bool meshes_only{};
    std::vector<std::string> names;
};
Listing& listing() { static auto* value = new Listing; return *value; }
} // namespace

std::vector<std::string> blueprints(std::uintptr_t base, std::string_view prefix, std::size_t most, bool meshes_only) {
    auto& l = listing();
    std::lock_guard lock(l.mutex);
    const auto now = GetTickCount64();
    if (!l.at || now >= l.at + 2000 || l.prefix != prefix || l.meshes_only != meshes_only) {
        std::set<std::string> found;
        std::set<std::uintptr_t> walked;
        try {
            for (std::uint16_t domain = 0; domain < 0xbbf && found.size() < 65536; ++domain) {
                const auto owner = ptr(base + addr::engine::domain_owners, std::uintptr_t(domain) * 8);
                if (const auto registry = owner ? ptr(owner + 0x48) : 0) walk_registry(base, registry, prefix, meshes_only, found, walked);
            }
        } catch (...) {
        }
        l.names.assign(found.begin(), found.end());
        l.prefix = std::string(prefix);
        l.meshes_only = meshes_only;
        l.at = now;
    }
    return {l.names.begin(), l.names.begin() + static_cast<std::ptrdiff_t>(std::min(most, l.names.size()))};
}

std::optional<Box> blueprint_bounds(std::uintptr_t base, std::string_view blueprint) {
    const auto find = game::native_data().find_asset;
    if (!find || blueprint.empty() || blueprint.size() > 200) return std::nullopt;
    const std::string name(blueprint);
    const auto read = [](auto at) { return ptr(at); };
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
        if (!ptr(base + addr::engine::domain_owners, std::uintptr_t(domain) * 8)) continue;
        if (const auto asset = find(domain, name.c_str()); detail::object_blueprint(asset, base, read)) return asset_bounds(base, asset);
    }
    return std::nullopt;
}

void set_joint_source(JointSource source) noexcept { joint_source.store(source, std::memory_order_release); }
void test_bodies(std::string name) {
    std::lock_guard lock(test_mutex);
    test_request = std::move(name);
}
void set_physics(const Physics& p) noexcept {
    physics() = p;
    physics_set.store(true, std::memory_order_release);
}

std::string set(std::string_view owner, std::uint32_t id, std::string asset, const Matrix& matrix, bool hidden, std::optional<Attach> attach,
                bool solid) {
    if (asset.empty() || asset.size() > 200) return "an object's blueprint is a name of 1 to 200 characters";
    if (!finite(matrix)) return "an object's transform must be finite, within 100 km, each axis at most 200 long";
    if (attach && attach->joint >= 395) return "a skater's joints are 0 to 394";
    if (attach && !(attach->ground >= 0 && attach->ground <= 50)) return "an object finds the ground at most 50 m below its joint";
    if ((!solid || (attach && attach->ground > 0)) && !physics_set.load(std::memory_order_acquire))
        return "objects that are not solid or stand on the ground are unavailable for this game build";
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const Key key{std::string(owner), id};
    if (!s.wanted.contains(key)) {
        const auto mine = std::count_if(s.wanted.begin(), s.wanted.end(), [&](const auto& w) { return w.first.first == owner; });
        if (static_cast<std::size_t>(mine) >= max_per_owner) return std::format("at most {} objects", max_per_owner);
        if (s.wanted.size() >= max_total) return "the world holds as many objects as ReSkate keeps";
    }
    s.wanted[key] = {std::move(asset), matrix, hidden, attach, solid};
    return {};
}
void remove(std::string_view owner, std::uint32_t id) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.wanted.erase({std::string(owner), id});
}
void clear(std::string_view owner) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.wanted, [&](const auto& w) { return w.first.first == owner; });
}
Status status(std::string_view owner, std::uint32_t id) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const Key key{std::string(owner), id};
    const auto w = s.wanted.find(key);
    if (w == s.wanted.end()) return Status::none;
    if (w->second.hidden) return Status::hidden;
    if (const auto l = s.live.find(key); l != s.live.end() && l->second.asset == w->second.asset) return Status::ready;
    const auto a = s.assets.find(w->second.asset);
    if (a == s.assets.end() || !a->second.looked) return Status::waiting;
    return a->second.asset ? Status::loading : Status::missing;
}
bool ready(std::string_view owner) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    for (const auto& [key, w] : s.wanted) {
        if (key.first != owner || w.hidden) continue;
        const auto l = s.live.find(key);
        if (l == s.live.end() || l->second.asset != w.asset) return false;
    }
    return true;
}
std::size_t count(std::string_view owner) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return static_cast<std::size_t>(std::count_if(s.wanted.begin(), s.wanted.end(), [&](const auto& w) { return w.first.first == owner; }));
}

std::vector<std::uintptr_t> live_entities() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<std::uintptr_t> out;
    for (const auto& [key, l] : s.live) out.push_back(l.entity);
    return out;
}

void entity_destroyed(std::uintptr_t entity) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.live, [&](const auto& l) { return l.second.entity == entity; });
}

namespace {
// An object's bodies, looked for while it is new (its physics may come a few frames after it), for
// the floor below it and for being made not solid; then ghosted or made solid as it is wanted.
void bodies_for(std::uintptr_t base, const Key& key, Live& l, const Wanted& w, std::uint64_t now) {
    auto& p = physics();
    if (!physics_set.load(std::memory_order_acquire) || !p.world) return;
    const bool needed = !w.solid || (w.attach && w.attach->ground > 0);
    if (!needed && !l.ghosted) return;
    if (l.bodies.empty() && !l.looked && now >= l.next_look) {
        l.next_look = now + 250;
        if (const auto world = p.world()) l.bodies = find_bodies(base, l.entity, world);
        // Not kept in the entity where they were looked for: rays into the object's own box, the
        // first body they meet, and the rest of that body's owner's.
        if (l.bodies.empty() && p.hit && p.owned)
            if (const auto box = blueprint_bounds(base, l.asset)) {
                const auto& m = l.matrix;
                const auto at = [&](float x, float y, float z) {
                    return std::array<float, 3>{m[12] + m[0] * x + m[4] * y + m[8] * z, m[13] + m[1] * x + m[5] * y + m[9] * z,
                                                m[14] + m[2] * x + m[6] * y + m[10] * z};
                };
                const auto& lo = (*box)[0];
                const auto& hi = (*box)[1];
                const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
                const std::array<std::pair<std::array<float, 3>, std::array<float, 3>>, 3> rays{{
                    {at(cx, hi[1] + 0.5f, cz), at(cx, lo[1] + 0.05f, cz)}, // short of the floor it stands on
                    {at(lo[0] - 0.5f, cy, cz), at(hi[0], cy, cz)},
                    {at(cx, cy, lo[2] - 0.5f), at(cx, cy, hi[2])},
                }};
                for (const auto& [from, to] : rays)
                    if (const auto body = p.hit(from, to)) {
                        l.bodies = p.owned(*body, 16);
                        if (!l.bodies.empty()) break;
                    }
            }
        if (!l.bodies.empty() || now >= l.made + 5000) {
            l.looked = true;
            logging::log(logging::Level::info, logging::Channel::runtime, "World objects: {} {} has {} physics bod{}.", key.first, l.asset,
                         l.bodies.size(), l.bodies.size() == 1 ? "y" : "ies");
        }
    }
    const bool ghost = !w.solid;
    if (ghost != l.ghosted && !l.bodies.empty()) {
        bool all = true;
        for (const auto& body : l.bodies) all = p.ghost(body, ghost) && all;
        if (all) l.ghosted = ghost;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (const auto at = s.live.find(key); at != s.live.end() && at->second.entity == l.entity) {
        at->second.bodies = l.bodies;
        at->second.looked = l.looked;
        at->second.ghosted = l.ghosted;
        at->second.next_look = l.next_look;
    }
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto& s = state();
    try {
        {
            std::lock_guard lock(s.mutex);
            if (s.lifetime.blocked()) return;
            if (s.wanted.empty() && s.live.empty()) return;
        }
        const auto context = ptr(client, 8);
        const auto now = GetTickCount64();
        if (s.context && s.context != context) {
            // The old client context took its entities with it.
            std::lock_guard lock(s.mutex);
            s.live.clear();
            s.assets.clear();
            s.context = 0;
        }
        if (!ready || !context) return note(s, "waiting for local client world readiness");
        const auto tls_array = static_cast<std::uintptr_t>(__readgsqword(0x58));
        const auto index = get<std::uint32_t>(base + addr::engine::tls_index);
        if (index > 4095) return;
        const auto tls = ptr(tls_array, std::uintptr_t(index) * 8);
        if (!get<std::uint8_t>(base + addr::engine::entity_creation_ready) || !get<std::uint8_t>(tls + 0xb19) || ptr(tls, 0x550) != context)
            return note(s, "waiting for verified native client creation scope");
        s.context = context;
        if (!s.verified) {
            prefix(base, e::descriptor_init, e::descriptor_init_prefix);
            prefix(base, e::descriptor_destroy, e::descriptor_destroy_prefix);
            prefix(base, e::create_entity, e::create_entity_prefix);
            prefix(base, e::initialize_placement, e::initialize_placement_prefix);
            prefix(base, e::move_spatial_entity, e::move_spatial_entity_prefix);
            prefix(base, b::create_bus, b::create_bus_prefix);
            prefix(base, b::acquire_bus, b::acquire_bus_prefix);
            // ReSkate's shared entity lifetime hook detours Destroy: it tells us of each one.
            std::string detail;
            require(multiplayer::install_entity_hooks(base, detail), "Native entity lifetime hooks unavailable.");
            s.verified = true;
        }

        std::map<Key, Wanted> wanted;
        std::map<Key, Live> live;
        {
            std::lock_guard lock(s.mutex);
            wanted = s.wanted;
            live = s.live;
        }
        // Attached objects onto their joints as they are now; one whose joint is not known stays put
        // (or waits), and after a second is taken away.
        const auto source = joint_source.load(std::memory_order_acquire);
        std::vector<Key> unplaced;
        for (auto& [key, w] : wanted) {
            if (!w.attach || w.hidden) continue;
            if (const auto at = source ? source(w.attach->player, w.attach->joint) : std::nullopt; at && finite(*at)) {
                const Matrix held{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, (*at)[12], (*at)[13], (*at)[14], 1};
                const float height = w.matrix[13];
                w.matrix = on_joint(!w.attach->turn ? held : w.attach->upright ? level(*at) : *at, w.matrix);
                // On the floor within reach below the joint (the object's own bodies are not floor).
                if (w.attach->ground > 0 && physics_set.load(std::memory_order_acquire) && physics().floor) {
                    const auto l = live.find(key);
                    static const std::vector<Body> none;
                    if (const auto floor = physics().floor({(*at)[12], (*at)[13], (*at)[14]}, w.attach->ground, l != live.end() ? l->second.bodies : none))
                        w.matrix[13] = *floor + height;
                }
                s.lost.erase(key);
            } else {
                unplaced.push_back(key);
                s.lost.try_emplace(key, now);
            }
        }
        std::erase_if(s.lost, [&](const auto& l) { return !wanted.contains(l.first); });
        // Gone or hidden, or a different blueprint now: taken away.
        std::vector<Key> drop;
        for (const auto& [key, l] : live) {
            const auto w = wanted.find(key);
            const bool gone = w != wanted.end() && s.lost.contains(key) && now - s.lost[key] > 1000;
            if (w == wanted.end() || w->second.hidden || w->second.asset != l.asset || gone || !intact(l)) drop.push_back(key);
        }
        // A blueprint whose bundle streamed out: its objects come back once it resolves again.
        unsigned lookups = 4;
        std::map<std::string, std::uintptr_t> resolved;
        for (const auto& [key, w] : wanted) {
            if (w.hidden || resolved.contains(w.asset)) continue;
            resolved[w.asset] = resolve(base, s.assets[w.asset], w.asset, now, lookups);
        }
        for (const auto& [key, l] : live)
            if (const auto r = resolved.find(l.asset); r != resolved.end() && !r->second && std::find(drop.begin(), drop.end(), key) == drop.end())
                drop.push_back(key);
        for (const auto& key : drop) {
            const auto l = live.find(key);
            destroy(base, l->second);
            live.erase(l);
            std::lock_guard lock(s.mutex);
            s.live.erase(key);
        }
        std::uintptr_t parent{};
        unsigned created{};
        for (const auto& [key, w] : wanted) {
            if (w.hidden || std::find(unplaced.begin(), unplaced.end(), key) != unplaced.end()) continue;
            if (const auto l = live.find(key); l != live.end()) {
                bodies_for(base, key, l->second, w, now);
                // Move a resident object in place: recreating restarts its effects and makes it jump.
                // One on a joint follows it every frame.
                if (l->second.matrix != w.matrix && (w.attach || now >= l->second.moved + 16)) {
                    move(base, l->second, w.matrix);
                    std::lock_guard lock(s.mutex);
                    if (const auto at = s.live.find(key); at != s.live.end()) at->second.matrix = w.matrix, at->second.moved = now;
                }
                continue;
            }
            const auto asset = resolved[w.asset];
            if (!asset) continue;
            if (created >= 64) break; // spread a long route over a few frames
            if (!parent) parent = parent_bus(base, context);
            if (!parent) return note(s, "waiting for the native local skater entity bus");
            auto made = create(base, parent, asset, w.matrix);
            made.asset = w.asset;
            made.matrix = w.matrix;
            made.moved = now;
            made.made = now;
            ++created;
            std::lock_guard lock(s.mutex);
            s.live[key] = std::move(made);
        }
        if (created) note(s, std::format("{} object(s) created", created));
        // Test only: a per-body setter on every live object's bodies.
        std::string test;
        {
            std::lock_guard lock(test_mutex);
            test.swap(test_request);
        }
        if (!test.empty() && physics_set.load(std::memory_order_acquire) && physics().test) {
            unsigned ran{}, bodies{};
            std::lock_guard lock(s.mutex);
            for (const auto& [key, l] : s.live)
                for (const auto& body : l.bodies) {
                    ++bodies;
                    if (physics().test(test, body)) ++ran;
                }
            logging::log(logging::Level::info, logging::Channel::runtime, "World objects: {} ran on {} of {} bodies.", test, ran, bodies);
        }
    } catch (const std::exception& error) {
        if (s.error != error.what()) {
            s.error = error.what();
            logging::log(logging::Level::error, logging::Channel::runtime, "World objects: {}", s.error);
        }
    } catch (...) {
        logging::write(logging::Level::error, logging::Channel::runtime, "World object creation failed.");
    }
}

void before_level_transition(std::uintptr_t base, unsigned next) noexcept {
    auto& s = state();
    bool release{};
    {
        std::lock_guard lock(s.mutex);
        release = s.lifetime.before_transition(next);
    }
    if (!release) return;
    // Retired in the live world, before its component data goes.
    try {
        destroy_all(base);
    } catch (...) {
        logging::write(logging::Level::error, logging::Channel::runtime, "World object cleanup before a level change failed.");
    }
    std::lock_guard lock(s.mutex);
    s.live.clear();
    s.wanted.clear();
    s.assets.clear();
    s.lost.clear();
    s.context = 0;
    logging::log(logging::Level::info, logging::Channel::runtime, "World objects retired before native state {}.", next);
}
} // namespace dingosdk::world_entities
