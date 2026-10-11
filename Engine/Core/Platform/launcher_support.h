#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include "Engine/Game/Build/supported_build.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::launcher {

inline constexpr std::uint64_t expected_game_file_size = supported_build::game_file_size;
inline constexpr std::uint32_t expected_game_image_size = supported_build::game_image_size;
inline constexpr std::string_view expected_game_sha256 = supported_build::game_sha256;
inline constexpr std::uint64_t expected_steam_api_file_size = supported_build::steam_api_file_size;
inline constexpr std::string_view expected_steam_api_sha256 = supported_build::steam_api_sha256;

struct SiblingPaths {
    std::filesystem::path directory;
    std::filesystem::path game;
    std::filesystem::path dll;
    std::filesystem::path steam_api;
    std::filesystem::path logs;
};

struct PeFileInfo {
    std::uint16_t machine{};
    std::uint16_t characteristics{};
    std::uint32_t image_size{};
    std::uint32_t headers_size{};
    bool pe64{};
};

#ifdef _WIN32
struct LaunchOptions {
    unsigned width{1280};
    unsigned height{720};
    bool force_windowed{};
    bool gpu_diagnostics{};
    bool discord{true}; // the game may show a Discord status (--no-discord: never)
    bool window_console{};
    bool log_trace{};
    bool loose_files{true};
    bool offline{};
    // Virtual-key codes that open ReSkate's menu and console (--menu-key /
    // --console-key); passed to the game as RESKATE_MENU_KEY / RESKATE_CONSOLE_KEY.
    unsigned menu_key{0x2D};     // VK_INSERT
    unsigned console_key{0xC0};  // VK_OEM_3, the key left of 1
    std::string log_level{"info"};
    // -map <name>: the map the game loads once it is up, named as the console's "load" names
    // one (a map's name, short name or asset path, or the start of one). Empty: none.
    std::wstring map;
    std::vector<std::wstring> game_arguments;
};

// Arguments exclude argv[0]. Saved game display settings apply by default.
// --windowed or an explicit width/height requests the windowed override.
LaunchOptions parse_launch_options(const std::vector<std::wstring>& arguments);
// Takes "-map <name>" (--map, -map=<name>) out of `arguments` and returns the name it gave,
// checked as parse_launch_options checks it; empty when there is none. For a launch whose
// other options come from somewhere else (the launcher's saved settings).
std::wstring take_map_option(std::vector<std::wstring>& arguments);
std::wstring windowed_arguments(const LaunchOptions& options);
// Detect mods beside Skate.exe. Explicit game data paths take precedence.
std::wstring mod_data_arguments(const std::filesystem::path& game_directory,
                               const std::vector<std::wstring>& game_arguments);

class LoaderGate {
public:
    LoaderGate() = default;
    ~LoaderGate();
    LoaderGate(const LoaderGate&) = delete;
    LoaderGate& operator=(const LoaderGate&) = delete;
    LoaderGate(LoaderGate&& other) noexcept;
    LoaderGate& operator=(LoaderGate&& other) noexcept;

    std::uintptr_t entrypoint() const noexcept { return entrypoint_; }
    std::size_t thread_count() const noexcept { return threads_.size(); }
    // Threads that were already exiting when the gate closed; they need no gate.
    std::size_t exiting_thread_count() const noexcept { return exiting_threads_; }
    void resume();

private:
    friend LoaderGate prepare_loader_for_injection(HANDLE, HANDLE, DWORD, std::uintptr_t);
    struct Thread {
        HANDLE handle{};
        DWORD id{};
        DWORD previous_suspend_count{};
        bool primary{};
    };
    std::vector<Thread> threads_;
    std::uintptr_t entrypoint_{};
    std::size_t exiting_threads_{};
    bool resumed_{};
};

SiblingPaths sibling_paths(const std::filesystem::path& launcher);
PeFileInfo inspect_pe_file(const std::filesystem::path& path);
std::uint32_t exported_function_rva(const std::filesystem::path& path,
                                    std::string_view export_name);
void validate_game_file(const std::filesystem::path& path);
#else
// Linux dedicated server: no PE image checks. Validation is a size/hash check
// in launcher_pe.cpp; PE helpers are Windows-only.
#endif
std::string sha256_file(const std::filesystem::path& path);
void validate_steam_api_file(const std::filesystem::path& path);

// -offline: the launcher sets RESKATE_OFFLINE=1 so the game runs without Steam,
// as "Unknown Player", with multiplayer hidden. Read once per process.
inline constexpr const char* offline_player_name = "Unknown Player";
bool offline_mode() noexcept;
// The SteamID offline mode reports: RESKATE_OFFLINE_STEAM_ID, the account
// Steam was last signed in to, so the game keeps the same settings save.
std::uint64_t offline_steam_id() noexcept;

#ifdef _WIN32
// Keys that open ReSkate's menu and console in game.
inline constexpr unsigned default_menu_key = 0x2D;     // VK_INSERT
inline constexpr unsigned default_console_key = 0xC0;  // VK_OEM_3
struct OverlayKeys { unsigned menu{default_menu_key}, console{default_console_key}; };
// The keys the launcher chose (RESKATE_MENU_KEY / RESKATE_CONSOLE_KEY), or
// the defaults. Read once per process.
OverlayKeys overlay_keys() noexcept;
// Whether a virtual key can open the menu or console: not a mouse button,
// modifier, Escape, Enter, Tab, Space or Backspace.
bool bindable_key(unsigned key) noexcept;
// "Insert", "F8", "`"... for settings and hints; `short_name` gives a keycap
// label such as "INS".
std::string key_name(unsigned key);
std::string key_cap(unsigned key);

// Temporarily gates the validated executable entrypoint of a process created
// with CREATE_SUSPENDED. This lets the Windows loader finish while preventing
// executable startup, then suspends every existing thread and restores the
// exact entrypoint bytes before returning.
LoaderGate prepare_loader_for_injection(HANDLE process, HANDLE primary_thread,
                                        DWORD process_id, std::uintptr_t image_base);

// Returns the ordinary LoadLibraryW address for a same-architecture suspended
// child only after verifying the matching KnownDLL mapping and entry bytes.
// An entry hooked by anti-virus or an overlay is allowed through when the rest
// of the function is still Windows' own; `note`, when given, is then filled
// with what was found so the caller can log it.
std::uintptr_t validated_remote_load_library(HANDLE process, std::string* note = nullptr);
#else
// Linux: virtual-key names and loader injection are Windows-only.
inline constexpr unsigned default_menu_key = 0x2D;
inline constexpr unsigned default_console_key = 0xC0;
struct OverlayKeys { unsigned menu{default_menu_key}, console{default_console_key}; };
inline OverlayKeys overlay_keys() noexcept { return {}; }
inline bool bindable_key(unsigned) noexcept { return false; }
inline std::string key_name(unsigned key) { return "Key " + std::to_string(key); }
inline std::string key_cap(unsigned key) { return key_name(key); }
#endif

} // namespace dingosdk::launcher
