#pragma once
#include "server_config.h"
#include "Engine/Game/World/network_objects.h"
#include <filesystem>
#include <string>
#include <vector>

// Park mods on a dedicated server: parks built in the Park Editor and shipped as mods
// (<Mods>\<folder>\parks\<key>.park.json, as Extension/Objects/ParkEditor/park_mods.h writes
// them), spawned for everyone as objects the server owns (server_parks.cpp).
namespace dingosdk::server {
inline constexpr std::size_t max_park_file_bytes = 2 * 1024 * 1024;
struct LoadedPark {
    std::string key; // the base map's park it is (bam...)
    // Numbered 1, 2... in the file's order; the server gives them IDs of its own when spawned.
    std::vector<NetworkObject> objects;
    // Objects players' games would not take (multiplayer::valid_network_object): not Build Kit
    // items, or a broken position, rotation or scale. "#<n> <item>", n counting from 1.
    std::vector<std::string> skipped;
};
// Reads `park` from the mods folder. Throws std::runtime_error saying what is wrong: no such
// folder or file, not a ReSkate park, no objects players' games can show.
LoadedPark load_park_mod(const std::filesystem::path &mods, const ParkMod &park);
} // namespace dingosdk::server
