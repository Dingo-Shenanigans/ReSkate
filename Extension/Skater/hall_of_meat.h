#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Hall of Meat: the local skater's bail impacts, as the game's own cause
// collector and per-bone contact report report them (contracts in
// Engine/Game/Build/20260929/no_bail.h). The bail hooks feed observe_cause and
// observe_wipeout from the game's threads; the menu reads recent().
namespace dingosdk::hall_of_meat {

// One recorded bail cause: the game's reason code and its impact magnitude.
struct Impact {
    std::int32_t reason{};
    float magnitude{};
};
// The game tracks contact for fourteen configured body bones, exported with
// every bail to contacts+0xfb0..0xfbd (one 0/1 flag each). Which index is
// which bone is calibration, not yet mapped.
inline constexpr std::size_t bone_contact_count = 14;

// One body impact: the physics bone that took it (ids index
// physics_bone_names) and where it happened, when the record carries one.
struct BoneHit {
    std::uint32_t bone_id{};
    bool has_position{};
    std::array<float, 3> position{};
};
inline constexpr std::size_t max_bone_hits = 20;
// The game's physics bone names, indexed by physics bone id (0-based; the
// name pool in the binary, SKATEBOARD_ROOT first).
inline constexpr std::array<const char*, 30> physics_bone_names{
    "SKATEBOARD_ROOT", "NECK1", "NECK", "LEFTHAND", "LEFTFOREARM", "LEFTARM",
    "LEFTSHOULDER", "RIGHTHAND", "RIGHTFOREARM", "RIGHTARM", "RIGHTSHOULDER",
    "SPINE3", "SPINE2", "SPINE1", "SPINE", "LEFTTOEBASE", "LEFTFOOT",
    "LEFTLEG", "LEFTUPLEG", "RIGHTTOEBASE", "RIGHTFOOT", "RIGHTLEG",
    "RIGHTUPLEG", "HIPS", "TRUCK_BACK", "TRUCK_FRONT", "LEFT_WHEELFRONT",
    "RIGHT_WHEELFRONT", "LEFT_WHEELBACK", "RIGHT_WHEELBACK"};
inline constexpr const char* physics_bone_name(std::uint32_t id) noexcept {
    return id < physics_bone_names.size() ? physics_bone_names[id] : "UNKNOWN";
}

// A wipeout: the causes recorded around it and the per-bone contact flags
// read in the same step.
struct Bail {
    std::uint64_t at{};                // GetTickCount64() of the wipeout
    float magnitude{};                 // the pending impacts' magnitudes, added up
    bool body_contact{};               // any of the fourteen bones reported contact
    std::array<std::uint8_t, bone_contact_count> bone_contacts{}; // 0/1 per bone
    std::array<Impact, 12> impacts{};  // arrival order, oldest first
    std::size_t impact_count{};
    std::array<BoneHit, max_bone_hits> bone_hits{}; // the wipeout's body impacts
    std::size_t bone_hit_count{};
};

// Feed from the bail hooks, local skater only. noexcept: nothing allocates,
// and a cause that does not fit is dropped rather than reported.
void observe_cause(std::int32_t reason, float magnitude) noexcept;
// Snapshot the pending causes when the wipeout flag arrives for the local
// skater; bone_contacts are the game's per-bone flags read in the same step.
// True when this opened a new bail — the ragdoll's follow-up wipeout steps are
// folded into the bail just recorded. `recorded` receives the new bail when
// non-null.
bool observe_wipeout(const std::array<std::uint8_t, bone_contact_count>& bone_contacts,
    const BoneHit* bone_hit_data, std::size_t bone_hit_data_count,
    Bail* recorded = nullptr) noexcept;
// The kept bails, newest first. Bounded; the menu thread may allocate.
std::vector<Bail> recent();
// The menu's Clear button: drop the history and any pending causes.
void forget() noexcept;
}
