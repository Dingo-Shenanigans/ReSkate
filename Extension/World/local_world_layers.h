#pragma once
#include "Extension/Profile/runtime_internal.h"
#include "subworld_loading_progress.h"

namespace dingosdk::profile_runtime {
// Catalog views: node bundles, parents and autoload flags; each row's leaf and switch node.
inline const std::string& seasonal_bundle(unsigned slot) { return world_layer_nodes()[slot].bundle; }
inline int world_parent(int slot) { return world_layer_nodes()[static_cast<std::size_t>(slot)].parent; }
inline bool world_autoload(unsigned slot) { return world_layer_nodes()[slot].autoload; }
inline unsigned world_leaf(unsigned row) { return world_layers()[row].leaf; }
inline unsigned world_switch(unsigned row) { return world_layers()[row].switch_slot; }

bool world_layer_uses_slot(unsigned choice, unsigned slot);

struct SeasonalEvent { std::uintptr_t vtable; std::uint32_t id, flags{}; };

static_assert(sizeof(SeasonalEvent) == 16);

struct SeasonalNode {
    std::uintptr_t entity{}, data{}, context{};
    std::uint64_t generation{};
    unsigned slot{};
    std::optional<bool> pending;
    std::uint64_t requested_at{};
};

struct SubworldLogRow {
    SeasonalNode identity;
    std::string bundle;
    subworld_logging::Progress progress;
};

struct WorldLayersRuntime {
    using Construct = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
    using Destroy = std::uintptr_t (*)(std::uintptr_t, unsigned);
    using Event = void (*)(std::uintptr_t, const SeasonalEvent*);
    Construct construct{};
    Destroy destroy{};
    Event event{};
    Event original_event{};
    std::atomic<bool> active{};
    std::atomic<WorldMap> map_hint{WorldMap::none};
    // The active custom map's own time of day (LevelInfo::time_of_day), 0 for none.
    std::atomic<unsigned> map_time{};
    // Constructors/destructors only use this lock, never native_mutex. Hold it
    // across dispatch so a captured reference cannot begin destruction mid-call.
    std::recursive_mutex lifetime_mutex;
    std::map<std::uintptr_t, SeasonalNode> nodes;
    // Observation-only registry. This includes references outside the editable
    // seasonal catalog and never expands the set of SDK-controlled layers.
    std::map<std::uintptr_t, SubworldLogRow> subworld_logs;
    std::uintptr_t streaming_cursor{};
    std::uint64_t streaming_generation{}, streaming_capacity_drops{};
    std::uint64_t generation{}, root_generation{}, next_poll{}, next_streaming_poll{};
    std::uintptr_t context{};
    std::vector<std::optional<bool>> original = std::vector<std::optional<bool>>(world_layer_nodes().size());
    std::vector<bool> managed = std::vector<bool>(world_layer_nodes().size());
    WorldLayersModel model;
    WorldLayerOverride lobby_override;
};

WorldLayersRuntime& world_layers_runtime();

bool seasonal_name_equal(std::string_view a, std::string_view b);

void capture_seasonal_node(std::uintptr_t entity);

std::uintptr_t seasonal_construct_hook(std::uintptr_t entity, std::uintptr_t info, std::uintptr_t data);

std::uintptr_t seasonal_destroy_hook(std::uintptr_t entity, unsigned flags);

void reset_world_layer_session();

struct SeasonalState { bool requested{}, loaded{}; };

bool seasonal_state(const SeasonalNode& node, SeasonalState& state);

bool ensure_seasonal_state(std::uintptr_t entity, bool wanted, std::uint64_t now,
                           bool& issued, std::string& status);

void update_world_layers(std::uintptr_t manager, std::uint64_t now);

void start_world_layers() noexcept;
}
