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
};
struct Live {
    std::uintptr_t entity{}, owner{}, context{};
    std::string asset;
    Matrix matrix{};
    std::uint64_t moved{};
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

void set_joint_source(JointSource source) noexcept { joint_source.store(source, std::memory_order_release); }

std::string set(std::string_view owner, std::uint32_t id, std::string asset, const Matrix& matrix, bool hidden, std::optional<Attach> attach) {
    if (asset.empty() || asset.size() > 200) return "an object's blueprint is a name of 1 to 200 characters";
    if (!finite(matrix)) return "an object's transform must be finite, within 100 km, each axis at most 200 long";
    if (attach && attach->joint >= 395) return "a skater's joints are 0 to 394";
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const Key key{std::string(owner), id};
    if (!s.wanted.contains(key)) {
        const auto mine = std::count_if(s.wanted.begin(), s.wanted.end(), [&](const auto& w) { return w.first.first == owner; });
        if (static_cast<std::size_t>(mine) >= max_per_owner) return std::format("at most {} objects", max_per_owner);
        if (s.wanted.size() >= max_total) return "the world holds as many objects as ReSkate keeps";
    }
    s.wanted[key] = {std::move(asset), matrix, hidden, attach};
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

void entity_destroyed(std::uintptr_t entity) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.live, [&](const auto& l) { return l.second.entity == entity; });
}

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
                w.matrix = on_joint(*at, w.matrix);
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
            ++created;
            std::lock_guard lock(s.mutex);
            s.live[key] = std::move(made);
        }
        if (created) note(s, std::format("{} object(s) created", created));
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
