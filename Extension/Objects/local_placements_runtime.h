#pragma once
#include "Extension/Profile/runtime_internal.h"
#include "placement_create_queue.h"

namespace dingosdk::profile_runtime {
struct PlacementApplied { std::uint64_t entity{}; std::array<std::uint32_t, 20> words{}; };

struct PlacementBatch {
    std::uint32_t type{};
    std::vector<PlacementApplied> applied;
    std::uint32_t network_source{};
    bool native{}; // Dispatched by the game's own placement messages, not ReSkate.
};

extern thread_local PlacementBatch* placement_batch;

struct PlacementRow {
    std::uint64_t token{}, entity{}, saved_id{};
    profile::PlacedObject object;
    bool spawned{};
    std::uint8_t missing_polls{};
};

// Private scripted requests can wait across a lobby/world change.
// Never let an old restore or editor preview enter the next layout's autosave.
class PlacementCreationScope {
    std::uint32_t source_{0xd2000001};
public:
    static bool tagged(std::uint32_t source) { return (source & 0xff000000) == 0xd2000000; }
    std::uint32_t source() const { return source_; }
    bool accepts(std::uint32_t source) const { return source_ && source == source_; }
    void reset() { source_ = source_ && source_ < 0xd2ffffff ? source_ + 1 : 0; }
};

struct PlacementsRuntime {
    std::uintptr_t (*construct)(std::uintptr_t, std::uintptr_t){};
    std::uintptr_t (*destroy)(std::uintptr_t, unsigned){};
    void* (*resolve)(void*, std::uint64_t, bool){};
    bool (*valid)(const void*){};
    void* (*query)(void*, std::uint64_t){};
    float* (*pose)(float*, const void*){};
    void* (*context)(void*){};
    void* (*delete_ctor)(void*){};
    void (*message_destroy)(void*){};
    void (*send)(std::uintptr_t, const void*){};
    std::uint32_t (*teleport)(std::uintptr_t, const float*, unsigned, const void*){};
    void* (*transition_ctor)(void*){};
    void (*transition_destroy)(void*){};
    std::unique_ptr<profile::PlacementStore> store;
    profile::PlacementSnapshot document;
    // Guests edit an in-memory session layout. Their disk-backed layout is
    // parked here until they leave, and is never included in network capture.
    std::optional<profile::PlacementSnapshot> personal_document;
    PlacementCreationScope creation;
    PlacementCreateQueue creates;
    bool creates_enabled{true};
    void (*server_create)(std::uintptr_t, const void*, std::uint8_t, std::uintptr_t, std::uint8_t){};
    // Trampolines of the hooked native placement message handler and recipe apply.
    void (*message)(std::uintptr_t, const void*){};
    void (*apply)(const void*, const void*){};
    std::uintptr_t manager{}, map_context{};
    std::string map;
    std::map<std::uint64_t, std::uint64_t> tracked; // live entity -> layout ID
    std::vector<std::uint64_t> restore;
    std::vector<profile::PlacedObject> late_restores;
    // Restored objects saved at another scale. The create recipe has no scale, so they spawn
    // at 1x; the server tick resizes them, and the pose poll leaves them alone until then
    // (`resized_at`, 0 while waiting) and for a moment after, so 1x is never saved over them.
    struct PendingResize {
        std::uint64_t entity{};
        profile::PlacedObject object;
        std::uint64_t resized_at{};
        bool settled{}; // one server tick after the spawn, as the editor waits before moving
    };
    std::vector<PendingResize> resizes;
    std::vector<PlacementRow> rows;
    std::uint64_t next_token{1}, teleport_token{}, teleport_queued_at{};
    // A teleport of the local skater to a position (console tp, the host's tpall).
    std::optional<std::array<float, 3>> position_teleport;
    std::uint64_t position_teleport_at{};
    std::optional<profile::PlacedObject> inflight;
    std::uint64_t sent_at{}, next_poll{}, map_since{}, map_generation{}, next_id{1};
    bool failed{}, restore_failed{};
    bool enabled{true}, clearing{}, clear_sent{};
    std::string status, clear_map, clear_wait_map;
    std::set<std::uint64_t> clear_entities;
    std::atomic<bool> clear_saved{};
    profile::ObjectLayout clear_wait_layout;
    std::mutex save_mutex;
    std::condition_variable save_wake;
    std::map<std::string, profile::ObjectLayout, std::less<>> pending;
    std::atomic<bool> save_failed{};
    std::jthread writer;
};

PlacementsRuntime& placements_runtime();

inline void queue_placement_layout(PlacementsRuntime& r) {
    r.next_id = 1;
    if (r.map.empty()) return;
    for (const auto& object : r.document.maps[r.map]) {
        if (object.id == UINT64_MAX) { r.failed = true; return; }
        r.next_id = (std::max)(r.next_id, object.id + 1);
        r.restore.push_back(object.id);
        r.rows.push_back({r.next_token++, 0, object.id, object, false});
    }
}

void queue_placement_save();

void placement_reset_session();

bool placement_session_ready();

bool placement_same_pose(const profile::PlacedObject& a, const profile::PlacedObject& b);

inline bool placement_matches_applied(const profile::PlacedObject& expected, profile::PlacedObject& applied) {
    // Applied recipes have position/quaternion but no scale lane. Match only
    // those observable fields and restore the request's durable identity/scale.
    if (expected.item != applied.item) return false;
    float distance{}, dot{};
    for (unsigned i = 0; i < 3; ++i)
        distance += (expected.position[i] - applied.position[i]) *
                    (expected.position[i] - applied.position[i]);
    for (unsigned i = 0; i < 4; ++i) dot += expected.rotation[i] * applied.rotation[i];
    if (distance >= .0001f || std::abs(dot) <= .99999f) return false;
    applied.id = expected.id;
    applied.scale = expected.scale;
    return true;
}

std::optional<profile::PlacedObject> placement_from_applied(const PlacementApplied& applied);

std::optional<std::set<std::uint64_t>> placement_live_entities(std::uintptr_t manager);

void observe_placement(std::uint64_t entity, const profile::PlacedObject& object);

void reconcile_placement_rows(const std::set<std::uint64_t>& live);

void placement_finish_batch(std::uintptr_t manager, const PlacementBatch& batch);

// One row of the placement manager's live table.
struct PlacementId {
    std::uint32_t id{};
    std::uint64_t entity{};
};
// The placement manager's live {id, entity} table, or nothing when it cannot be
// read whole and consistent.
std::optional<std::vector<PlacementId>> placement_manager_ids(std::uintptr_t manager);

void placement_message_hook(std::uintptr_t manager, const void* message);

void placement_apply_hook(const void* entity, const void* recipe);

std::uintptr_t placement_construct_hook(std::uintptr_t manager, std::uintptr_t allocator);

std::uintptr_t placement_destroy_hook(std::uintptr_t manager, unsigned flags);

void update_placement_poses();

bool queue_placement_create(const profile::PlacedObject& object, std::uint32_t item, std::uint32_t token);
void update_placement_creates();

struct alignas(16) PlacementRequest {
    std::array<std::uint32_t, 4> header{0, 0, 1, 1}; // capacity/count at data-8/-4
    std::array<float, 16> transform{};
    std::uint32_t variant{}, item{};
    std::uint8_t flags{255};
    std::array<std::byte, 7> padding{};
};

static_assert(offsetof(PlacementRequest, transform) == 16 && sizeof(PlacementRequest) == 96);

std::uintptr_t placement_client_channel();

void send_placement_delete(std::uintptr_t channel, const std::vector<std::uint32_t>& ids);

void update_placement_clear();

bool begin_placement_delete(const std::set<std::uint64_t>& tokens);

std::array<float, 16> placement_teleport_transform(const profile::PlacedObject& object);

std::uint32_t send_placement_teleport(std::uintptr_t manager, const profile::PlacedObject& object);

void update_placement_teleport();
// Sends a queued position teleport once the native teleport manager is idle. Needs no
// placement session, so it works on every map.
void update_position_teleport();

void update_placement_restore();

void initialize_placement_store(const std::filesystem::path& profile_path);
}
