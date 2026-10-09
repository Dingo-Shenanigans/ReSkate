#pragma once

#include "Engine/Core/Platform/launcher_support.h"
#include "updater.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dingosdk::launcher_app {

struct Session {
    std::filesystem::path self;
    launcher::SiblingPaths paths;
};

// Resolves ReSkateLauncher.exe and its sibling game files and opens ReSkate.log.
Session open_session(const std::string& log_level);
// Skate.exe and steam_api64.dll are present and exactly the supported build.
bool game_files_supported(const launcher::SiblingPaths& paths);
// The config's depot/manifest ships the build this launcher was compiled for.
bool config_matches_build(const launcher_update::Config& config);
void relaunch(const std::filesystem::path& self, const std::vector<std::wstring>& arguments);
// Steam's client process is running and signed in to an account. When it is
// not, start_game runs the game in offline mode.
bool steam_signed_in();
// Why steam_signed_in() is false, for ReSkate.log; empty when it is true.
std::wstring steam_offline_reason();
// Display name of the signed-in Steam account (UTF-8), or empty when Steam is
// not running, signed out, or the name cannot be read.
std::string steam_persona_name();
// Validates, starts, injects and resumes Skate; returns its process id.
// `created` receives the id as soon as the process exists (ReSkate's startup
// splash can open before this returns). Throws a user-facing message.
DWORD start_game(const Session& session, const launcher::LaunchOptions& options,
                 const std::function<void(DWORD)>& created = {});

} // namespace dingosdk::launcher_app
