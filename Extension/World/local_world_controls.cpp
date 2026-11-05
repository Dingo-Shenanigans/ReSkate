#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_population_controls.h"
#include "visual_environment.h"
#include "local_world_controls.h"
#include "local_atmosphere_controls.h"
#include "Engine/Game/Build/20260929/atmosphere.h"

namespace dingosdk::profile_runtime {
// Exact retail component ABI and spawn consumers.

// Component kinds: fog, sky, wind. Offsets refer to live native component caches.

WorldControlRuntime& world_control_runtime() { static auto* r = new WorldControlRuntime; return *r; }

bool environment_identity(const EnvironmentNode& n) {
    const auto base = local_runtime().base;
    std::uintptr_t vt{}, data{}, type{}, collection{}, parent{};
    return read(n.component, vt) && vt == base + environment_vtables[n.kind] &&
        read(n.component + 8, data) && data == n.data && read(data + 8, type) && type == base + environment_types[n.kind] &&
        read(n.component + 0x18, collection) && read(collection, parent) && parent == n.parent;
}

float environment_choice(unsigned field, const WorldControls& c, float original, const EnvironmentNode& n) {
    switch (field) {
    case 0: return static_cast<float>(c.fog);
    case 1: case 8: case 9: case 10: case 11:
        return c.fog_distance < 0 ? -1 : original * c.fog_distance;
    case 2: case 12: case 13: {
        // SkyType_Physical (2) lights its atmosphere from its light sources;
        // LuminanceScale belongs to procedural/HDRI sky normalization.
        std::uint32_t sky_type{};
        if (!read(n.component + 0x1e8, sky_type) || sky_type > 2 ||
            (field == 2 ? sky_type == 2 : sky_type != 2)) return -1;
        return c.sky_brightness < 0 ? -1 : original * c.sky_brightness;
    }
    case 3: case 4: return c.clouds < 0 ? -1 : original * c.clouds;
    case 5: return c.wind_strength;
    case 6: return c.wind_direction;
    case 7: return c.wind_strength < 0 ? -1 : c.wind_strength == 0 ? 0 : original;
    default: return -1;
    }
}

void update_environment_controls(WorldMap map) {
    auto& r = world_control_runtime(); auto& environments = visual_environments();
    // Parent lifetime lock always precedes the component lock. Factory/destructor
    // hooks never take a profile/model/parent lock, including during native calls.
    std::lock_guard parents(environments.mutex);
    std::lock_guard lock(r.mutex);
    if (r.map != map) { r.map = map; r.last_update = 0; r.diagnostic_due = GetTickCount64() + 6000; }
    if (!r.environment_active.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (!r.thread) r.thread = GetCurrentThreadId();
    if (r.thread != GetCurrentThreadId()) return;
    if (now - r.last_update < 250) return;
    r.last_update = now;
    r.model.environments = {};
    r.model.atmosphere = {};
    r.model.textures = {};
    r.textures.clear();
    for (const auto& [_, n] : r.nodes) {
        const auto parent=environments.nodes.find(n.parent);
        if (parent!=environments.nodes.end() && parent->second.map==map &&
            visual_environment_identity(parent->second,false) && environment_identity(n)) collect_atmosphere_textures(n);
    }
    bool ok = true;
    for (auto& [_, n] : r.nodes) {
        const auto parent = environments.nodes.find(n.parent);
        if (parent == environments.nodes.end() || !visual_environment_identity(parent->second, false) || !environment_identity(n)) continue;
        std::uint16_t handle{};
        if (!read(n.component + 0x30, handle) || !handle) continue;
        const bool selected = map != WorldMap::none && parent->second.map == map;
        if (selected) ++r.model.environments[n.kind];
        ok &= apply_atmosphere_controls(n, selected ? r.model.choices : WorldControls{}, selected);
    }
    r.model.status = !ok ? "Some environment controls could not be applied." :
        map == WorldMap::none ? "Load a level to use world controls." :
        r.model.environments[0] && r.model.environments[1] && r.model.environments[2] ?
        "Environment controls active." : "Waiting for the level's environment components.";
}

void start_world_controls() noexcept {
    start_visual_environments();
    auto& r = world_control_runtime(); auto& p = population_runtime(); const auto base = local_runtime().base;
    std::vector<void*> created;
    try {
        r.model.choices = profile::world_controls(*local_runtime().store->shared_snapshot());
        for (const auto& site : world_control_sites) {
            std::array<unsigned char, 32> bytes{};
            if (!read(base + site.rva, bytes) || bytes != site.bytes) throw std::runtime_error("World controls fingerprint mismatch");
        }
        for (const auto& site : addr::atmosphere::atmosphere_sites) {
            std::array<unsigned char,32> bytes{};
            if (!read(base+site.rva,bytes) || bytes!=site.bytes) throw std::runtime_error("Atmosphere property fingerprint mismatch");
        }
        for (unsigned i = 0; i < 3; ++i) {
            std::uintptr_t property{}, destroy{};
            if (!read(base + environment_vtables[i] + 0x18, property) || property != base + world_control_sites[i * 3 + 2].rva ||
                !read(base + environment_vtables[i] + 8, destroy) || destroy != base + world_control_sites[i * 3 + 1].rva)
                throw std::runtime_error("World environment vtable mismatch");
            r.property[i] = reinterpret_cast<WorldControlRuntime::Property>(property);
        }
        const auto hook = [&](unsigned i, auto detour, auto& original) {
            auto* target = reinterpret_cast<void*>(base + world_control_sites[i].rva);
            if (hook_prepare(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != HookOk)
                throw std::runtime_error("Cannot prepare world controls");
            created.push_back(target);
        };
        hook(0, &environment_construct<0>, r.construct[0]); hook(1, &environment_destroy<0>, r.destroy[0]);
        hook(3, &environment_construct<1>, r.construct[1]); hook(4, &environment_destroy<1>, r.destroy[1]);
        hook(6, &environment_construct<2>, r.construct[2]); hook(7, &environment_destroy<2>, r.destroy[2]);
        hook(9, &population_construct, p.construct); hook(10, &population_destroy, p.destroy);
        hook(11, &population_tick<0>, p.tick[0]); hook(12, &population_tick<1>, p.tick[1]);
        hook(13, &population_enable<0>, p.enable[0]); hook(14, &population_enable<1>, p.enable[1]);
        for (auto* target : created)
            if (hook_enable(target) != HookOk) throw std::runtime_error("Cannot enable world controls");
        r.environment_active.store(true, std::memory_order_release);
        r.population_active.store(true, std::memory_order_release);
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"world_controls_initialized\",\"active\":true}");
    } catch (...) {
        for (auto* target : created) hook_disable(target);
        r.model.status = "World controls are unavailable in this build.";
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"world_controls_initialized\",\"active\":false}");
    }
}
}