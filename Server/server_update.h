#pragma once
#include <filesystem>
#include <string>
#include <string_view>

// Self-update for release builds of the dedicated server. The latest GitHub
// release's launcher.json pins the server zip (size and SHA-256) and the
// SHA-256 of the ReSkateServer.exe inside it; a server whose own exe differs
// installs that zip over its folder and starts again. Its own files
// (ReSkateServer.json, Mods, logs) are not in the zip, so they are kept.
// The Linux server does the same from the release's tarball ("server_linux" in
// launcher.json, server_release.h), using the machine's curl and tar.
namespace dingosdk::server {
// Release builds only: a local build never replaces itself.
bool updates_enabled() noexcept;

struct UpdateCheck {
    bool available{};
    std::string version; // the release's server version, e.g. "0.3.8"
    std::string problem; // why the check could not finish, or empty
};
// Asks GitHub for the latest release; blocks for a few seconds at most.
UpdateCheck check_for_update();
// Downloads the pinned zip into `folder` and unpacks it there. Throws on any
// failure, before anything in the folder has changed.
void install_update(const std::filesystem::path& folder);
// Starts a new copy of this exe with the same command line. On Linux the new copy takes
// this process's place, so this only returns (false) when that failed.
bool relaunch();
// Deletes the files a previous update renamed aside.
void remove_previous_update(const std::filesystem::path& folder) noexcept;
#ifndef _WIN32
// The second half of install_update, for a tarball already downloaded and verified: checks
// what is in it, that its ReSkateServer has this SHA-256, and puts its files in `folder`.
void install_archive(const std::filesystem::path& archive, const std::filesystem::path& folder, std::string_view exe_sha256);
#endif
} // namespace dingosdk::server
