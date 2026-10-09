#pragma once

#include "world_model.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk {

inline constexpr std::size_t custom_level_manifest_max_bytes = 64 * 1024;
inline constexpr std::size_t custom_level_manifest_max_levels = 64;
// Ceiling for every mod's manifest put together, above the per-file limit but
// still bounded so a folder full of mods cannot grow the catalogue without end.
inline constexpr std::size_t combined_custom_level_limit = 256;

struct CustomLevelManifest {
    bool present{};
    std::vector<LevelInfo> levels;
    std::string issue;
};

// Parse a mod's or the Patch folder's reskate-levels.json. Invalid input
// fails closed: no partial set of destinations is returned.
CustomLevelManifest parse_custom_level_manifest(std::string_view text) noexcept;
CustomLevelManifest read_custom_level_manifest(const std::filesystem::path& path) noexcept;

// Combines manifests in priority order, so one folder per mod can each declare
// destinations. The earlier manifest wins when two declare the same asset, and
// a manifest carrying an issue contributes nothing: one broken mod must not
// hide the rest.
CustomLevelManifest combine_custom_level_manifests(std::span<const CustomLevelManifest> manifests);

// Native rows stay authoritative. A manifest row can supply a display label for
// an already-native level, or add a manifest-only detached-level destination.
WorldCatalog merge_custom_levels(WorldCatalog native, const CustomLevelManifest& manifest);

} // namespace dingosdk
