#pragma once
// Map teleports: zones a map mod ships in teleports.json (beside its manifest.json). Riding
// into one sends the local skater to the zone's spot, facing its heading, and keeps its speed.
// Rules in map_teleports_model.h; the file format in contrib/map-teleports.md.
#include <cstdint>
#include <string>

namespace dingosdk::map_teleports {
// Client thread, every tick. `level` is the level the player means (a custom map's own).
void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string& level) noexcept;
}
