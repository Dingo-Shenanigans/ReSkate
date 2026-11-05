#include "plugin_effects.h"
#include "effect_receivers.h"
#include "effect_draw_budget.h"
#include "effect_asset.h"
#include "Extension/Decals/native_decals.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/effects.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/replay_activity.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace dingosdk::effects {
namespace {
namespace fx = addr::effects;
namespace decal = decals::native;
struct Entry {
    std::string owner;
    std::uintptr_t particle{}, decal_realm{};
    decal::Handle decal;
    ReceiverId receiver{};
    Matrix local{}, drawn{};
    std::uint64_t expires{};
    bool stopped{}, retired{};
};
struct State {
    std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::map<std::string, AssetLookup, std::less<>> assets;
    Receivers receivers;
    DrawBudget budget;
    std::uintptr_t base{}, service{}, manager{}, world{}, entity{};
    std::uint64_t serial{}, budget_at{}, epoch{1};
    DWORD thread{};
    std::atomic<bool> suspended{true};
    bool checked{}, valid{}, allowed{};
};
State& state() { static auto* s = new State; return *s; }
#ifndef DINGOSDK_EFFECT_FUNCTION
#define DINGOSDK_EFFECT_FUNCTION(base, rva) ((base) + (rva))
#endif
template<class F> F function(const State& s, std::uintptr_t rva) { return reinterpret_cast<F>(DINGOSDK_EFFECT_FUNCTION(s.base, rva)); }
std::uintptr_t pointer(std::uintptr_t address) { std::uintptr_t value{}; memory::peek(address, value); return value; }
std::uintptr_t service(const State& s) {
    const auto p = pointer(s.base + fx::service), vtable = pointer(p);
    if (!p || !vtable) return 0;
    for (const auto slot : fx::service_methods) {
        const auto method = pointer(vtable + slot);
        if (method < s.base || method >= s.base + supported_build::game_image_size) return 0;
    }
    return p;
}
#ifndef DINGOSDK_EFFECT_SERVICE
#define DINGOSDK_EFFECT_SERVICE(s) service(s)
#endif
#ifndef DINGOSDK_EFFECT_DECAL_MANAGER
#define DINGOSDK_EFFECT_DECAL_MANAGER(base) decal::manager(base)
#endif
#ifndef DINGOSDK_EFFECT_DECAL_CREATE
#define DINGOSDK_EFFECT_DECAL_CREATE(base, manager, asset, transform) decal::create(base, manager, asset, transform, decal::ReceiverMask::all)
#endif
#ifndef DINGOSDK_EFFECT_DECAL_RELEASE
#define DINGOSDK_EFFECT_DECAL_RELEASE(base, handle) decal::release(base, handle)
#endif
bool same_services(const State& s) { return DINGOSDK_EFFECT_SERVICE(s) == s.service && DINGOSDK_EFFECT_DECAL_MANAGER(s.base) == s.manager; }
void destroy(State& s, Entry& e) {
    // Replacing one global service must not abandon the other's live effects.
    if (e.particle && DINGOSDK_EFFECT_SERVICE(s) == s.service) {
        function<void(*)(std::uintptr_t*, bool, float)>(s, fx::stop.rva)(&e.particle, true, 0);
        function<void(*)(std::uintptr_t*)>(s, fx::release.rva)(&e.particle);
    }
    // The manager can outlive its realm. Never send an old ID to a cleared or
    // replacement realm, even when the global service itself has not changed.
    if (e.decal.id && e.decal_realm && DINGOSDK_EFFECT_DECAL_MANAGER(s.base) == s.manager &&
        decal::client_realm(s.manager) == e.decal_realm) DINGOSDK_EFFECT_DECAL_RELEASE(s.base, e.decal);
}
void reset(State& s) {
    if (s.allowed || !s.entries.empty()) ++s.epoch;
    // Queue releases before forwarding native level teardown, including when
    // it arrives on another thread. Native EffectManager's level-unload handler
    // drains its queues before the level-wide Game compartment is cleared.
    if (s.valid)
        for (auto& [id, e] : s.entries) destroy(s, e);
    s.entries.clear(); s.assets.clear();
    if (s.valid) (void)s.budget.update(s.base, 0);
    s.budget_at = 0;
    s.allowed = false;
}
bool usable(const State& s) { return s.valid && s.allowed && !s.suspended && s.thread == GetCurrentThreadId() && same_services(s); }
Entry* owned(State& s, std::string_view plugin, std::uint64_t id) {
    const auto it = s.entries.find(id);
    return usable(s) && it != s.entries.end() && !it->second.retired && it->second.owner == plugin ? &it->second : nullptr;
}
std::uintptr_t asset(State& s, const std::string& name) {
    const auto find = game::native_data().find_asset;
    if (!find) return 0;
    if (!s.assets.contains(name) && s.assets.size() >= 512) return 0;
    return s.assets[name].resolve(GetTickCount64(), [&](std::uint16_t domain) -> std::uintptr_t {
        if (!pointer(s.base + addr::engine::domain_owners + sizeof(std::uintptr_t) * domain)) return 0;
        return find(domain, name.c_str());
    });
}
#ifndef DINGOSDK_EFFECT_ASSET
#define DINGOSDK_EFFECT_ASSET(s, name) asset(s, name)
#endif
bool replaying(std::uintptr_t base) {
    namespace r = addr::replay_activity;
    const auto client = pointer(base + r::client), manager = pointer(client + r::manager);
    const auto lease = pointer(manager + r::lease);
    return lease && pointer(lease) == base + r::lease_vtable && pointer(lease + r::lease_manager) == manager;
}
}
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool noclip, bool editor) noexcept {
    try {
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (!s.thread) s.thread = GetCurrentThreadId();
        if (s.thread != GetCurrentThreadId()) return;
        if (!s.checked) {
            s.base = base; s.checked = true; s.valid = base != 0;
            for (const auto& f : {fx::descriptor_init, fx::descriptor_destroy, fx::create, fx::move, fx::stop, fx::release}) {
                std::array<unsigned char, 32> bytes{};
                s.valid = memory::read(base + f.rva, bytes) && bytes == f.bytes && s.valid;
            }
            s.valid = s.valid && decal::contracts_match(base) && decal::opacity_contracts_match(base) &&
                decal::surface_contracts_match(base) && s.receivers.initialize(base);
        }
        if (!s.valid || base != s.base) return;
        const auto current = DINGOSDK_EFFECT_SERVICE(s), manager = DINGOSDK_EFFECT_DECAL_MANAGER(base);
        NoBailSkater skater;
        const bool playing = ready && !s.suspended && !noclip && !editor && !replaying(base) && no_bail_skater(skater);
        const auto world = pointer(client + fx::client_world);
        // The transition hook releases before assets unload. A missed world
        // change still releases particles, but decals also require their realm.
        if (!playing || current != s.service || manager != s.manager || world != s.world || skater.entity != s.entity)
            reset(s);
        s.service = current; s.manager = manager; s.world = world; s.entity = skater.entity;
        s.allowed = playing && world && current && manager;
        if (!s.allowed) return;
        s.receivers.begin(world);
        const auto now = GetTickCount64();
        for (auto it = s.entries.begin(); it != s.entries.end();) {
            auto& e = it->second;
            Matrix pose{};
            if (e.retired || now >= e.expires || (e.receiver[0] && !s.receivers.pose(e.receiver, pose))) {
                destroy(s, e); it = s.entries.erase(it); continue;
            }
            if (e.receiver[0]) {
                const auto transform = surface::world(pose, e.local);
                if (surface::changed(transform, e.drawn)) {
                    decal::set_transform(base, manager, e.decal.id, transform); e.drawn = transform;
                }
            }
            ++it;
        }
        if (now >= s.budget_at) {
            const auto count = std::count_if(s.entries.begin(), s.entries.end(), [](const auto& e) { return e.second.decal.id != 0; });
            (void)s.budget.update(base, static_cast<std::uint32_t>(count)); s.budget_at = now + 1000;
        }
    } catch (...) {}
}
void before_level_transition(unsigned next) noexcept {
    auto& s = state();
    if (next == 3 || next == 14 || next == 22 || next == 24 || next == 25) {
        s.suspended = true;
        try { std::lock_guard lock(s.mutex); reset(s); } catch (...) {}
    } else if (next == 13 || next == 21) s.suspended = false;
}
std::uint64_t create(std::string_view plugin, const plugins::EffectRequest& r) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (!usable(s) || !plugins::private_effect_asset(r.asset, r.decal) || !plugins::valid_effect_matrix(r.transform) ||
        !(r.lift >= 0 && r.lift <= 1)) return 0;
    const auto total = std::count_if(s.entries.begin(), s.entries.end(), [&](const auto& e) {
        return (e.second.decal.id != 0) == r.decal;
    });
    if (total >= (r.decal ? 4096 : 64)) return 0;
    const auto mine = std::count_if(s.entries.begin(), s.entries.end(), [&](const auto& e) {
        return e.second.owner == plugin && (e.second.decal.id != 0) == r.decal;
    });
    if (mine >= (r.decal ? 2048 : 24)) return 0;
    const auto found = DINGOSDK_EFFECT_ASSET(s, r.asset);
    if (!asset_is_resident(found) || !asset_type_matches(found, r.decal)) return 0;
    Entry e; e.owner = plugin; e.expires = GetTickCount64() + 600000;
    e.drawn = r.transform;
    if (r.decal) {
        e.decal_realm = decal::client_realm(s.manager);
        if (!e.decal_realm) return 0;
        if (r.attach) {
            Receiver receiver;
            if (!s.receivers.bind(r.transform, r.lift, receiver)) return 0;
            e.receiver = receiver.id; e.local = surface::local(receiver.pose, r.transform);
        }
        e.decal = DINGOSDK_EFFECT_DECAL_CREATE(s.base, s.manager, found, r.transform);
        if (!e.decal.id) return 0;
    } else {
        alignas(16) std::array<std::byte, fx::descriptor_size> descriptor{};
        alignas(16) const auto transform = r.transform;
        function<void(*)(void*, std::uintptr_t, const float*)>(s, fx::descriptor_init.rva)(descriptor.data(), found, transform.data());
        std::memcpy(descriptor.data() + fx::descriptor_flags, &fx::descriptor_flags_value, sizeof(fx::descriptor_flags_value));
        function<void(*)(std::uintptr_t, std::uintptr_t*, void*)>(s, fx::create.rva)(0, &e.particle, descriptor.data());
        function<void(*)(void*)>(s, fx::descriptor_destroy.rva)(descriptor.data());
        if (!e.particle) return 0;
    }
    const auto id = ++s.serial;
    s.entries.emplace(id, std::move(e)); return id;
}
void move(std::string_view plugin, std::uint64_t id, const Matrix& matrix) {
    if (!plugins::valid_effect_matrix(matrix)) return;
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (auto* e = owned(s, plugin, id)) {
        if (e->particle) { alignas(16) const auto m = matrix; function<void(*)(std::uintptr_t*, const float*)>(s, fx::move.rva)(&e->particle, m.data()); }
        else if (!e->receiver[0]) decal::set_transform(s.base, s.manager, e->decal.id, matrix);
    }
}
void stop(std::string_view plugin, std::uint64_t id) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (auto* e = owned(s, plugin, id); e && e->particle && !e->stopped) {
        function<void(*)(std::uintptr_t*, bool, float)>(s, fx::stop.rva)(&e->particle, false, 0); e->stopped = true;
    }
}
void opacity(std::string_view plugin, std::uint64_t id, float value) {
    if (!(value >= 0 && value <= 1)) return;
    auto& s = state(); std::lock_guard lock(s.mutex);
    if (auto* e = owned(s, plugin, id); e && e->decal.id) decal::set_opacity(s.base, s.manager, e->decal.id, value);
}
void release(std::string_view plugin, std::uint64_t id) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto it = s.entries.find(id);
    if (it == s.entries.end() || it->second.owner != plugin) return;
    if (s.thread != GetCurrentThreadId()) { it->second.retired = true; return; }
    if (s.valid) destroy(s, it->second);
    s.entries.erase(it);
}
std::vector<std::uint64_t> live(std::string_view plugin) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    std::vector<std::uint64_t> result;
    for (const auto& [id, e] : s.entries)
        if (!e.retired && e.owner == plugin) result.push_back(id);
    return result;
}
std::pair<bool, std::uint64_t> status() {
    auto& s = state(); std::lock_guard lock(s.mutex); return {usable(s), s.epoch};
}
void clear(std::string_view plugin) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        if (it->second.owner != plugin) { ++it; continue; }
        if (s.thread != GetCurrentThreadId()) { it->second.retired = true; ++it; continue; }
        if (s.valid) destroy(s, it->second);
        it = s.entries.erase(it);
    }
    // Reconcile the allowance in tick, once per second. A create/clear loop
    // must not resize five native render buffers on every script callback.
}
}
