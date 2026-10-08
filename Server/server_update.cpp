#include "server_update.h"
#include "Engine/Core/Platform/launcher_support.h"
#ifdef _WIN32
#include "Launcher/updater.h"
#include <Windows.h>
#else
#include "server_release.h"
#include "launcher_update_config.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <vector>
#endif
#include <mutex>
#include <optional>
#include <stdexcept>

namespace dingosdk::server {
#ifdef _WIN32
namespace {
std::mutex release_mutex;
std::optional<launcher_update::Config> release; // from the last check

std::filesystem::path self() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot find ReSkateServer.exe");
    path.resize(length);
    return path;
}
} // namespace

bool updates_enabled() noexcept { return launcher_update::binary_updates_enabled(); }

UpdateCheck check_for_update() {
    UpdateCheck check;
    try {
        auto config = launcher_update::fetch_config();
        if (!config) {
            check.problem = "GitHub could not be reached";
            return check;
        }
        if (config->server.url.empty()) {
            check.problem = "the latest release has no server";
            return check;
        }
        check.version = config->server.version;
        check.available = launcher::sha256_file(self()) != config->server_exe_sha256;
        std::lock_guard lock(release_mutex);
        release = std::move(config);
    } catch (const std::exception &e) {
        check.problem = e.what();
    }
    return check;
}

void install_update(const std::filesystem::path &folder) {
    launcher_update::RemoteFile file;
    std::string exe_sha256;
    {
        std::lock_guard lock(release_mutex);
        if (!release || release->server.url.empty()) throw std::runtime_error("no update has been found");
        file = release->server;
        exe_sha256 = release->server_exe_sha256;
    }
    const auto archive = launcher_update::download_verified(folder / L"ReSkateServer-update.zip", file);
    struct Remove {
        std::filesystem::path path;
        ~Remove() { DeleteFileW(path.c_str()); }
    } remove{archive};
    // Otherwise the new copy would find itself out of date and update forever.
    if (launcher_update::archive_entry_sha256(archive, "ReSkateServer.exe") != exe_sha256)
        throw std::runtime_error("the release's ReSkateServer.exe doesn't match launcher.json");
    launcher_update::install_archive(archive, folder);
}

bool relaunch() {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = GetCommandLineW();
    const auto started = CreateProcessW(self().c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void remove_previous_update(const std::filesystem::path &folder) noexcept {
    launcher_update::remove_replaced_files(folder);
    DeleteFileW((folder / L"ReSkateServer-update.zip.new").c_str());
}
#else // _WIN32

// Linux: the same releases, read with the tools every server machine has. curl does the HTTPS
// (as for the global bans) and tar unpacks; both are run directly, never through a shell, and
// nothing is installed that launcher.json does not pin by SHA-256.
namespace {
std::mutex release_mutex;
std::optional<LinuxRelease> release; // from the last check

// Read once, before an update can replace the file: /proc/self/exe then names a deleted one.
const std::filesystem::path &self() {
    static const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe");
    return path;
}

// Runs a program found on PATH with these arguments and returns what it printed, at most
// `limit` bytes. Throws with the program's own complaint when it fails or cannot be started.
std::string run_tool(const std::vector<std::string> &arguments, std::size_t limit) {
    int out[2], err[2];
    if (pipe(out) != 0) throw std::runtime_error(arguments.front() + " could not be started");
    if (pipe(err) != 0) {
        close(out[0]), close(out[1]);
        throw std::runtime_error(arguments.front() + " could not be started");
    }
    const auto child = fork();
    if (child < 0) {
        for (const int fd : {out[0], out[1], err[0], err[1]}) close(fd);
        throw std::runtime_error(arguments.front() + " could not be started");
    }
    if (child == 0) {
        std::vector<char *> argv;
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        dup2(out[1], STDOUT_FILENO);
        dup2(err[1], STDERR_FILENO);
        if (const int null = open("/dev/null", O_RDONLY); null >= 0) dup2(null, STDIN_FILENO);
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    close(out[1]), close(err[1]);
    std::string output, complaint;
    bool too_much{};
    pollfd fds[2]{{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    for (int open_ends = 2; open_ends;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (auto &fd : fds) {
            if (fd.fd < 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buffer[65536];
            const auto count = read(fd.fd, buffer, sizeof(buffer));
            if (count > 0) {
                auto &text = fd.fd == out[0] ? output : complaint;
                const auto room = (fd.fd == out[0] ? limit : std::size_t{4096}) - std::min(text.size(), fd.fd == out[0] ? limit : std::size_t{4096});
                text.append(buffer, std::min<std::size_t>(room, static_cast<std::size_t>(count)));
                if (fd.fd == out[0] && static_cast<std::size_t>(count) > room) too_much = true;
            } else if (count == 0 || errno != EINTR) {
                close(fd.fd);
                fd.fd = -1;
                --open_ends;
            }
        }
    }
    for (auto &fd : fds)
        if (fd.fd >= 0) close(fd.fd);
    int status{};
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && complaint.empty())
        throw std::runtime_error(arguments.front() + " is not installed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        while (!complaint.empty() && (complaint.back() == '\n' || complaint.back() == '\r')) complaint.pop_back();
        std::replace(complaint.begin(), complaint.end(), '\n', ' ');
        throw std::runtime_error(complaint.empty() ? arguments.front() + " failed" : complaint);
    }
    if (too_much) throw std::runtime_error(arguments.front() + " answered with more than expected");
    return output;
}

std::vector<std::string> curl(const std::string &url, std::uint64_t limit, int seconds) {
    if (!https_url(url)) throw std::runtime_error("a download link is not HTTPS");
    return {"curl", "--silent", "--show-error", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
            "--max-time", std::to_string(seconds), "--max-filesize", std::to_string(limit), "--user-agent", "ReSkateServer/1",
            "--url", url};
}
std::string https_text(const std::string &url, std::size_t limit) { return run_tool(curl(url, limit, 20), limit); }

struct RemovePath {
    std::filesystem::path path;
    ~RemovePath() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
const char *const archive_name = "ReSkateServer-update.tar.gz";
const char *const stage_name = ".reskate-update";
} // namespace

bool updates_enabled() noexcept { return launcher_update::launcher_binary_updates; }

UpdateCheck check_for_update() {
    UpdateCheck check;
    try {
        std::optional<LinuxRelease> found;
        if (const char *local = std::getenv("RESKATE_LAUNCHER_CONFIG_FILE"); local && *local) {
            // Test an unpublished config before attaching it to a release, as the launcher can.
            // Its links must be whole HTTPS ones: there is no release to find an "asset:" in.
            std::ifstream input(local, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (!input && !input.eof()) throw std::runtime_error(std::string("cannot read ") + local);
            found = parse_linux_release(text, {});
        } else {
            std::string repo;
            for (const wchar_t c : std::wstring_view(launcher_update::launcher_release_repo)) repo += static_cast<char>(c);
            // Only this lookup uses GitHub's rate-limited API; the files come from the release's download links.
            const auto assets = release_assets(https_text("https://api.github.com/repos/" + repo + "/releases/latest", 4 * 1024 * 1024));
            const auto manifest = assets.find("launcher.json");
            if (manifest == assets.end()) {
                check.problem = "the latest release has no launcher.json";
                return check;
            }
            found = parse_linux_release(https_text(manifest->second, 256 * 1024), assets);
        }
        if (!found) {
            check.problem = "the latest release has no Linux server";
            return check;
        }
        check.version = found->version;
        check.available = launcher::sha256_file(self()) != found->exe_sha256;
        std::lock_guard lock(release_mutex);
        release = std::move(found);
    } catch (const std::exception &e) {
        check.problem = e.what();
    }
    return check;
}

void install_archive(const std::filesystem::path &archive, const std::filesystem::path &folder, std::string_view exe_sha256) {
    namespace fs = std::filesystem;
    // Looked at before it is unpacked: one folder, nothing that reaches outside it.
    checked_archive_listing(run_tool({"tar", "-tzf", archive.string()}, 1024 * 1024));
    const auto stage = folder / stage_name;
    RemovePath remove_stage{stage};
    fs::remove_all(stage);
    fs::create_directory(stage);
    run_tool({"tar", "-xzf", archive.string(), "-C", stage.string(), "--strip-components=1", "--no-same-owner"}, 64 * 1024);
    // Plain files and folders only: a link could point anywhere.
    std::vector<fs::path> files, folders;
    for (auto entry = fs::recursive_directory_iterator(stage); entry != fs::recursive_directory_iterator(); ++entry) {
        const auto status = entry->symlink_status();
        if (fs::is_directory(status)) folders.push_back(fs::relative(entry->path(), stage));
        else if (fs::is_regular_file(status)) files.push_back(fs::relative(entry->path(), stage));
        else throw std::runtime_error("the update has something that is not a file or a folder");
    }
    const auto exe = self().filename();
    // Otherwise the new copy would find itself out of date and update forever.
    if (std::find(files.begin(), files.end(), exe) == files.end() || launcher::sha256_file(stage / exe) != exe_sha256)
        throw std::runtime_error("the release's ReSkateServer doesn't match launcher.json");
    // Nothing in the folder has changed until here. Each file then takes its place in one step
    // (a rename within the folder), which Linux allows for a program and libraries in use; the
    // server itself goes last, so a failure part-way leaves a server that updates again.
    for (const auto &relative : folders) fs::create_directories(folder / relative);
    std::stable_partition(files.begin(), files.end(), [&](const fs::path &file) { return file != exe; });
    for (const auto &relative : files) fs::rename(stage / relative, folder / relative);
}

void install_update(const std::filesystem::path &folder) {
    LinuxRelease pinned;
    {
        std::lock_guard lock(release_mutex);
        if (!release) throw std::runtime_error("no update has been found");
        pinned = *release;
    }
    const auto archive = folder / archive_name;
    RemovePath remove_archive{archive};
    auto download = curl(pinned.url, pinned.size, 600);
    download.insert(download.end(), {"--output", archive.string()});
    run_tool(download, 64 * 1024);
    if (std::filesystem::file_size(archive) != pinned.size || launcher::sha256_file(archive) != pinned.sha256)
        throw std::runtime_error("the downloaded update doesn't match launcher.json");
    install_archive(archive, folder, pinned.exe_sha256);
}

// The updated server takes this one's place: the same process, so whatever started it (a
// terminal, systemd) keeps the server it started.
bool relaunch() {
    std::vector<std::string> arguments;
    {
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        for (std::string argument; std::getline(in, argument, '\0');) arguments.push_back(argument);
    }
    if (arguments.empty()) arguments.push_back(self().string());
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    // Nothing this server had open (Steam's sockets, the log) carries over.
    std::error_code ignored;
    std::vector<int> open_files;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd", ignored)) {
        const auto fd = std::atoi(entry.path().filename().c_str());
        if (fd > STDERR_FILENO) open_files.push_back(fd);
    }
    for (const int fd : open_files)
        if (const int flags = fcntl(fd, F_GETFD); flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    execv(self().c_str(), argv.data());
    return false;
}

void remove_previous_update(const std::filesystem::path &folder) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(folder / stage_name, ignored);
    std::filesystem::remove(folder / archive_name, ignored);
}

#endif // _WIN32
} // namespace dingosdk::server
