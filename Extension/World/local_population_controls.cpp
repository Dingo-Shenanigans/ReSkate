#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/traffic_vehicle_provider.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_population_controls.h"
#include "local_world_controls.h"
#include "Engine/Game/Build/20260929/population.h"

namespace dingosdk::profile_runtime {
PopulationRuntime& population_runtime() { static auto* r = new PopulationRuntime; return *r; }

bool population_identity(std::uintptr_t manager, int realm) {
    std::uintptr_t vt{}, global{}; int actual{};
    // Every population tick: the game's own manager global first, then that live object, peeked.
    return realm >= 0 && realm <= 1 && manager &&
        memory::peek(local_runtime().base + addr::population::managers + realm * 8, global) && global == manager &&
        memory::peek(manager, vt) && vt == local_runtime().base + addr::population::manager_vtable &&
        memory::peek(manager + 8, actual) && actual == realm;
}

bool population_config_identity(std::uintptr_t config) {
    std::uintptr_t type{}; std::string name;
    // The validated manager supplies a TrafficSystemConfig, but the asset name
    // varies by map (BAM uses Gameplay/AI/BAM_TrafficSystemConfig). The exact
    // native class identifies the field layout; the default asset name does not.
    return memory::peek(config + 8, type) && type == local_runtime().base + addr::population::traffic_system_config_type &&
        identifier(reinterpret_cast<void*>(config + 0x18), name);
}

// Every population tick, on the validated live config: peeked.
bool population_value(std::uintptr_t config, unsigned i, float& out) {
    if (i == 0 || i == 3) return memory::peek(config + population_offsets[i], out) && std::isfinite(out) && out >= 0 && out <= 100;
    std::uint32_t v{}; if (!memory::peek(config + population_offsets[i], v) || v > 1000) return false; out = static_cast<float>(v); return true;
}

bool population_write(std::uintptr_t config, unsigned i, float value) {
    const auto integer = static_cast<std::uint32_t>(value);
    const void* data = i == 0 || i == 3 ? static_cast<const void*>(&value) : &integer;
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(config + population_offsets[i]), data, 4, &written) && written == 4;
}

void population_release_config(std::uintptr_t manager) {
    auto& p = population_runtime();
    const auto it = p.nodes.find(manager); if (it == p.nodes.end()) return;
    const auto config = it->second.config; it->second.config = 0;
    if (!config || std::any_of(p.nodes.begin(), p.nodes.end(), [&](const auto& n) { return n.second.config == config; })) return;
    const auto saved = p.configs.find(config);
    if (saved != p.configs.end() && population_config_identity(config)) {
        for (unsigned i = 0; i < population_offsets.size(); ++i) {
            float current{}; const auto& f = saved->second.fields[i];
            if (f.owned && population_value(config, i, current) && current == f.applied) population_write(config, i, f.original);
        }
    }
    p.configs.erase(config);
}

std::uintptr_t population_construct(std::uintptr_t manager, int realm, std::uintptr_t allocator) {
    if (realm == 1) car_grab_native::invalidate();
    auto& p = population_runtime(); const auto result = p.construct(manager, realm, allocator);
    PreserveError preserve;
    try {
        std::lock_guard lock(world_control_runtime().mutex);
        if (population_identity(manager, realm)) p.nodes[manager] = PopulationNode{0,realm};
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"world_population_capture_failed\"}"); }
    return result;
}

std::uintptr_t population_destroy(std::uintptr_t manager, unsigned flags) {
    car_grab_native::invalidate_population(manager);
    auto& p = population_runtime();
    { PreserveError preserve; std::lock_guard lock(world_control_runtime().mutex);
      population_release_config(manager); p.nodes.erase(manager); }
    return p.destroy(manager, flags);
}

int population_choice(unsigned kind) {
    const auto& r = world_control_runtime();
    if (r.map == WorldMap::none || !r.population_active.load(std::memory_order_acquire)) return -1;
    return kind == 0 ? r.model.choices.traffic : r.model.choices.pedestrians;
}

void update_population_controls() {
    auto& r = world_control_runtime(); auto& p = population_runtime(); std::lock_guard lock(r.mutex);
    if (!r.population_active.load(std::memory_order_acquire)) return;
    for (auto& [manager, node] : p.nodes) {
        if (!population_identity(manager, node.realm)) continue;
        for (unsigned kind = 0; kind < 2; ++kind) {
            const auto choice = population_choice(kind);
            if (choice < 0 && !node.owned[kind]) continue;
            std::uint8_t current{};
            if (!read(manager + 0x150 + kind, current) || current > 1) continue;
            if (!node.owned[kind]) node.original[kind] = current != 0;
            const bool desired = choice < 0 ? node.original[kind] : node.realm == 1 || choice != 0;
            p.enable[kind](desired, node.realm);
            node.owned[kind] = choice >= 0;
        }
    }
    if (r.diagnostic_due && GetTickCount64() >= r.diagnostic_due) {
        r.diagnostic_due = 0;
        dingosdk::Json managers = dingosdk::Json::array();
        for (const auto& [manager, node] : p.nodes) {
            if (!population_identity(manager, node.realm)) continue;
            std::array<std::uint8_t,3> flags{}; std::uintptr_t config{};
            read(manager + 0x150, flags); read(manager + 0x108, config);
            config &= ~std::uintptr_t{4};
            std::string name;
            if (population_config_identity(config)) identifier(reinterpret_cast<void*>(config + 0x18), name);
            managers.push_back({{"realm",node.realm},{"flags",flags},{"config",config},{"asset",name}});
        }
        auto fields = dingosdk::Json::array();
        for (unsigned i=0;i<atmosphere_controls.size();++i) {
            const auto& control=atmosphere_controls[i];
            const auto& reading=r.model.atmosphere[i];
            const auto choice=r.model.choices.atmosphere.find(control.key);
            if (choice==r.model.choices.atmosphere.end()) continue;
            fields.push_back({{"control",control.key},{"requested",atmosphere_value_text(control,choice->second)},
                {"observed",reading.value ? atmosphere_value_text(control,*reading.value) : "unavailable"},
                {"varies",reading.varies},{"applied",reading.applied}});
        }
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event","world_controls_observed"},{"environments",r.model.environments},{"fields",fields},
            {"components",r.nodes.size()},{"population_ready",r.model.population_ready},
            {"population",r.model.population},{"managers",managers},{"status",r.model.status}}.dump().c_str());
    }
}
}