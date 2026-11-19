#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::initfs {
struct ExportReport {
    std::size_t scripts{}, configs{}, created{}, preserved{}, skipped{};
    bool already_exported{};
};

// Virtual Scripts/*.lua -> scripts/*; textual settings -> config/*.
// Rejects paths that cannot be represented safely as ordinary Windows files.
std::optional<std::string> loose_relative_path(std::string_view virtual_path);
std::filesystem::path utf8_path(std::string_view text);
std::filesystem::path export_manifest(const std::filesystem::path& root);
// Stored outside config/ so this preference also works while overrides are off.
bool loose_files_preference(const std::filesystem::path& game_directory);
bool loose_files_session_enabled(const std::filesystem::path& game_directory);
// Plugin developer mode, in the same file: a plugin the player already approved keeps its approval
// when its files change. Read by the launcher and the game, and set from either. Throws when
// ReSkate.settings.json cannot be read or written.
bool plugin_developer_mode(const std::filesystem::path& game_directory);
void set_plugin_developer_mode(const std::filesystem::path& game_directory, bool on);
// How strict ReSkate is with plugins on this PC, in the same file ("plugin_limits"): the player's
// own choice (the menu's PLUGINS, LIMITS). What is not there, or is not a value it can be, is the
// default. Throws as plugin developer mode does.
struct PluginLimits {
    bool stop_slow{true};   // a plugin that takes too long a tick for five seconds is stopped
    double tick_ms{16};     // how long a tick that is: 6 to 50 (6 the default until 2.1.5)
    bool stop_errors{true}; // a plugin with more than five errors in ten seconds is stopped
    int memory_mb{512};     // the memory a plugin may use: 128 to 1024 (128 the default until 2.1.5)
    bool operator==(const PluginLimits&) const = default;
};
PluginLimits plugin_limits(const std::filesystem::path& game_directory);
void set_plugin_limits(const std::filesystem::path& game_directory, const PluginLimits& limits);
std::filesystem::path data_directory(const std::filesystem::path& game_directory,
    const std::vector<std::wstring>& game_arguments);

// Reads only the exact supported executable to obtain the InitFS AES key.
// The key stays in memory. Existing loose files are never overwritten.
// First-launch mode leaves a completed export alone, including deleted files.
ExportReport export_files(const std::filesystem::path& executable,
    const std::filesystem::path& data_root, const std::filesystem::path& output_root,
    bool first_launch_only = false);
}
