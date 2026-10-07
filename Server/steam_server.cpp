#include "steam_server.h"
#include "server_text.h"
#include "Extension/Multiplayer/Net/protocol.h"
#ifdef _WIN32
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <chrono>
#include <dlfcn.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace dingosdk::server {
namespace {
constexpr std::uint32_t skate_app = 3354750;
template <class T> T symbol(void *module, const char *name) {
#ifdef _WIN32
    const auto result = GetProcAddress(static_cast<HMODULE>(module), name);
#else
    const auto result = dlsym(module, name);
#endif
    if (!result) throw std::runtime_error(std::string("Missing Steam export: ") + name);
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(result);
#pragma warning(pop)
#else
    T value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
#endif
}
struct SteamIP {
    std::uint8_t ip[16];
    int type; // 0: IPv4 in the first four bytes, host order
};
// Tags are comma separated, so a name loses its commas; the whole list must
// stay under Steam's 128 byte limit.
std::string tag_text(std::string_view text, std::size_t limit) {
    std::string result(text);
    std::replace(result.begin(), result.end(), ',', ' ');
    // Cut at the limit only, and never through a character: a whole last character
    // is kept ("Café" stays "Café"), one the limit splits is dropped.
    cut_text(result, limit);
    return result;
}
// Steam's library prints its own crash-reporting notes ("Setting breakpad
// minidump AppID", "Caching Steam ID") while it loads and starts. Point stdout
// and stderr at NUL for just that, for both the C runtime and the OS
// handles (the library may write through either), then put them back.
class QuietSteam {
  public:
#ifdef _WIN32
    QuietSteam() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (_sopen_s(&null_, "NUL", _O_WRONLY, _SH_DENYNO, 0) != 0) null_ = -1;
        if (null_ < 0) return;
        for (int stream = 0; stream < 2; ++stream) {
            handles_[stream] = GetStdHandle(stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
            saved_[stream] = _dup(stream + 1);
            if (saved_[stream] >= 0) _dup2(null_, stream + 1);
            SetStdHandle(stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(null_)));
        }
    }
    ~QuietSteam() {
        if (null_ < 0) return;
        std::fflush(stdout);
        std::fflush(stderr);
        for (int stream = 0; stream < 2; ++stream) {
            if (saved_[stream] >= 0) {
                _dup2(saved_[stream], stream + 1);
                _close(saved_[stream]);
            }
            SetStdHandle(stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, handles_[stream]);
        }
        _close(null_);
    }
#else
    QuietSteam() {
        std::fflush(stdout);
        std::fflush(stderr);
        null_ = open("/dev/null", O_WRONLY);
        if (null_ < 0) return;
        for (int stream = 0; stream < 2; ++stream) {
            saved_[stream] = dup(stream + 1);
            if (saved_[stream] >= 0) dup2(null_, stream + 1);
        }
    }
    ~QuietSteam() {
        if (null_ < 0) return;
        std::fflush(stdout);
        std::fflush(stderr);
        for (int stream = 0; stream < 2; ++stream) {
            if (saved_[stream] >= 0) {
                dup2(saved_[stream], stream + 1);
                close(saved_[stream]);
            }
        }
        close(null_);
    }
#endif
    QuietSteam(const QuietSteam &) = delete;
    QuietSteam &operator=(const QuietSteam &) = delete;

  private:
    int null_ = -1;
#ifdef _WIN32
    HANDLE handles_[2]{};
#endif
    int saved_[2]{-1, -1};
};
} // namespace
std::string server_tags(const Advertisement &a) {
    constexpr char hex[] = "0123456789abcdef";
    std::string secret;
    for (int shift = 60; shift >= 0; shift -= 4) secret += hex[(a.secret >> shift) & 15];
    auto tags = "reskate,v" + std::to_string(multiplayer::protocol_version) + ",k" + secret + ",p" +
                std::to_string(a.players) + ",c" + std::to_string(a.max_players) + (a.password ? ",w1" : ",w0");
    tags += ",m" + tag_text(a.map, 24);
    tags += ",n" + tag_text(a.name, 127 - tags.size() - 2);
    return tags;
}
SteamServer::~SteamServer() { stop(); }
bool SteamServer::start(const std::filesystem::path &folder, std::uint16_t port, std::uint16_t query_port,
                        const std::string &token, std::string &error) {
    try {
        std::optional<QuietSteam> quiet{std::in_place};
#ifdef _WIN32
        const auto library = folder / "steam_api64.dll";
        module_ = LoadLibraryW(library.c_str());
        if (!module_) throw std::runtime_error("Cannot load " + library.string() + ". Put steam_api64.dll next to the server.");
        // The game server reads its app from the environment, like the game does.
        SetEnvironmentVariableW(L"SteamAppId", L"3354750");
        SetEnvironmentVariableW(L"SteamGameId", L"3354750");
#else
        // Linux: Steamworks SDK libsteam_api.so (plus steamclient.so et al).
        const char *candidates[] = {"libsteam_api.so", "steam_api64.dll"};
        std::filesystem::path library;
        for (const char *name : candidates) {
            const auto attempt = folder / name;
            std::error_code exists_error;
            if (!std::filesystem::is_regular_file(attempt, exists_error) || exists_error) continue;
            module_ = dlopen(attempt.c_str(), RTLD_NOW);
            if (module_) {
                library = attempt;
                break;
            }
        }
        if (!module_) {
            // Also try system search path (e.g. LD_LIBRARY_PATH).
            module_ = dlopen("libsteam_api.so", RTLD_NOW);
            library = folder / "libsteam_api.so";
        }
        if (!module_) {
            const char *details = dlerror();
            throw std::runtime_error("Cannot load " + library.string() +
                                     ". Put libsteam_api.so (Steamworks SDK) next to the server." +
                                     (details && details[0] ? std::string(" ") + details : ""));
        }
        setenv("SteamAppId", "3354750", 1);
        setenv("SteamGameId", "3354750", 1);
#endif
        static const char versions[] =
            "SteamUtils010\0SteamNetworkingUtils004\0SteamGameServer015\0SteamNetworkingSockets012\0";
        char message[1024]{};
        const auto init = symbol<int (*)(std::uint32_t, std::uint16_t, std::uint16_t, int, const char *, const char *, char *)>(
            module_, "SteamInternal_GameServer_Init_V2");
        // 2: authentication mode. Players connect through Steam networking, not the game port.
        const auto result = init(0, port, query_port, 2, "1.0.0.0", versions, message);
        quiet.reset();
        if (result != 0)
#ifdef _WIN32
            throw std::runtime_error(std::string("Steam game server did not start: ") +
                                     (message[0] ? message : "are steamclient64.dll, tier0_s64.dll and vstdlib_s64.dll next to it?") +
                                     ". If another server is running on this PC, give this one different port and query_port.");
#else
            throw std::runtime_error(std::string("Steam game server did not start: ") +
                                     (message[0] ? message : "is steamclient.so next to libsteam_api.so?") +
                                     ". If another server is running on this machine, give this one different port and query_port.");
#endif
        started_ = true;
        server_ = symbol<void *(*)()>(module_, "SteamAPI_SteamGameServer_v015")();
        if (!server_) throw std::runtime_error("Steam game server interface is unavailable.");
        const auto text = [&](const char *name, const char *value) {
            symbol<void (*)(void *, const char *)>(module_, name)(server_, value);
        };
        text("SteamAPI_ISteamGameServer_SetModDir", "skate");
        text("SteamAPI_ISteamGameServer_SetProduct", "reskate");
        text("SteamAPI_ISteamGameServer_SetGameDescription", "ReSkate dedicated server");
        symbol<void (*)(void *, bool)>(module_, "SteamAPI_ISteamGameServer_SetDedicatedServer")(server_, true);
        if (token.empty()) symbol<void (*)(void *)>(module_, "SteamAPI_ISteamGameServer_LogOnAnonymous")(server_);
        else text("SteamAPI_ISteamGameServer_LogOn", token.c_str());
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        stop();
        return false;
    }
}
void SteamServer::run_callbacks() {
    if (started_) symbol<void (*)()>(module_, "SteamGameServer_RunCallbacks")();
}
bool SteamServer::logged_on() {
    return started_ && symbol<bool (*)(void *)>(module_, "SteamAPI_ISteamGameServer_BLoggedOn")(server_);
}
std::uint64_t SteamServer::steam_id() {
    return started_ ? symbol<std::uint64_t (*)(void *)>(module_, "SteamAPI_ISteamGameServer_GetSteamID")(server_) : 0;
}
std::string SteamServer::public_ip() {
    if (!started_) return {};
    const auto ip = symbol<SteamIP (*)(void *)>(module_, "SteamAPI_ISteamGameServer_GetPublicIP")(server_);
    if (ip.type != 0) return "IPv6";
    std::uint32_t value{};
    std::memcpy(&value, ip.ip, 4);
    return std::to_string(value >> 24) + "." + std::to_string((value >> 16) & 255) + "." +
           std::to_string((value >> 8) & 255) + "." + std::to_string(value & 255);
}
void SteamServer::advertise(const Advertisement &a) {
    if (!started_) return;
    const auto text = [&](const char *name, const std::string &value) {
        symbol<void (*)(void *, const char *)>(module_, name)(server_, value.c_str());
    };
    text("SteamAPI_ISteamGameServer_SetServerName", a.name);
    text("SteamAPI_ISteamGameServer_SetMapName", a.map.substr(0, 31));
    symbol<void (*)(void *, int)>(module_, "SteamAPI_ISteamGameServer_SetMaxPlayerCount")(server_, static_cast<int>(a.max_players));
    symbol<void (*)(void *, bool)>(module_, "SteamAPI_ISteamGameServer_SetPasswordProtected")(server_, a.password);
    const auto tags = server_tags(a);
    if (tags != tags_) {
        text("SteamAPI_ISteamGameServer_SetGameTags", tags);
        tags_ = tags;
    }
    symbol<void (*)(void *, bool)>(module_, "SteamAPI_ISteamGameServer_SetAdvertiseServerActive")(server_, a.listed);
}
void SteamServer::stop() {
    if (started_) {
        try {
            symbol<void (*)(void *, bool)>(module_, "SteamAPI_ISteamGameServer_SetAdvertiseServerActive")(server_, false);
            symbol<void (*)(void *)>(module_, "SteamAPI_ISteamGameServer_LogOff")(server_);
            for (int i = 0; i < 10; ++i) {
                run_callbacks();
#ifdef _WIN32
                Sleep(20);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
            }
            symbol<void (*)()>(module_, "SteamGameServer_Shutdown")();
        } catch (...) {
        }
        started_ = false;
    }
    server_ = nullptr;
#ifdef __linux__
    // Keep the Steam library loaded for the process lifetime (matches Windows);
    // dlclose here would invalidate in-flight callbacks during restart.
#endif
}
} // namespace dingosdk::server
