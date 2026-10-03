#pragma once
// Named sets of physics tuning values, one JSON file each. A preset holds only the values that
// differ from the game's own tuning, keyed by the game's names for them
// ("PhysicsPush.MaxPushableSpeed"), so it survives a game update that moves the values around.
// No game process is needed here.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::physics_tuning::presets {
using Values = std::map<std::string, float, std::less<>>;
// 1 to 40 letters, digits, spaces, '-' and '_'; not starting or ending with a space.
bool valid_name(std::string_view name) noexcept;
// The preset names in `directory`, sorted. Empty when it does not exist.
std::vector<std::string> list(const std::filesystem::path &directory);
// Writes (or replaces) a preset. False when the name is invalid or the file could not be written.
bool save(const std::filesystem::path &directory, std::string_view name, const Values &values);
// Nothing when the preset is missing or is not a preset file. Values that are not finite numbers are skipped.
std::optional<Values> load(const std::filesystem::path &directory, std::string_view name);
bool remove(const std::filesystem::path &directory, std::string_view name);
} // namespace dingosdk::physics_tuning::presets
