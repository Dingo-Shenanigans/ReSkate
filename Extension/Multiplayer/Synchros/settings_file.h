#pragma once
#include "sharing_rules.h"
#include <filesystem>
#include <string>

// Skate Synchros' settings, kept in Mods/.synchros/settings.json (beside the cache, and like it out
// of the profile, which is sent to ReSkate's backend). Codes are kept as their twelve symbols.
namespace dingosdk::multiplayer::synchros {

[[nodiscard]] std::string settings_json(const Settings& settings);
// Anything unreadable, out of range or not a valid code is left out; a file that cannot be read at
// all gives the defaults (both switches off).
[[nodiscard]] Settings settings_from_json(std::string_view text);

[[nodiscard]] Settings load_settings(const std::filesystem::path& file);
bool save_settings(const std::filesystem::path& file, const Settings& settings);

} // namespace dingosdk::multiplayer::synchros
