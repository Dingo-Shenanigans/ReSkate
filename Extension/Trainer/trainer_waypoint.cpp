#include "trainer_waypoint.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include <cmath>
#include <format>
#include <map>

// The pause map keeps every point of interest in one registry (the map POI manager, the same
// one ReSkate's party markers register into: native_party_hooks.cpp update_map_markers). A
// waypoint the player places with the map cursor is a POI whose kind (MapObjectData @404) is
// DingoMapPOIType_Waypoint (3); its world transform is the matrix at @0.
namespace dingosdk::trainer {
namespace {
namespace party = addr::native_party;
using Address = std::uintptr_t;
using Handle = std::uint64_t;
constexpr std::uint32_t kind_waypoint = 3;
constexpr std::uint32_t kind_offset = 404;
constexpr std::uint32_t max_buckets = 65536, max_entries = 20000;

template<class T> bool get(Address address, T &value) { return address && memory::peek(address, value); }

Address model_type(Address base, Address manager, Handle handle) {
    const auto record = reinterpret_cast<Address (*)(Address, Handle, std::uint8_t)>(base + party::model_record)(manager, handle, 0);
    if (!record) return 0;
    return reinterpret_cast<Address (*)(Address, Handle, Address, std::uint8_t)>(base + party::model_type)(manager, handle, record, 0);
}

bool sane(const std::array<float, 16> &world) {
    for (const auto i : {12, 13, 14})
        if (!std::isfinite(world[i]) || std::abs(world[i]) > 1e6f) return false;
    return true;
}

MapWaypointRead walk(Address base) {
    MapWaypointRead out;
    Address map{}, vtable{}, manager{};
    if (!get(base + party::map_manager, map) || !map) { out.detail = "no map POI manager"; return out; }
    if (!get(map, vtable) || vtable != base + party::map_manager_vtable) { out.detail = "map POI manager type differs"; return out; }
    if (!get(map + 0x18, manager) || !manager) { out.detail = "no map model manager"; return out; }
    auto &n = game::native_data().models;
    if (!n.value || !n.lock || !n.unlock) { out.detail = "native model API unavailable"; return out; }
    game::ModelWriteLock lock(manager);
    Address table{};
    std::uint32_t buckets{}, count{};
    if (!get(map + 0x48, table) || !get(map + 0x50, buckets) || !get(map + 0x54, count) || !table ||
        buckets > max_buckets || count > max_entries) { out.detail = "map POI registry unreadable"; return out; }
    const auto poi_type = base + party::map_poi_type;
    std::map<std::uint32_t, unsigned> kinds;
    unsigned visited{};
    for (std::uint32_t i = 0; i < buckets && visited < count; ++i) {
        Address node{};
        if (!get(table + i * 8, node)) break;
        while (node && visited++ < count) {
            Handle handle{};
            if (!get(node, handle)) break;
            if (handle && model_type(base, manager, handle) == poi_type) {
                if (const auto value = n.value(manager, handle, 0, 0)) {
                    std::uint32_t kind{};
                    if (get(value + kind_offset, kind)) {
                        ++kinds[kind];
                        ++out.reading.typed;
                        std::array<float, 16> world{};
                        if (kind == kind_waypoint && !out.reading.waypoint && get(value, world) && sane(world))
                            out.reading.waypoint = landing::Vec3{world[12], world[13], world[14]};
                    }
                }
            }
            if (!get(node + 8, node)) break;
        }
    }
    out.reading.read = true;
    out.detail = std::format("{} map POIs", visited);
    for (const auto &[kind, number] : kinds) out.detail += std::format(", kind {} x{}", kind, number);
    return out;
}
} // namespace

MapWaypointRead read_map_waypoint(std::uintptr_t base) noexcept {
    if (!base) return {};
    try {
        return walk(base);
    } catch (const std::exception &e) {
        return MapWaypointRead{.reading = {}, .detail = e.what()};
    } catch (...) {
        return MapWaypointRead{.reading = {}, .detail = "map POI walk failed"};
    }
}
} // namespace dingosdk::trainer
