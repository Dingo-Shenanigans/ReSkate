#pragma once
// The waypoint the player placed on the pause map (DingoMapPOIType_Waypoint), read from the
// map's point-of-interest registry so the trainer can teleport to it. Game thread only.
#include "trainer_landing.h"
#include <cstdint>
#include <string>

namespace dingosdk::trainer {
struct MapWaypointRead {
    landing::Reading reading;
    std::string detail; // what the walk saw, for `trainer waypoint info` and the log
};
// One walk of the registry under the map model manager's lock. Cheap, but it walks every
// POI, so the trainer only calls it while the player has a use for the answer.
MapWaypointRead read_map_waypoint(std::uintptr_t base) noexcept;
} // namespace dingosdk::trainer
