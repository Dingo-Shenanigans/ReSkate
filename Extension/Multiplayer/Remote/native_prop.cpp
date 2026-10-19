#include "native_prop.h"
#include "native_skater_internal.h"
#include "native_creation_list.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/world_props.h"
#include "Extension/Skater/drone_beacon.h"
#include <cmath>
#include <intrin.h>
#include <optional>

namespace dingosdk::multiplayer {
using namespace native_skater_detail;
namespace props = addr::world_props;
namespace {
// ---- Placed props, and the engine taking them away
// Every prop entity placed and not yet destroyed. The engine's destruction hook takes an entity
// out before it is freed, under the same lock that every move and removal holds, so nothing
// moves a freed prop. (Recursive: a removal is a destruction of our own.)
struct Registry {
    std::recursive_mutex mutex;
    std::vector<std::uintptr_t> live;
    std::atomic<std::size_t> count{};
};
Registry &registry() {
    static auto *r = new Registry;
    return *r;
}
bool tracked(std::uintptr_t entity) {
    auto &r = registry();
    return entity && std::find(r.live.begin(), r.live.end(), entity) != r.live.end();
}
void track(std::uintptr_t entity) {
    auto &r = registry();
    r.live.push_back(entity);
    r.count.store(r.live.size(), std::memory_order_release);
}
void untrack(std::uintptr_t entity) {
    auto &r = registry();
    std::erase(r.live, entity);
    r.count.store(r.live.size(), std::memory_order_release);
}

struct Prop {
    std::uintptr_t entity{}, parent{}, context{};
};
// A blueprint the way the cone's is one (world_props.h), or the skateboard's or the skater
// code's; `chain` gets what it was, for the log.
bool is_blueprint(std::uintptr_t base, std::uintptr_t object, std::string &chain) {
    const auto vtable = ptr(object);
    chain = std::format("vtable Skate+{:#x}", vtable - base);
    if (vtable == base + props::object_blueprint_vtable || vtable == base + addr::engine::blueprint_vtable) return true;
    auto type = ptr(object, 8);
    for (int depth = 0; depth < 8 && type; ++depth) {
        if (type == base + addr::engine::board_blueprint_type || type == base + entities::blueprint_type) return true;
        const auto super = ptr(type, 0x20);
        if (super == type) break;
        type = super;
    }
    return false;
}
// A loaded named blueprint, from whichever asset domain holds it; 0 when none does.
std::uintptr_t find_named_blueprint(std::uintptr_t base, std::string_view name) {
    const auto find = game::native_data().find_asset;
    require(find != nullptr, "Native asset lookup is unavailable.");
    const std::string text(name);
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
        if (!ptr(base, addr::engine::domain_owners + domain * std::uintptr_t{8})) continue;
        if (const auto asset = find(domain, text.c_str())) {
            std::string chain;
            if (!is_blueprint(base, asset, chain))
                throw std::runtime_error(std::format("{} is not a blueprint ({}).", name, chain));
            return asset;
        }
    }
    return 0;
}
// Whether entities can be created now, on this thread: the hooks are in, the engine is ready
// and the client's job context is this one. Throws only when something is wrong; false is "wait",
// and `why` (when given) gets what is waited for.
bool can_create(std::uintptr_t base, const NativeFrame &local, std::string *why = nullptr) {
    install(base);
    require(shared().hooks, "The native entity hooks are not installed.");
    require(GetCurrentThreadId() == shared().engine_thread, "Props are created on the client thread.");
    const auto wait = [&](std::string reason) {
        if (why) *why = std::move(reason);
        return false;
    };
    if (read<std::uint8_t>(base, addr::engine::entity_creation_ready) == 0) return wait("the engine is not ready to create entities");
    const auto tls_index = read<std::uint32_t>(base, addr::engine::tls_index);
    require(tls_index <= 4095, "Native TLS index changed.");
    const auto tls = ptr(static_cast<std::uintptr_t>(__readgsqword(0x58)), std::uintptr_t{tls_index} * 8);
    if (read<std::uint8_t>(tls, 0xb19) == 0) return wait("this thread may not create entities now");
    if (const auto context = ptr(tls, 0x550); context != local.context)
        return wait(std::format("the thread's job context {:#x} is not the local skater's {:#x}", context, local.context));
    return true;
}
// As the remote skateboard is made (native_skater_spawn.cpp): a fresh entity through a creation
// list, the list initialized, then placed with its lifecycle callbacks. `kinds` gets the vtable
// of each entity the list made. Under the registry lock.
Prop create_prop(std::uintptr_t base, const NativeFrame &local, std::uintptr_t blueprint, const std::array<float, 16> &where,
                 std::string *kinds = nullptr) {
    alignas(16) const auto matrix = where;
    NativeCreationList list{};
    alignas(16) std::array<std::uint8_t, 0x190> descriptor{};
    using Init = void *(*)(void *, std::uintptr_t, std::uintptr_t, const void *);
    reinterpret_cast<Init>(base + entities::descriptor_init)(descriptor.data(), 0, local.parent, matrix.data());
    struct DescriptorScope {
        std::uintptr_t base;
        void *data;
        ~DescriptorScope() { reinterpret_cast<void (*)(void *)>(base + entities::descriptor_destroy)(static_cast<std::uint8_t *>(data) + 0x10); }
    } scope{base, descriptor.data()};
    const std::uint32_t id = 255; // local, unassociated
    std::memcpy(descriptor.data() + 0x38, &id, sizeof(id));
    descriptor[0x151] = 0;
    auto *list_pointer = &list;
    std::memcpy(descriptor.data() + 0x158, &list_pointer, sizeof(list_pointer));
    std::array<std::uintptr_t, 3> result{};
    using Create = void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    reinterpret_cast<Create>(base + entities::create_entity)(result.data(), descriptor.data(), blueprint, 0, 0);
    if (result[1]) reinterpret_cast<void (*)(std::uintptr_t)>(base + entities::release_reference)(result[1]);
    const auto created = read_native_creation_list(readable, list);
    if (kinds)
        for (const auto entity : created.entities) *kinds += std::format(" Skate+{:#x}", ptr(entity) - base);
    if (!created.pages.empty()) reinterpret_cast<void (*)(void *)>(base + native::initialize_creation_list)(&list);
    for (const auto page : created.pages) {
        const std::uint32_t empty = 0;
        (void)write(page + 8, &empty, sizeof(empty));
        reinterpret_cast<void (*)(void *, std::uintptr_t, std::size_t)>(base + native::free_creation_page)(&list, page, 0x1f0);
    }
    require(result[0] != 0, "The engine did not create the prop.");
    using InitializePlacement = void (*)(std::uintptr_t, const void *, std::uintptr_t, std::uint8_t);
    reinterpret_cast<InitializePlacement>(base + entities::initialize_placement)(result[0], matrix.data(), 0, 1);
    track(result[0]);
    return {result[0], ptr(result[0], 0x40), local.context};
}
// The plain entity transform setter: what moves a prop smoothly (world_props.h). Under the lock.
void place(std::uintptr_t base, const Prop &prop, const std::array<float, 16> &where) {
    alignas(16) const auto matrix = where;
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + entities::place_entity)(prop.entity, matrix.data());
}
// Under the lock. One the engine already took is only forgotten.
void destroy(const Prop &prop) {
    if (!tracked(prop.entity)) return;
    untrack(prop.entity);
    if (shared().hooks && ptr(prop.entity, 0x20) == prop.context) shared().original_destroy(prop.entity, prop.parent);
}
std::array<float, 16> matrix_of(const std::array<float, 3> &position, const std::array<float, 4> &rotation) {
    Transform t;
    t.position = position;
    t.rotation = rotation;
    return to_matrix(t);
}

// ---- Other players' drones
struct DroneModel {
    std::uint64_t id{};
    Prop prop;
};
struct Models {
    std::vector<DroneModel> list;          // client thread
    std::uintptr_t context{}, blueprint{}; // the map the models are on, and what they are made of
    std::string blueprint_name;
    ULONGLONG next_lookup{};
    ULONGLONG waiting_since{}; // since when a flying drone has had no model, 0 while none has
    std::mutex status_mutex;
    std::string status = "No other player is flying a drone.";
};
Models &models() {
    static auto *m = new Models;
    return *m;
}
void set_models_status(std::string text, bool log) {
    auto &m = models();
    {
        std::lock_guard lock(m.status_mutex);
        if (m.status == text) return;
        m.status = text;
    }
    if (log) logging::log(logging::Level::info, logging::Channel::runtime, "Drone models: {}", text);
}

// ---- The test that found how
struct Request {
    std::string blueprint; // empty: stop
    int mode{};
};
struct PropTest {
    std::mutex mutex;
    std::optional<Request> request;
    std::optional<bool> fake_request; // start or stop the pretend other player's drone
    bool fake{};                      // client thread: it flies
    std::array<float, 3> fake_anchor{};
    ULONGLONG fake_started{};
    std::string status = "Not running. Console: dronemodeltest [blueprint] [mode 0-2], dronemodeltest stop";
    Prop prop; // client thread
    std::array<float, 3> anchor{}, position{};
    ULONGLONG started{};
    unsigned frames{};
    int mode{};
};
PropTest &test() {
    static auto *t = new PropTest;
    return *t;
}
void set_test_status(std::string text) {
    logging::log(logging::Level::info, logging::Channel::runtime, "Drone model test: {}", text);
    std::lock_guard lock(test().mutex);
    test().status = std::move(text);
}
std::array<float, 16> yawed(const std::array<float, 3> &position, float yaw) {
    return matrix_of(position, {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)});
}
void test_spawn(std::uintptr_t base, std::uintptr_t client, const Request &request) {
    auto &t = test();
    const auto local = capture_local(base, client, false);
    require(local.ready && local.context && local.parent && local.entity, "The local skater is not ready.");
    require(can_create(base, local), "Waiting for the engine to allow creating entities; try again.");
    const auto blueprint = find_named_blueprint(base, request.blueprint);
    require(blueprint != 0, "That blueprint is not loaded on this map.");
    const auto root = to_matrix(local.pose.root);
    const std::array<float, 3> at{local.pose.root.position[0] - root[8] * 4.0f, local.pose.root.position[1] + 2.0f,
                                  local.pose.root.position[2] - root[10] * 4.0f};
    std::string kinds;
    t.prop = create_prop(base, local, blueprint, yawed(at, 0.0f), &kinds);
    t.anchor = at;
    t.started = GetTickCount64();
    t.frames = 0;
    t.mode = request.mode;
    set_test_status(std::format("spawned {} at ({:.1f}, {:.1f}, {:.1f}): entity {:#x} (vtable Skate+{:#x}), made:{}; moving it with mode {}.",
                                request.blueprint, at[0], at[1], at[2], t.prop.entity, ptr(t.prop.entity) - base, kinds, t.mode));
}
// Round a 3 m circle every 6 s, turning to face along it.
void test_move(std::uintptr_t base) {
    auto &t = test();
    const float seconds = static_cast<float>(GetTickCount64() - t.started) / 1000.0f;
    const float angle = seconds * 6.2831853f / 6.0f;
    const std::array<float, 3> at{t.anchor[0] + 3.0f * std::cos(angle), t.anchor[1] + 0.5f * std::sin(angle * 2.0f),
                                  t.anchor[2] + 3.0f * std::sin(angle)};
    const auto matrix = yawed(at, -angle);
    t.position = at;
    if (t.mode >= 1 && t.prop.parent) {
        const auto transform = ptr(t.prop.parent, 0x18);
        alignas(16) const auto copy = matrix;
        if (transform && ptr(transform) == base + native::blueprint_transform_vtable)
            reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + native::set_blueprint_transform)(transform, copy.data());
    }
    if (t.mode <= 1) place(base, t.prop, matrix);
    if (++t.frames == 120) set_test_status(std::format("moved 120 frames with mode {}. Is it going round a circle?", t.mode));
}
// The pretend other player's drone (dronemodeltest remote): its position goes into the beacon as
// a received one would, so the models (tick_drone_models) and the markers show it with no second
// player. It goes round a 4 m circle every 8 s, in front of where the skater was.
constexpr std::uint64_t fake_remote_id = 1;
// Whether real players' drones get a model (tick_drone_models): off until their crash is found.
constexpr bool players_drone_models = false;
void fake_remote_start(std::uintptr_t base, std::uintptr_t client) {
    auto &t = test();
    const auto local = capture_local(base, client, false);
    require(local.ready && local.entity, "The local skater is not ready.");
    const auto root = to_matrix(local.pose.root);
    t.fake_anchor = {local.pose.root.position[0] - root[8] * 6.0f, local.pose.root.position[1] + 2.5f,
                     local.pose.root.position[2] - root[10] * 6.0f};
    t.fake_started = GetTickCount64();
    t.fake = true;
    set_test_status(std::format("a pretend other player's drone flies round ({:.1f}, {:.1f}, {:.1f}). 'dronemodeltest' says whether its model is shown.",
                                t.fake_anchor[0], t.fake_anchor[1], t.fake_anchor[2]));
}
void fake_remote_fly() {
    auto &t = test();
    const float angle = static_cast<float>(GetTickCount64() - t.fake_started) / 1000.0f * 6.2831853f / 8.0f;
    const drone::beacon::Position at{t.fake_anchor[0] + 4.0f * std::cos(angle), t.fake_anchor[1] + 0.5f * std::sin(angle * 2.0f),
                                     t.fake_anchor[2] + 4.0f * std::sin(angle)};
    const float yaw = -angle;
    drone::beacon::receive(fake_remote_id, "Test drone", at, {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)}, true);
}
void fake_remote_stop() {
    auto &t = test();
    if (!t.fake) return;
    t.fake = false;
    drone::beacon::receive(fake_remote_id, {}, t.fake_anchor, drone::beacon::no_rotation, false);
}
} // namespace

void prop_entity_destroyed(std::uintptr_t entity) noexcept {
    auto &r = registry();
    if (!r.count.load(std::memory_order_acquire)) return; // every destruction passes through here
    std::lock_guard lock(r.mutex);
    if (tracked(entity)) untrack(entity);
}

void tick_drone_models(std::uintptr_t base, std::uintptr_t client, bool show) noexcept {
    auto &m = models();
    try {
        std::lock_guard lock(registry().mutex);
        // The engine took some away (a map change takes them all).
        std::erase_if(m.list, [](const DroneModel &model) { return !tracked(model.prop.entity); });
        auto drones = show ? drone::beacon::snapshot() : std::vector<drone::beacon::Remote>{};
        // Real players' drones are markers only for now: a model made for one crashed the game a
        // few seconds later in the per-frame update of its shader sections (Skate.exe+0x505b04b,
        // 2.1.0, 2026-10-18). The pretend one (dronemodeltest remote) still gets its model, to find why.
        const auto players = drones.size();
        if (!players_drone_models)
            std::erase_if(drones, [](const drone::beacon::Remote &d) { return d.id != fake_remote_id; });
        if (drones.empty() && m.list.empty()) {
            m.waiting_since = 0;
            set_models_status(!show ? "Other players' drones are hidden."
                              : players ? std::format("{} other player(s) flying; shown as markers only.", players)
                                        : std::string("No other player is flying a drone."), false);
            return;
        }
        const auto now = GetTickCount64();
        // A drone flying for a few seconds with no model: why, in the log once.
        const auto waiting = [&](const std::string &why) {
            if (!m.waiting_since) m.waiting_since = now;
            if (now - m.waiting_since >= 3000)
                set_models_status(std::format("{} other player(s) flying; {} model(s) shown; waiting: {}.", drones.size(), m.list.size(), why), true);
        };
        const auto local = capture_local(base, client, false);
        if (!local.ready || !local.context || !local.parent) {
            waiting(std::format("the local skater is not ready (ready {}, context {:#x}, parent {:#x})", local.ready, local.context, local.parent));
            return;
        }
        if (local.context != m.context) {
            // Another map: whatever was there went with it, and the blueprint is found again.
            m.list.clear();
            m.context = local.context;
            m.blueprint = 0;
            m.next_lookup = 0;
        }
        // Those who landed or left.
        for (auto it = m.list.begin(); it != m.list.end();) {
            const bool flying = std::any_of(drones.begin(), drones.end(), [&](const auto &d) { return d.id == it->id; });
            if (flying) {
                ++it;
                continue;
            }
            destroy(it->prop);
            it = m.list.erase(it);
        }
        // What the models are made of: the drone's own model, else the stand-in.
        if (!m.blueprint && !drones.empty() && now >= m.next_lookup) {
            m.next_lookup = now + 5000;
            for (const auto name : {drone_model_blueprint, drone_model_stand_in})
                if ((m.blueprint = find_named_blueprint(base, name))) {
                    m.blueprint_name = std::string(name);
                    break;
                }
            if (!m.blueprint)
                set_models_status("Neither the drone model nor its stand-in is loaded on this map: other players' drones show as markers only.", true);
        }
        // One new model a frame at most, so a lobby taking off together does not stall a frame.
        bool spawned = false;
        std::string blocked;
        for (const auto &drone : drones) {
            const auto matrix = matrix_of(drone.position, drone.rotation);
            const auto found = std::find_if(m.list.begin(), m.list.end(), [&](const DroneModel &model) { return model.id == drone.id; });
            if (found != m.list.end()) {
                place(base, found->prop, matrix);
            } else if (m.blueprint && !spawned && blocked.empty() && m.list.size() < drone::beacon::max_remote &&
                       can_create(base, local, &blocked)) {
                m.list.push_back({drone.id, create_prop(base, local, m.blueprint, matrix)});
                spawned = true;
            }
        }
        if (!blocked.empty()) {
            waiting(blocked);
        } else if (m.blueprint) {
            if (m.list.size() >= drones.size()) m.waiting_since = 0;
            set_models_status(std::format("{} other player(s) flying; {} model(s) shown, made of {}.", drones.size(), m.list.size(),
                                          m.blueprint_name), spawned);
        }
    } catch (const std::exception &e) {
        // Give up on what is there; the next frame starts over.
        for (const auto &model : m.list) destroy(model.prop);
        m.list.clear();
        m.blueprint = 0;
        m.next_lookup = GetTickCount64() + 5000;
        set_models_status(std::format("failed: {}", e.what()), true);
    }
}
std::string drone_models_status() {
    auto &m = models();
    std::lock_guard lock(m.status_mutex);
    return m.status;
}

void request_prop_test(std::string blueprint, int mode) {
    std::lock_guard lock(test().mutex);
    test().request = Request{std::move(blueprint), std::clamp(mode, 0, 2)};
}
void request_fake_remote_drone(bool fly) {
    std::lock_guard lock(test().mutex);
    test().fake_request = fly;
}
std::optional<std::array<float, 3>> prop_test_position() noexcept {
    const auto &t = test();
    if (!t.prop.entity) return std::nullopt;
    return t.position;
}
std::string prop_test_status() {
    std::lock_guard lock(test().mutex);
    return test().status;
}
void tick_prop_test(std::uintptr_t base, std::uintptr_t client) noexcept {
    auto &t = test();
    std::optional<Request> request;
    std::optional<bool> fake_request;
    {
        std::lock_guard lock(t.mutex);
        request.swap(t.request);
        fake_request.swap(t.fake_request);
    }
    try {
        if (fake_request) {
            fake_remote_stop();
            if (*fake_request) fake_remote_start(base, client);
            else set_test_status("the pretend other player's drone landed.");
        }
        if (t.fake) fake_remote_fly();
    } catch (const std::exception &e) {
        fake_remote_stop();
        set_test_status(std::format("failed: {}", e.what()));
    }
    try {
        std::lock_guard lock(registry().mutex);
        if (t.prop.entity && !tracked(t.prop.entity)) {
            t.prop = {};
            set_test_status("the engine took the prop away (a map change).");
        }
        if (request) {
            if (t.prop.entity) destroy(t.prop);
            t.prop = {};
            if (request->blueprint.empty()) set_test_status("stopped; the prop is removed.");
            else test_spawn(base, client, *request);
        }
        if (t.prop.entity) test_move(base);
    } catch (const std::exception &e) {
        t.prop = {};
        set_test_status(std::format("failed: {}", e.what()));
    }
}
} // namespace dingosdk::multiplayer
