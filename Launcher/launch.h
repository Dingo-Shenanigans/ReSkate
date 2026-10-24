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
// The build number the installed game's data says it is (Data\layout.toc), or empty when
// there is none or it cannot be read.
std::string game_data_build(const launcher::SiblingPaths& paths);
// Whether the game's data is missing or of another build than the supported one, whatever
// Skate.exe is: Steam updated the game under it, a copy of an updated game was put here, or a
// download stopped part-way. Such a game has to be verified against the supported build, file
// by file, not only topped up. Also true when a file of the supported build's data is missing,
// of another size, or kept online only by a cloud folder (game_data_damage). False when the
// data's build cannot be told and every file is there.
bool game_data_mismatched(const launcher::SiblingPaths& paths);
// Advice for a PC whose RAM and page file are too small for the game with mods; empty when they
// are enough.
std::string memory_advice();
// What is wrong with the game's data files against the supported build's list, for the log; empty
// when every file is there, of its size, and on this PC.
std::string game_data_damage(const launcher::SiblingPaths& paths);
// The config's depot/manifest ships the build this launcher was compiled for.
bool config_matches_build(const launcher_update::Config& config);
void relaunch(const std::filesystem::path& self, const std::vector<std::wstring>& arguments);
// Under Wine/Proton with MangoHud on, restarts the launcher with MangoHud off for
// its own window and returns true. The game it starts still gets MangoHud.
bool relaunch_without_mangohud(const std::filesystem::path& self, const std::vector<std::wstring>& arguments);
// Steam's client process is running and signed in to an account (or, started by the native
// Steam client through a compatibility tool, it has a signed-in account). When it is not,
// start_game runs the game in offline mode.
bool steam_signed_in();
// Running under Wine (Proton, or a Wine-based tool on macOS).
bool under_wine();
// Why steam_signed_in() is false, for ReSkate.log; empty when it is true.
std::wstring steam_offline_reason();
// Display name of the signed-in Steam account (UTF-8), or empty when Steam is
// not running, signed out, or the name cannot be read.
std::string steam_persona_name();
// The signed-in account's profile picture in Steam's avatar cache, or empty
// when Steam is not running, signed out, or has not cached it.
std::filesystem::path steam_avatar_path();
// Validates, starts, injects and resumes Skate; returns its process id.
// `created` receives the id as soon as the process exists (ReSkate's startup
// splash can open before this returns). Throws a user-facing message.
DWORD start_game(const Session& session, const launcher::LaunchOptions& options,
                 const std::function<void(DWORD)>& created = {});

} // namespace dingosdk::launcher_app
