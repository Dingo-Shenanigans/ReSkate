#include "Engine/Core/Log/types.h"
#include "launcher_support.h"
#include "launcher_support_internal.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace dingosdk::launcher {
namespace {
using detail::fail;
} // namespace

#ifdef _WIN32
bool bindable_key(unsigned key) noexcept {
    if (key == 0 || key > 0xFE) return false;
    switch (key) {
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2: case VK_CANCEL:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_ESCAPE: case VK_RETURN: case VK_TAB: case VK_SPACE:
    case VK_BACK: case VK_PACKET: case VK_PROCESSKEY:
        return false;
    default:
        return true;
    }
}

std::string key_name(unsigned key) {
    switch (key) {
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_PAUSE: return "Pause";
    case VK_SCROLL: return "Scroll Lock";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_DIVIDE: return "Num /";
    case VK_OEM_3: return "` / ~";
    default: break;
    }
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return "Num " + std::to_string(key - VK_NUMPAD0);
    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    std::array<wchar_t, 64> text{};
    const auto length = scan ? GetKeyNameTextW(static_cast<LONG>(scan << 16), text.data(), static_cast<int>(text.size())) : 0;
    if (length <= 0) return "Key " + std::to_string(key);
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

std::string key_cap(unsigned key) {
    switch (key) {
    case VK_INSERT: return "INS";
    case VK_DELETE: return "DEL";
    case VK_PRIOR: return "PGUP";
    case VK_NEXT: return "PGDN";
    default: break;
    }
    auto name = key_name(key);
    for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
    return name;
}

OverlayKeys overlay_keys() noexcept {
    static const OverlayKeys keys = [] {
        const auto incoming_error = GetLastError();
        const auto read = [](const wchar_t* name, unsigned fallback) {
            wchar_t value[16]{};
            const auto length = GetEnvironmentVariableW(name, value, 16);
            if (!length || length >= 16) return fallback;
            wchar_t* end{};
            const auto parsed = std::wcstoul(value, &end, 0);
            return end && *end == L'\0' && bindable_key(parsed) ? static_cast<unsigned>(parsed) : fallback;
        };
        OverlayKeys result{read(L"RESKATE_MENU_KEY", default_menu_key), read(L"RESKATE_CONSOLE_KEY", default_console_key)};
        if (result.menu == result.console) result = {};
        SetLastError(incoming_error);
        return result;
    }();
    return keys;
}

#endif // _WIN32

bool offline_mode() noexcept {
#ifdef _WIN32
    static const bool offline = [] {
        const auto incoming_error = GetLastError();
        wchar_t value[2]{};
        const auto length = GetEnvironmentVariableW(L"RESKATE_OFFLINE", value, 2);
        SetLastError(incoming_error);
        return length == 1 && value[0] == L'1';
    }();
    return offline;
#else
    static const bool offline = [] {
        const char *value = std::getenv("RESKATE_OFFLINE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return offline;
#endif
}

std::uint64_t offline_steam_id() noexcept {
    static const std::uint64_t id = [] {
        constexpr std::uint64_t fallback = 0x0110000100000001ull;
#ifdef _WIN32
        const auto incoming_error = GetLastError();
        char value[32]{};
        const auto length = GetEnvironmentVariableA("RESKATE_OFFLINE_STEAM_ID", value, sizeof(value));
        SetLastError(incoming_error);
        if (!length || length >= sizeof(value)) return fallback;
#else
        const char *env = std::getenv("RESKATE_OFFLINE_STEAM_ID");
        if (!env || !env[0]) return fallback;
        char value[32]{};
        std::size_t length = 0;
        for (; env[length] && length < sizeof(value) - 1; ++length) value[length] = env[length];
        if (env[length]) return fallback;
#endif
        char* end{};
        const auto parsed = std::strtoull(value, &end, 10);
        // Individual accounts in the public universe only.
        if (!end || *end != '\0' || (static_cast<std::uint64_t>(parsed) >> 52) != 0x011) return fallback;
        return static_cast<std::uint64_t>(parsed);
    }();
    return id;
}

#ifdef _WIN32
namespace {
bool map_option(std::wstring_view argument) {
    return argument == L"-map" || argument == L"--map" || argument.starts_with(L"-map=") || argument.starts_with(L"--map=");
}
// The name goes into a console command the game runs: one line of plain text.
std::wstring map_name(std::wstring_view value) {
    while (!value.empty() && value.front() == L' ') value.remove_prefix(1);
    while (!value.empty() && value.back() == L' ') value.remove_suffix(1);
    if (value.empty() || value.size() > 96) fail("-map requires a map's name, at most 96 characters");
    for (const auto character : value)
        if (character < 0x20 || character > 0x7e || character == L';' || character == L'"')
            fail("-map takes a map's name in plain letters, digits and spaces");
    return std::wstring(value);
}
} // namespace

std::wstring take_map_option(std::vector<std::wstring>& arguments) {
    std::wstring map;
    for (std::size_t index = 0; index < arguments.size();) {
        const std::wstring_view argument(arguments[index]);
        if (!map_option(argument)) { ++index; continue; }
        const auto equals = argument.find(L'=');
        if (equals != std::wstring_view::npos) map = map_name(argument.substr(equals + 1));
        else {
            if (index + 1 == arguments.size()) fail("-map requires a map's name");
            map = map_name(arguments[index + 1]);
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        }
        arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return map;
}

LaunchOptions parse_launch_options(const std::vector<std::wstring>& arguments) {
    LaunchOptions options;
    bool log_level_explicit = false;
    const auto dimension = [](std::wstring_view value, unsigned minimum) {
        unsigned result = 0;
        if (value.empty()) fail("Resolution requires a decimal pixel count");
        for (const auto character : value) {
            if (character < L'0' || character > L'9' || result > 16384)
                fail("Resolution requires a decimal pixel count, at most 16384");
            result = result * 10 + static_cast<unsigned>(character - L'0');
        }
        if (result < minimum || result > 16384)
            fail("Resolution must be at least 320 x 200 and at most 16384 x 16384");
        return result;
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--no-loose-files") { options.loose_files = false; continue; }
        if (argument == L"-offline" || argument == L"--offline") { options.offline = true; continue; }
        if (argument == L"-wconsole") { options.window_console = true; continue; }
        if (argument == L"--log-trace") { options.log_trace = true; continue; }
        if (argument == L"--log-level" || argument.starts_with(L"--log-level=")) {
            const auto equals = argument.find(L'=');
            std::wstring_view value;
            if (equals != std::wstring_view::npos) value = argument.substr(equals + 1);
            else {
                if (++index == arguments.size()) fail("--log-level requires a severity");
                value = arguments[index];
            }
            std::string level;
            for (auto ch : value) {
                if (ch < L'a' || ch > L'z') fail("Invalid --log-level");
                level += static_cast<char>(ch);
            }
            if (!logging::parse_level(level)) fail("--log-level requires trace, debug, info, warning, error, critical, or off");
            options.log_level = std::move(level); log_level_explicit = true; continue;
        }
        if (map_option(argument)) {
            const auto equals = argument.find(L'=');
            if (equals != std::wstring_view::npos) options.map = map_name(argument.substr(equals + 1));
            else {
                if (++index == arguments.size()) fail("-map requires a map's name");
                options.map = map_name(arguments[index]);
            }
            continue;
        }
        if (argument == L"--gpu-diagnostics") { options.gpu_diagnostics = true; continue; }
        if (argument == L"--no-discord") { options.discord = false; continue; }
        const bool menu_key = argument.starts_with(L"--menu-key=");
        if (menu_key || argument.starts_with(L"--console-key=")) {
            const std::wstring value(argument.substr(argument.find(L'=') + 1));
            wchar_t* end{};
            const auto key = std::wcstoul(value.c_str(), &end, 0);
            if (value.empty() || !end || *end != L'\0' || !bindable_key(key))
                fail("--menu-key and --console-key take a virtual-key code such as 0x2D (Insert)");
            (menu_key ? options.menu_key : options.console_key) = static_cast<unsigned>(key);
            continue;
        }
        // Translate the friendly flag into the inspected engine settings below.
        if (argument == L"-windowed" || argument == L"--windowed") {
            options.force_windowed = true;
            continue;
        }
        const bool width = argument == L"--width" || argument.starts_with(L"--width=");
        const bool height = argument == L"--height" || argument.starts_with(L"--height=");
        if (width || height) {
            options.force_windowed = true;
            const auto equals = argument.find(L'=');
            std::wstring_view value;
            if (equals != std::wstring_view::npos) value = argument.substr(equals + 1);
            else {
                if (++index == arguments.size()) fail("--width and --height require a pixel count");
                value = arguments[index];
            }
            if (width) options.width = dimension(value, 320);
            else options.height = dimension(value, 200);
            continue;
        }
        options.game_arguments.emplace_back(argument);
    }
    if (options.log_trace && !log_level_explicit) options.log_level = "trace";
    return options;
}

std::wstring windowed_arguments(const LaunchOptions& options) {
    if (!options.force_windowed) return {};
    return L" -Window.Width " + std::to_wstring(options.width) +
        L" -Window.Height " + std::to_wstring(options.height) +
        L" -Window.AutoSize 0 -Window.FullscreenAutoSize 0"
        L" -RenderDevice.FullscreenModeEnable 0 -RenderDevice.WindowedBorderless 0";
}

std::wstring mod_data_arguments(const fs::path& game_directory,
                               const std::vector<std::wstring>& game_arguments) {
    for (const auto& argument : game_arguments) {
        const std::wstring_view key(argument.data(), argument.find(L'=') == std::wstring::npos
            ? argument.size() : argument.find(L'='));
        if (key.size() == 9 && CompareStringOrdinal(key.data(), 9, L"-dataPath", 9, TRUE) == CSTR_EQUAL)
            return {};
    }
    return fs::is_directory(game_directory / L"ModData")
        ? L" -dataPath \"ModData/Default\"" : L"";
}

SiblingPaths sibling_paths(const fs::path& launcher) {
    const auto directory = launcher.parent_path();
    return {directory, directory / L"Skate.exe", directory / L"ReSkate.dll",
            directory / L"steam_api64.dll", directory / L"logs"};
}

#endif // _WIN32

} // namespace dingosdk::launcher
