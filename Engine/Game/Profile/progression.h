#pragma once
#include "Engine/Game/World/bus_stop_world.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk {
struct BusStopRow {
    unsigned number{};
    FixedBusStopWorld world{FixedBusStopWorld::bam};
    bool visible{}, unlocked{};
};
struct ChallengeProgressRow {
    std::string id, type;
    bool available{};
    std::uint64_t attempts{};
    std::vector<std::string> completed_goals;
    std::size_t goal_count{};   // goals the challenge has in total
    std::string neighborhood;   // district id from the content catalogue; may be empty
};
struct DistrictProgressRow {
    std::string id, name;
    std::uint32_t rank{};
};
struct ProgressionModel {
    bool available{}, challenges_enabled{}, ranks_maxed{}, score_available{};
    bool challenges_hidden{};
    bool everything_unlocked{};   // ReSkate.UnlockEverything
    std::vector<BusStopRow> bus_stops;
    std::vector<ChallengeProgressRow> challenges;
    std::array<DistrictProgressRow, 4> districts;
    std::int64_t score{}, score_cap{};
    std::int32_t score_level{};
    std::string feedback;
};
}
