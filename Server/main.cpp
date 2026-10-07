// ReSkate dedicated server: a headless session host that players find in the
// in-game server browser. Runs from its own folder, next to steam_api64.dll and
// the Steam client files (steamclient64.dll, tier0_s64.dll, vstdlib_s64.dll).
// On Linux the Steam files are libsteam_api.so and steamclient.so.
#include "global_bans.h"
#include "server_config.h"
#include "server_host.h"
#include "server_update.h"
#include "steam_server.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/World/world_layer_catalog.h"
#include "Engine/Game/World/world_names.h"
#include "Engine/Vfs/world_layer_scan.h"
#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace dingosdk;
using namespace dingosdk::server;

namespace {
std::atomic<bool> stopping{};
// run() returns this when a newer server release should be installed.
constexpr int restart_for_update = -2;
std::string update_version;
std::mutex log_mutex;
std::ofstream log_file;
// The console is written by a thread of its own. Windows holds a console's output while text
// in its window is selected, and a pipe nobody reads fills up: whoever writes then waits. When
// that was the server loop, the server read nothing from its players until the console let go,
// and they were gone by then. The log file is still written where the line is made.
struct Console {
    std::mutex mutex;
    std::condition_variable more, drained;
    std::deque<std::string> lines;
    std::size_t skipped{};
    bool writing{}, started{};
    void write(std::string line) {
        std::lock_guard lock(mutex);
        if (!started) {
            started = true;
            std::thread([this] { run(); }).detach();
        }
        // A console that stays held must not grow this without end: the oldest lines go, and
        // the file has them all.
        if (lines.size() >= 4096) {
            lines.pop_front();
            ++skipped;
        }
        lines.push_back(std::move(line));
        more.notify_one();
    }
    void run() {
        for (;;) {
            std::unique_lock lock(mutex);
            more.wait(lock, [&] { return !lines.empty(); });
            const auto line = std::move(lines.front());
            lines.pop_front();
            const auto missed = std::exchange(skipped, 0);
            writing = true;
            lock.unlock();
            if (missed) std::printf("(%zu earlier lines are only in ReSkateServer.log: the console was not taking output)\n", missed);
            std::fputs(line.c_str(), stdout);
            lock.lock();
            writing = false;
            if (lines.empty()) drained.notify_all();
        }
    }
    // Waits until everything written so far is on screen, but not for a console that is held.
    void flush(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex);
        drained.wait_for(lock, limit, [&] { return lines.empty() && !writing; });
    }
};
Console &console() {
    static auto *value = new Console; // outlives every thread that may still be writing at exit
    return *value;
}
void write_log(const std::string &text) {
    std::lock_guard lock(log_mutex);
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    if (log_file) log_file << '[' << stamp << "] " << text << std::endl;
    console().write(std::string("[") + (stamp + 11) + "] " + text + "\n");
}
std::atomic<bool> finished{};
#ifdef _WIN32
BOOL WINAPI on_console(DWORD event) {
    stopping = true;
    // Closing the window ends the process as soon as this returns. Wait (Windows
    // allows about 5 s) for the server to sign out of Steam, so its entry leaves
    // the server list instead of lingering there.
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT)
        for (int i = 0; i < 90 && !finished; ++i) Sleep(50);
    return TRUE;
}
#else
void on_signal(int) { stopping = true; }
#endif
std::filesystem::path folder() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot find ReSkateServer.exe");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    char path[4096]{};
    const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) throw std::runtime_error("Cannot find ReSkateServer");
    return std::filesystem::path(std::string(path, static_cast<std::size_t>(length))).parent_path();
#endif
}
// Console lines, read on their own thread so the network loop never waits for typing.
// One reader for the whole process: run() can start again after a failed update.
struct Input {
    std::mutex mutex;
    std::deque<std::string> lines;
#ifdef _WIN32
    void run() {
        std::string line;
        while (!stopping && std::getline(std::cin, line)) {
            std::lock_guard lock(mutex);
            lines.push_back(line);
        }
    }
#else
    std::thread worker;
    // Raw poll()+read() on stdin instead of iostream: the thread holds no C++
    // stream locks, so process exit can never deadlock against it, and poll()
    // wakes regularly so stopping is noticed without closing stdin.
    void run() {
        // Control pipes (fleet tools, `echo cmd > fifo`) open a writer per
        // command: read() returning 0 then means "no writers right now", not
        // end of input, so keep waiting instead of ending console input.
        const bool fifo = [] {
            struct stat info{};
            return fstat(STDIN_FILENO, &info) == 0 && S_ISFIFO(info.st_mode);
        }();
        std::string pending;
        pending.reserve(256);
        for (;;) {
            if (stopping) return;
            pollfd waiting{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&waiting, 1, 200);
            if (ready < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (!ready) continue; // timeout: re-check stopping
            if (!(waiting.revents & (POLLIN | POLLHUP))) return;
            char buffer[4096];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return;
            }
            if (count == 0) {
                if (!fifo) return; // EOF on files, /dev/null, terminals: input is over
                // No writers at the moment; sleep so POLLHUP doesn't spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    // CRLF input (a script saved on Windows, a panel or telnet): without
                    // this, "quit\r" is an unknown command and the server keeps running.
                    if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                    std::lock_guard lock(mutex);
                    lines.push_back(pending);
                    pending.clear();
                } else {
                    pending.push_back(buffer[i]);
                }
            }
        }
    }
    void shutdown() {
        if (worker.joinable()) worker.join();
    }
    void ensure_running() {
        std::lock_guard lock(mutex);
        if (!worker.joinable()) worker = std::thread([this] { run(); });
    }
#endif
    std::deque<std::string> take() {
        std::lock_guard lock(mutex);
        return std::exchange(lines, {});
    }
};
Input &console_input() {
    static Input input;
#ifdef _WIN32
    static std::once_flag started;
    std::call_once(started, [] { std::thread([] { input.run(); }).detach(); });
#else
    input.ensure_running();
#endif
    return input;
}
constexpr auto update_interval = std::chrono::minutes(30);
#ifdef _WIN32
// Double-clicked, the server is alone on a visible console that closes with it.
// From a terminal, a script, a hosting panel or a hidden scheduled task it is not.
bool own_window() {
    DWORD processes[2]{};
    const auto window = GetConsoleWindow();
    return GetConsoleProcessList(processes, 2) == 1 && window && IsWindowVisible(window) &&
           GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
}
#endif
} // namespace

#ifdef _WIN32
int run(int argc, wchar_t **argv, bool skip_update) {
    // Unbuffered, so a hosting panel reading the pipe sees each line at once.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(on_console, TRUE);
    const auto here = folder();
    // Release builds: ReSkateServer --export-world-layers "<Skate folder>" "<file>" writes the
    // players' world-layer catalog from the game's level data (release\build_release.bat).
    if (argc == 4 && std::wstring(argv[1]) == L"--export-world-layers") {
        try {
            const auto catalog = world_layer_scan::scan(argv[2]);
            if (catalog.layers.empty()) throw std::runtime_error("no world layers found; is that the Skate folder?");
            std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
            out << world_layer_scan::to_json(catalog, "server");
            if (!out) throw std::runtime_error("cannot write the file");
            std::printf("Wrote %zu world layers.\n", catalog.layers.size());
            return 0;
        } catch (const std::exception &e) {
            std::printf("World layer export failed: %s\n", e.what());
            return 1;
        }
    }
    auto config_file = here / L"ReSkateServer.json";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--config") config_file = argv[++i];
    if (!log_file.is_open()) log_file.open(here / L"ReSkateServer.log", std::ios::app);
#else
int run(int argc, char **argv, bool skip_update) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // A hosting panel that dies leaves stdout a pipe with no reader. Without this
    // the next printf kills the server before it logs, saves or signs out of Steam.
    std::signal(SIGPIPE, SIG_IGN);
    const auto here = folder();
    if (argc == 4 && std::string(argv[1]) == "--export-world-layers") {
        try {
            const auto catalog = world_layer_scan::scan(std::filesystem::path(argv[2]));
            if (catalog.layers.empty()) throw std::runtime_error("no world layers found; is that the Skate folder?");
            std::ofstream out(std::filesystem::path(argv[3]), std::ios::binary | std::ios::trunc);
            out << world_layer_scan::to_json(catalog, "server");
            if (!out) throw std::runtime_error("cannot write the file");
            std::printf("Wrote %zu world layers.\n", catalog.layers.size());
            return 0;
        } catch (const std::exception &e) {
            std::printf("World layer export failed: %s\n", e.what());
            return 1;
        }
    }
    auto config_file = here / "ReSkateServer.json";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config") config_file = argv[++i];
    if (!log_file.is_open()) log_file.open(here / "ReSkateServer.log", std::ios::app);
#endif

    ServerConfig config;
    try {
        const bool fresh = !std::filesystem::exists(config_file);
        std::vector<std::string> added;
        config = load_config(config_file, &added);
        if (fresh) write_log("Wrote a default " + path_utf8(config_file.filename()) + ". Edit it to name the server and add admins.");
        if (!added.empty()) {
            std::string names;
            for (const auto &name : added) names += (names.empty() ? "" : ", ") + name;
            write_log("Added new settings to " + path_utf8(config_file.filename()) + " with their defaults: " + names + ".");
        }
    } catch (const std::exception &e) {
        write_log("Cannot read " + path_utf8(config_file) + ": " + e.what());
        return 1;
    }
    // Maps: the retail ones and custom maps from Mods\<mod>\reskate-levels.json.
    for (const auto &problem : load_levels(here / "Mods")) write_log("Mods: skipped " + problem);
    if (levels().size() > 6) write_log("Mods: " + std::to_string(levels().size() - 6) + " custom map(s).");
    // Older configs name the map by its full destination; keep the plain name instead.
    bool renamed{};
    if (const auto setting = map_setting(config.map); setting != config.map && !setting.empty()) {
        config.map = setting;
        renamed = true;
    }
    for (auto &map : config.map_pool) // short pool names ("isle") are saved in full
        if (const auto *level = find_level(map); level && level->name != map) {
            map = level->name;
            renamed = true;
        }
    if (renamed) try { save_config(config); } catch (...) {}
    if (const auto error = config_error(config); !error.empty()) {
        write_log("Config problem: " + error);
        return 1;
    }
    // Updates: at startup, then every half hour while running (installed once
    // nobody is on). --no-update or "auto_update": false turns them off.
    remove_previous_update(here);
    bool auto_update = config.auto_update && updates_enabled() && !skip_update;
    for (int i = 1; i < argc; ++i)
#ifdef _WIN32
        if (std::wstring(argv[i]) == L"--no-update") auto_update = false;
#else
        if (std::string(argv[i]) == "--no-update") auto_update = false;
#endif
    if (auto_update) {
        write_log("Checking for updates...");
        const auto check = check_for_update();
        if (check.available) {
            update_version = check.version;
            write_log("Server update " + update_version + " found.");
            return restart_for_update;
        }
        write_log(check.problem.empty() ? "The server is up to date (" + check.version + ")."
                                        : "Update check skipped: " + check.problem + ".");
    }
    // World layers are optional: without the players' catalog every player keeps their own.
    if (const auto catalog = here / "world-layers.json"; std::filesystem::exists(catalog)) {
        try {
            install_world_layer_catalog(world_layer_scan::read(catalog));
            write_log("World layers: " + std::to_string(world_layers().size()) + " from world-layers.json.");
        } catch (const std::exception &e) {
            write_log(std::string("world-layers.json is unreadable; world layer sync is off: ") + e.what());
        }
    }

    SteamServer steam;
    std::string error;
    if (!steam.start(here, config.port, config.query_port, error)) {
        write_log(error);
        return 1;
    }
    write_log("Signing in to Steam...");
    const auto login_started = std::chrono::steady_clock::now();
    while (!steam.logged_on() && !stopping) {
        steam.run_callbacks();
        if (std::chrono::steady_clock::now() - login_started > std::chrono::seconds(60)) {
            write_log("Steam sign-in timed out after 60 s. Check the internet connection and try again.");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stopping) return 0;

    multiplayer::SteamTransport transport;
    if (!transport.open_game_server(steam.module())) {
        write_log("Steam networking failed: " + transport.status().detail);
        return 1;
    }
    Host host(config, transport, write_log);
    if (!host.start(error)) {
        write_log("Could not open the server: " + error);
        return 1;
    }
    write_log(config.name + " is up on " + host.map_name() + " for " + std::to_string(config.max_players) + " players.");
    write_log("Steam ID " + std::to_string(steam.steam_id()) + ", public IP " + steam.public_ip() + ".");
    write_log("Join code: " + host.invite() + (config.password.empty() ? "" : " (password required)"));
    write_log(config.admins.empty() ? "No admins yet: type \"admin add <SteamID64>\" to add one."
                              : std::to_string(config.admins.size()) + " admin(s). Type help for commands.");

    auto &input = console_input();
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    auto next_advertise = std::chrono::steady_clock::now();
    std::optional<bool> name_allowed; // last seen: whether the name may be listed
    auto next_update_check = next_advertise + update_interval;
    std::future<UpdateCheck> update_check;
    bool update_now{}, update_waiting{}, restart{};
    // The backend's ban list (global_bans.h): read now and every ten minutes, a minute after a
    // failure. "global_bans": false leaves it unread and lets those players in.
    std::future<BanListCheck> ban_check;
    auto next_ban_check = next_advertise;
    bool bans_unread{};
    if (!config.global_bans) write_log("Global bans are off (\"global_bans\": false): only this server's own bans apply.");
    while (!stopping && !restart) {
        steam.run_callbacks();
        try {
            host.tick(multiplayer::now_us());
        } catch (const std::exception &e) {
            write_log(std::string("Server error: ") + e.what());
        }
        for (const auto &line : input.take()) {
            if (line == "quit" || line == "exit" || line == "stop") {
                stopping = true;
                break;
            }
            if (line.empty()) continue;
            // "update": check now and install straight away, even with players on.
            if (line == "update") {
                if (!updates_enabled()) {
                    write_log("This is a local build; it doesn't update itself.");
                } else if (update_waiting) {
                    write_log("Restarting to install server update " + update_version + ".");
                    restart = true;
                } else {
                    update_now = true;
                    if (!update_check.valid()) update_check = std::async(std::launch::async, check_for_update);
                    write_log("Checking for updates...");
                }
                continue;
            }
            try {
                write_log(host.command(line));
            } catch (const std::exception &e) {
                write_log(std::string("Command failed: ") + e.what());
            }
        }
        const auto now_time = std::chrono::steady_clock::now();
        if (auto_update && !update_waiting && !update_check.valid() && now_time >= next_update_check)
            update_check = std::async(std::launch::async, check_for_update);
        if (update_check.valid() && update_check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto check = update_check.get();
            next_update_check = now_time + update_interval;
            if (check.available) {
                update_version = check.version;
                update_waiting = true;
                if (!update_now)
                    write_log("Server update " + update_version + " is ready; the server restarts to install it when nobody is on.");
            } else if (update_now) {
                write_log(check.problem.empty() ? "The server is up to date (" + check.version + ")."
                                                : "Update check failed: " + check.problem + ".");
            }
            if (update_waiting && update_now) {
                write_log("Restarting to install server update " + update_version + ".");
                restart = true;
            }
            update_now = false;
        }
        if (config.global_bans && !ban_check.valid() && now_time >= next_ban_check)
            ban_check = std::async(std::launch::async, read_global_bans);
        if (ban_check.valid() && ban_check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto check = ban_check.get();
            next_ban_check = now_time + (check.ok ? std::chrono::minutes(10) : std::chrono::minutes(1));
            // Said when it changes, not every ten minutes.
            if (check.ok && (check.changed || bans_unread))
                write_log("Global bans: " + std::to_string(check.banned) + " player(s) banned from ReSkate multiplayer cannot join.");
            else if (!check.ok && !bans_unread)
                write_log("The global ban list could not be read (" + check.problem + "). Trying again every minute; " +
                          "until then the bans already read hold.");
            bans_unread = !check.ok;
        }
        if (update_waiting && !restart && host.players() == 0) {
            write_log("Nobody is on; restarting to install server update " + update_version + ".");
            restart = true;
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_advertise) {
            next_advertise = now + std::chrono::seconds(2);
            // A name with a bad word in it is never listed (clients hide one too); the
            // server still runs and players can join with its code.
            const bool allowed = !text::contains_bad_words(config.name);
            if (name_allowed != allowed) {
                if (!allowed)
                    write_log("The server name \"" + config.name + "\" contains blocked words, so the server is not listed "
                              "in the server browser. Rename it with: name <new name>");
                else if (name_allowed)
                    write_log(config.listed ? "The server name is allowed again; the server is listed."
                                            : "The server name is allowed again (the server is still set to unlisted).");
                name_allowed = allowed;
            }
            steam.advertise({config.name, host.map_name(), host.players(), config.max_players, !config.password.empty(),
                             config.listed && allowed, host.secret()});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#ifdef _WIN32
    timeEndPeriod(1);
#else
    // Join the console reader before touching Steam shutdown and process exit:
    // it holds no stream locks, and poll() notices stopping within ~200 ms.
    input.shutdown();
#endif
    if (update_check.valid()) update_check.wait();
    if (ban_check.valid()) ban_check.wait();
    write_log(restart ? "Restarting for an update." : "Shutting down.");
    host.stop(restart ? "The server is restarting for an update. Rejoin in a minute." : "The server is shutting down.");
    // Leaving scope closes the networking before Steam itself shuts down.
    return restart ? restart_for_update : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    bool skip_update{};
    int code{};
    for (;;) {
        code = run(argc, argv, skip_update);
        if (code != restart_for_update) break;
        // Steam has shut down, so every server file can be replaced now.
        try {
            write_log("Installing server update " + update_version + "...");
            install_update(folder());
            write_log("Server update " + update_version + " installed; starting it.");
            console().flush(std::chrono::seconds(2));
            if (relaunch()) {
                code = 0;
                break;
            }
            write_log("Could not start the updated server; start ReSkateServer.exe again.");
            code = 1;
            break;
        } catch (const std::exception &e) {
            write_log(std::string("Server update failed (") + e.what() + "); carrying on with this version.");
            skip_update = true;
        }
    }
    finished = true;
    // Keep a failure on screen until the host has read it, instead of the window
    // vanishing with it. Closing the window or Ctrl+C still ends it at once.
    if (code != 0 && !stopping && own_window()) {
        auto &input = console_input();
        input.take();
        console().write("Press Enter to close.\n");
        while (!stopping && input.take().empty()) Sleep(50);
    }
    console().flush(std::chrono::seconds(2));
    return code;
}
#else
int main(int argc, char **argv) {
    bool skip_update{};
    int code{};
    for (;;) {
        code = run(argc, argv, skip_update);
        if (code != restart_for_update) break;
        try {
            write_log("Installing server update " + update_version + "...");
            install_update(folder());
            write_log("Server update " + update_version + " installed; starting it.");
            console().flush(std::chrono::seconds(2));
            if (relaunch()) {
                code = 0;
                break;
            }
            write_log("Could not start the updated server; start ReSkateServer again.");
            code = 1;
            break;
        } catch (const std::exception &e) {
            write_log(std::string("Server update failed (") + e.what() + "); carrying on with this version.");
            skip_update = true;
        }
    }
    finished = true;
    console().flush(std::chrono::seconds(2));
    return code;
}
#endif
