#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Remote launcher config: launcher.json in the latest GitHub release. Every download
// it names is pinned by size and SHA-256; the config only chooses what to fetch.
namespace dingosdk::launcher_update {

struct RemoteFile {
    std::string version;
    std::wstring url;
    std::string sha256;
    std::uint64_t size{};
};

struct GameBuild {
    std::uint32_t app_id{};
    std::uint32_t depot_id{};
    std::string manifest_id;
    std::string build_id;
    std::string skate_sha256;
};

struct Config {
    RemoteFile launcher;
    RemoteFile runtime;
    RemoteFile depot_downloader;
    GameBuild game;
    // The dedicated server's zip, and the SHA-256 of the ReSkateServer.exe inside
    // it: a server whose own exe differs is out of date. Empty when not published.
    RemoteFile server;
    std::string server_exe_sha256;
};

// Release builds replace ReSkateLauncher.exe and ReSkate.dll; local builds only report.
bool binary_updates_enabled() noexcept;
Config parse_config(std::string_view text);
// Returns nothing when offline or the config is invalid; the reason is logged.
std::optional<Config> fetch_config();

// One GitHub release, for the launcher's changelogs: its patch notes are what its page on
// GitHub shows, the release's description or, without one, its commit's message.
struct ReleaseNote {
    std::string tag, title, date, notes;   // date: YYYY-MM-DD; notes: markdown
};
// The releases page's Atom feed (<repo>/releases.atom), newest first as it comes.
std::vector<ReleaseNote> parse_release_feed(std::string_view feed);
// The newest release of the repo the launcher updates from. Throws when GitHub cannot be reached.
ReleaseNote fetch_release_note();
// That repo's releases page on GitHub.
std::string releases_page();

// Streams an HTTPS GET into `sink`, following HTTPS redirects; throws past
// `limit` bytes, on a non-200 answer, or whatever `sink` throws (to cancel).
void http_stream(const std::wstring& url, std::uint64_t limit, int timeout_ms,
                 const std::function<void(const char*, DWORD)>& sink);

// Reports bytes received and the expected total while a download runs.
using Progress = std::function<void(std::uint64_t received, std::uint64_t total)>;

// True when the file on disk already has the pinned SHA-256.
bool file_matches(const std::filesystem::path& path, const RemoteFile& file);
// Downloads beside `target` as .new and verifies it. Returns the verified path.
std::filesystem::path download_verified(const std::filesystem::path& target, const RemoteFile& file,
                                        const Progress& progress = {});
// Replaces an unloaded file (ReSkate.dll) in place.
void replace_file(const std::filesystem::path& target, const RemoteFile& file, const Progress& progress = {});
// Renames the running launcher to .old and moves the verified update into place.
void replace_running_launcher(const std::filesystem::path& self, const RemoteFile& file,
                              const Progress& progress = {});
// Removes ReSkateLauncher.exe.old left by a previous self-update.
void remove_previous_launcher(const std::filesystem::path& self) noexcept;

// Unpacks every file of a verified ZIP into `directory`, replacing what is
// there. All files are unpacked beside their targets first, so a bad archive
// changes nothing. A file Windows won't overwrite (the running exe, a loaded
// DLL) is renamed to <name>.update-old first. Returns the names installed.
std::vector<std::string> install_archive(const std::filesystem::path& archive, const std::filesystem::path& directory);
// SHA-256 of one file inside a ZIP, or empty when the ZIP has no such file.
std::string archive_entry_sha256(const std::filesystem::path& archive, const char* name);
// Deletes the *.update-old files a previous install_archive left behind.
void remove_replaced_files(const std::filesystem::path& directory) noexcept;

// Downloads and unpacks DepotDownloader under %LOCALAPPDATA%\ReSkate\tools.
std::filesystem::path ensure_depot_downloader(const RemoteFile& file, const Progress& progress = {});
// How DepotDownloader signs in to Steam. An empty username shows a QR code.
struct SteamLogin {
    std::string username;
    bool remember{true};  // DepotDownloader keeps a login token, not the password
    bool prefer_code{};   // ask for a Steam Guard code instead of app approval
};

enum class PromptKind { password, authenticator_code, email_code };
struct Prompt {
    PromptKind kind{};
    std::string text;     // DepotDownloader's own wording, e.g. the masked email
    bool retry{};         // Steam rejected the previous code
};
// Blocks until the user answers; nothing stops DepotDownloader.
using PromptHandler = std::function<std::optional<std::string>(const Prompt&)>;

// Runs DepotDownloader hidden and hands each stdout/stderr line to `on_line`.
// Password and Steam Guard prompts go to `on_prompt`; answers are written to
// its stdin, never its command line. Setting `cancel` terminates it.
DWORD run_depot_downloader(const std::filesystem::path& depot_downloader, const GameBuild& game,
                           const std::filesystem::path& directory, bool validate, const SteamLogin& login,
                           const std::function<void(std::string_view line)>& on_line,
                           const PromptHandler& on_prompt, const std::atomic<bool>& cancel);

} // namespace dingosdk::launcher_update
