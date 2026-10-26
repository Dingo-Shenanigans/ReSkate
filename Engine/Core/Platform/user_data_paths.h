#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <array>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Where ReSkate keeps what it stores for the player: %LOCALAPPDATA%\ReSkate, or the folder
// RESKATE_DATA_ROOT names (the launcher's --dev-data-root sets it). That variable replaces the whole
// ReSkate folder, not LocalAppData, so a test profile never shares a file with the player's own.
// A folder that was asked for and cannot be used is an error everywhere: nothing falls back to the
// normal profile. Without the variable every path is where it always was; known_folder_root() is
// for the features that have always found LocalAppData through the Known Folder API.
namespace dingosdk::user_data {
namespace detail {
inline std::optional<std::wstring> environment(const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return {};
    if (length >= value.size()) throw std::runtime_error("User-data environment value is too long");
    return std::wstring(value.data(), length);
}
inline std::filesystem::path normal_root() {
    const auto local = environment(L"LOCALAPPDATA");
    if (!local || local->empty()) throw std::runtime_error("LOCALAPPDATA is unavailable");
    return std::filesystem::path(*local) / L"ReSkate";
}
inline std::filesystem::path normal_known_root() {
    PWSTR value{};
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &value);
    if (FAILED(result) || !value) {
        CoTaskMemFree(value);
        throw std::runtime_error("Local AppData is unavailable");
    }
    const std::filesystem::path local(value);
    CoTaskMemFree(value);
    return local / L"ReSkate";
}
// Whether path is parent or below it, comparing folder names as Windows does (case-insensitively).
inline bool within(const std::filesystem::path& path, const std::filesystem::path& parent) {
    auto p = path.begin();
    for (auto q = parent.begin(); q != parent.end(); ++q, ++p)
        if (p == path.end() || _wcsicmp(p->c_str(), q->c_str()) != 0) return false;
    return true;
}
inline std::filesystem::path canonical(const std::filesystem::path& path) {
    std::error_code error;
    auto value = std::filesystem::weakly_canonical(path, error);
    if (error || value.empty()) throw std::runtime_error("Cannot resolve RESKATE_DATA_ROOT");
    return value;
}
inline void set_environment(const wchar_t* name, const std::filesystem::path& value) {
    if (!SetEnvironmentVariableW(name, value.c_str())) throw std::runtime_error("Cannot configure isolated user-data environment");
}
} // namespace detail

// True when RESKATE_DATA_ROOT is set at all, even to something unusable: such a launch must not
// quietly use the normal profile.
inline bool development_requested() noexcept {
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr, 0);
    return length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

// The checked folder for a requested root (public for the path tests). Junctions and '..' are
// resolved first, so a link into the normal profile is refused as well.
inline std::filesystem::path validate_override(std::wstring_view value) {
    if (value.empty() || value.find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("RESKATE_DATA_ROOT requires an absolute development directory");
    const std::filesystem::path supplied(value);
    if (!supplied.is_absolute()) throw std::runtime_error("RESKATE_DATA_ROOT must be absolute");
    const auto path = detail::canonical(supplied);
    if (path == path.root_path()) throw std::runtime_error("RESKATE_DATA_ROOT cannot be a drive root");
    // The game's older folder API has a MAX_PATH buffer. This leaves room for Game\Skate\data and
    // the save names under it, so the game's own redirect cannot fall through to the live profile.
    if (path.native().size() > 180) throw std::runtime_error("RESKATE_DATA_ROOT is too long for game user-data paths");
    const auto reject_overlap = [&](const std::filesystem::path& original) {
        const auto normal = detail::canonical(original);
        if (detail::within(path, normal) || detail::within(normal, path))
            throw std::runtime_error("RESKATE_DATA_ROOT must be separate from the normal ReSkate user data");
    };
    // Both ways the normal folder can be found, since a process's LOCALAPPDATA can differ from the
    // Known Folder.
    const auto local = detail::environment(L"LOCALAPPDATA");
    if (local && !local->empty()) reject_overlap(std::filesystem::path(*local) / L"ReSkate");
    reject_overlap(detail::normal_known_root());
    return path;
}
// The requested root, checked again on every call: nothing is cached that could go stale.
inline std::optional<std::filesystem::path> override_root() {
    const auto value = detail::environment(L"RESKATE_DATA_ROOT");
    if (!value) return {};
    return validate_override(*value);
}
inline std::filesystem::path root() {
    if (const auto value = override_root()) return *value;
    return detail::normal_root();
}
inline std::filesystem::path known_folder_root() {
    if (const auto value = override_root()) return *value;
    return detail::normal_known_root();
}
// The launcher's settings stay beside the launcher, unless a separate root is in use.
inline std::filesystem::path launcher_settings(const std::filesystem::path& executable_directory) {
    if (const auto value = override_root()) return *value / L"ReSkateLauncher.settings.json";
    return executable_directory / L"ReSkateLauncher.settings.json";
}

// With a requested root: checks it can be written, makes its logs and tmp folders, and points the
// process's log, favourites and temporary-file variables into it, which the game inherits. It must
// run before logging starts and before any cache or profile is read. It writes only beneath the
// requested folder, and never creates or changes the normal profile. Without one it does nothing.
inline void initialize_development_root() {
    const auto value = override_root();
    if (!value) return;
    for (const auto* name : {L"", L"logs", L"tmp"}) std::filesystem::create_directories(*value / name);
    const auto probe = *value / (L".write-check-" + std::to_wstring(GetCurrentProcessId()));
    const auto file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("RESKATE_DATA_ROOT is not writable");
    CloseHandle(file);
    detail::set_environment(L"RESKATE_DATA_ROOT", *value);
    detail::set_environment(L"RESKATE_LOG_DIRECTORY", *value / L"logs");
    detail::set_environment(L"RESKATE_FAVORITES_FILE", *value / L"ReSkate.favorites.json");
    detail::set_environment(L"TEMP", *value / L"tmp");
    detail::set_environment(L"TMP", *value / L"tmp");
}

// Takes --dev-data-root <folder> or --dev-data-root=<folder> out of the launcher's arguments before
// they are parsed, since unknown flags are passed on to Skate.exe, and sets RESKATE_DATA_ROOT from
// it. The variable is inherited, so the launcher's relaunches and crash helper use the same root.
inline void consume_development_root_option(std::vector<std::wstring>& arguments) {
    std::optional<std::wstring> requested;
    for (std::size_t index = 0; index < arguments.size();) {
        const std::wstring_view argument(arguments[index]);
        if (argument != L"--dev-data-root" && !argument.starts_with(L"--dev-data-root=")) { ++index; continue; }
        if (requested) throw std::runtime_error("--dev-data-root can be provided only once");
        if (argument == L"--dev-data-root") {
            if (index + 1 >= arguments.size()) throw std::runtime_error("--dev-data-root requires a directory");
            requested = arguments[index + 1];
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index + 1));
        } else requested = std::wstring(argument.substr(16));
        arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
    }
    if (requested) detail::set_environment(L"RESKATE_DATA_ROOT", validate_override(*requested));
}
} // namespace dingosdk::user_data
