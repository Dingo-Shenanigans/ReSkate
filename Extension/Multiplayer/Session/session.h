#pragma once
#include "Engine/Game/Multiplayer/session_model.h"
#include <cstdint>
#include <string>

namespace dingosdk::multiplayer {
// Commands and tick execute on the game's client thread. model() is thread-safe.
std::string command(std::string_view action, std::string_view argument = {}, std::string_view password = {});
// Private menu queue: passwords never pass through console history/logging.
bool queue_command(std::string_view action, std::string_view argument, std::string_view password);
// Called just before the host's validated native load is submitted, on the client thread.
void host_map_change(std::string_view destination);
enum class MapLoadResult { waiting, queued, failed, missing }; // missing: the map is not installed
// Runs on the client thread; only catalog-registered destinations may be queued.
using MapLoader = MapLoadResult (*)(std::string_view map, bool submitted, std::string &detail);
void tick(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready, std::string_view map,
          MapLoader loader = nullptr);
MultiplayerModel model();
std::string take_leave_notice(); // why the session just ended over a missing map, once; client thread
// The session's chat lines for the overlay. Thread-safe and cheap: callable every frame.
MultiplayerChat chat();
} // namespace dingosdk::multiplayer
