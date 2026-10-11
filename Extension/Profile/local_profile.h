#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Engine/Core/Json/json.h"
#include "Extension/Customization/local_customization.h"
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Game/World/world_layers.h"
#include "Engine/Game/World/world_controls.h"
#include "Engine/Game/Rendering/graphics_controls.h"
#include "Engine/Game/Input/controller_bindings.h"

namespace dingosdk::storage { class SaveDatabase; }

namespace dingosdk::profile {
// ReSkate-owned policy keys, not names claimed to exist in the game service.
inline constexpr std::string_view unlock_neighborhoods_option = "ReSkate.UnlockNeighborhoods";
inline constexpr std::string_view unlock_preset_slots_option = "ReSkate.UnlockPresetSlots";
inline constexpr std::string_view max_neighborhood_ranks_option = "ReSkate.MaxNeighborhoodRanks";
inline constexpr std::string_view unlock_bus_stops_option = "ReSkate.UnlockBusStops";
inline constexpr std::string_view unlock_cosmetics_option = "ReSkate.UnlockCosmetics";
inline constexpr std::string_view unlock_objects_option = "ReSkate.UnlockObjects";
inline constexpr std::string_view hide_challenges_option = "ReSkate.HideChallenges";
inline constexpr std::array<std::string_view, 4> neighborhood_ids{
    "neighbourhood_rank_entertainment", "neighbourhood_rank_financial",
    "neighbourhood_rank_historic", "neighbourhood_rank_stadium"};
inline constexpr std::array<std::string_view, 16> onboarding_ids{
    "Ent_Intro_Q01_Set", "Ent_Intro_Q02_Set", "Ent_Intro_Q02_LineChallenge", "StoreOnboardingSet", "CRASOnboardingSet",
    "QuestSelectionOnboardingSet", "FirstQuestCompletedOnboardingSet", "SecondQuestCompletedOnboardingSet",
    "RankUp_1_OnboardingSet", "RankUpOnboardingSet", "RankUnlockedOnboardingSet",
    "SteeringCourse", "OllieCourse", "GrindCourse", "FlipTrickCourse", "GoToCity"};
// Domain records mirror the client's decoded progression boundary. They are
// deliberately independent of the game's allocators, ECS handles and RPC ABI.
struct PlayEvent {
    std::string id, context;
    std::uint64_t timestamp{};
    std::int32_t count{};
    bool operator==(const PlayEvent&) const = default;
};
struct Snapshot {
    std::uint64_t revision{};
    std::uint32_t onboarding_seed{};
    std::uint32_t offline_rank_cap{};
    std::map<std::string, bool, std::less<>> bool_options;
    std::map<std::string, PlayEvent, std::less<>> play_events;
    std::map<std::string, std::int32_t, std::less<>> quests;
    std::map<std::string, bool, std::less<>> entitlements;
    // Numeric ranks survive restarts; the runtime resolves the native curve or
    // the explicit offline cap before durably applying the max-rank policy.
    std::map<std::string, std::uint32_t, std::less<>> neighborhood_ranks;
    dingosdk::Json customization = dingosdk::Json::object();
    dingosdk::Json settings = dingosdk::Json::object();
    // Preserve future extension fields across progress writes, including nested
    // unknown fields in progress. Known fields remain represented above.
    dingosdk::Json extensions = dingosdk::Json::object();
    bool operator==(const Snapshot&) const = default;
};
// Detached, narrow read models: UI polling never needs to copy inventories,
// settings, or the complete extensions document to inspect saved progress.
struct MissionState {
    std::uint64_t revision{};
    std::map<std::string, PlayEvent, std::less<>> play_events;
    std::map<std::string, std::int32_t, std::less<>> quests;
};
struct NeighborhoodState {
    bool unlocked{}, max_ranks{};
    std::optional<std::uint32_t> rank;
};
struct NewsPost {
    std::string id, title, description, body, small_image, large_image;
    std::string fallback_small, fallback_large;
};
struct NewsFeed {
    bool enabled{};
    std::vector<NewsPost> posts;
};
NewsFeed news_feed(const Snapshot&);
// A news section ({"enabled", "posts": [...]}) as validated for the Hub; throws on invalid input.
NewsFeed parse_news(const Json& section);
struct RipScore {
    std::int64_t value{}, cap{};
    std::int32_t level{};
};
std::optional<RipScore> rip_score(const Snapshot&);
struct ChallengeGoal {
    std::string id;
    bool optional{};
    std::string title_key, feed_key, short_description_key, description_key;
    bool operator==(const ChallengeGoal&) const = default;
};
struct ChallengeDefinition {
    std::string id, type, asset;
    bool available{};
    std::string title_key;
    std::vector<ChallengeGoal> goals;
    std::string neighborhood;
    std::string description_key; // the challenge's short description (ActivityData.Description)
};
struct ChallengePolicy {
    bool enabled{};
    std::map<std::string, ChallengeDefinition, std::less<>> catalog;
};
ChallengePolicy challenge_policy(const Snapshot&);
ParkChoices park_choices(const Snapshot&);
WorldLayerChoices world_layer_choices(const Snapshot&);
WorldControls world_controls(const Snapshot&);
GraphicsControls graphics_controls(const Snapshot&);
std::uint32_t freecam_controller_binding(const Snapshot&);
bool freecam_controller(const Snapshot&);
std::uint32_t freecam_binding(const Snapshot&);
std::uint32_t tp_to_freecam_binding(const Snapshot&);
std::uint32_t vote_yes_binding(const Snapshot&);
// One of action_binds, by its key; 0: not bound.
std::uint32_t action_binding(const Snapshot&, std::string_view key);
std::uint32_t vote_no_binding(const Snapshot&);
std::uint32_t noclip_binding(const Snapshot&);
std::uint32_t forward_velocity_binding(const Snapshot&);
std::uint32_t up_velocity_binding(const Snapshot&);
std::uint32_t offboard_up_velocity_binding(const Snapshot&);
std::vector<std::string> challenge_completed_criteria(const Snapshot&, std::string_view);
std::string encode(const Snapshot&);
Snapshot decode(std::string_view);
void seed_completed_onboarding(Snapshot&, const Snapshot& defaults);
std::filesystem::path default_path();
std::filesystem::path defaults_path();
std::string_view embedded_defaults() noexcept;

class Store {
public:
    explicit Store(std::filesystem::path, const std::filesystem::path& defaults = defaults_path());
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Snapshot snapshot() const;
    // The same snapshot shared, copied once per revision: for readers that only look.
    // Profiled 2026-10-01: copying the whole profile on each of the 500 ms update passes was
    // much of their cost. Retained snapshots stay valid after writes.
    std::shared_ptr<const Snapshot> shared_snapshot() const;
    std::string export_json() const;
    void import_json(std::string_view);
    std::uint64_t revision() const;
    // Bumped whenever any Store's saved values may have changed (each commit, each Store made);
    // read without the lock, so caches on hot paths can tell whether a lookup is still current.
    static std::uint64_t changes() noexcept;
    // Same immutable object until a successful commit changes this Store's
    // revision. Retained readers remain valid after writes or Store destruction.
    std::shared_ptr<const MissionState> mission_state() const;
    NeighborhoodState neighborhood_state(std::string_view) const;
    std::uint32_t offline_rank_cap() const;
    std::optional<RipScore> saved_rip_score() const;
    std::vector<std::string> completed_challenge_criteria(std::string_view) const;
    std::optional<bool> bool_option(std::string_view) const;
    std::optional<dingosdk::Json> user_value(std::string_view) const;
    void set_user_value(std::string_view, const dingosdk::Json&);
    // Several user values in one transaction; each follows set_user_value's rules.
    void set_user_values(const std::vector<std::pair<std::string, dingosdk::Json>>&);
    std::optional<dingosdk::Json> native_profile_option(unsigned location, std::string_view key) const;
    void set_native_profile_option(unsigned location, std::string_view key, const dingosdk::Json&);
    std::optional<std::int32_t> quest_state(std::string_view) const;
    std::optional<bool> entitlement(std::string_view) const;
    PlayEvent record_play_event(std::string_view id);
    void set_bool_option(std::string_view, bool);
    void set_neighborhood_rank(std::string_view, std::uint32_t);
    // Coupled progression edits are committed together before UI acknowledgement.
    void set_bus_stop_state(unsigned number, unsigned state); // 0 hidden, 1 locked, 2 unlocked
    void save_rip_score(const RipScore&);
    void save_district_rank(std::string_view, std::uint32_t);
    void set_quest_state(std::string_view, std::int32_t);
    void set_onboarding_completed(std::string_view, bool);
    std::optional<CosmeticLoadout> cosmetic_loadout(std::string_view) const;
    void save_cosmetic_loadout(std::string_view, const CosmeticLoadout&);
    std::optional<CosmeticLoadout> player_card() const;
    void save_player_card(const CosmeticLoadout&);
    void seed_cosmetic_inventory(const std::vector<std::string>&);
    void seed_object_inventory(const std::vector<std::string>&);
    void reconcile_inventory(const std::vector<std::string>& cosmetics, const std::vector<std::string>& objects);
    std::uint32_t selected_cosmetic_preset() const;
    void set_selected_cosmetic_preset(std::uint32_t);
    void save_park_choice(unsigned lot, std::string_view id);
    void save_park_choices(const ParkChoices&);
    void save_world_controls(const WorldControls&);
    void save_graphics_controls(const GraphicsControls&);
    bool freecam_controller() const;
    void save_freecam_controller(bool);
    std::uint32_t freecam_controller_binding() const;
    void save_freecam_controller_binding(std::uint32_t);
    std::uint32_t freecam_binding() const;
    void save_freecam_binding(std::uint32_t);
    std::uint32_t tp_to_freecam_binding() const;
    void save_tp_to_freecam_binding(std::uint32_t);
    std::uint32_t vote_binding(bool yes) const;
    void save_vote_binding(bool yes, std::uint32_t);
    std::uint32_t action_binding(std::string_view key) const;
    void save_action_binding(std::string_view key, std::uint32_t);
    std::uint32_t noclip_binding() const;
    void save_noclip_binding(std::uint32_t);
    std::uint32_t forward_velocity_binding() const;
    void save_forward_velocity_binding(std::uint32_t);
    std::uint32_t up_velocity_binding() const;
    void save_up_velocity_binding(std::uint32_t);
    std::uint32_t offboard_up_velocity_binding() const;
    void save_offboard_up_velocity_binding(std::uint32_t);
    void save_world_layer_choice(unsigned layer, std::string_view mode);
    void restore_world_layers(WorldMap map);
    std::uint64_t begin_challenge(std::string_view id);
    bool remember_challenge_goals(std::string_view id, const std::vector<ChallengeGoal>& goals);
    // The receipt and completed goals share one durable transaction. Repeated
    // end callbacks for this attempt return the same receipt without rewards twice.
    std::uint64_t finish_challenge(std::string_view id, std::uint64_t attempt,
        const std::vector<std::string>& completed_criteria);
    const std::filesystem::path& path() const { return path_; }
private:
    class Update;
    void commit(Snapshot);
    // Advances changes(). Called by every save once value_ holds the new values and before mutex_
    // is released, so a reader that sees the new count never looks up an old value.
    static void note_change() noexcept;
    std::filesystem::path path_;
    std::unique_ptr<storage::SaveDatabase> database_;
    mutable std::mutex mutex_;
    Snapshot value_;
    mutable std::shared_ptr<const MissionState> mission_cache_;
    mutable std::shared_ptr<const Snapshot> snapshot_cache_;
};
}
