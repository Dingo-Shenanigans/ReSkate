#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// RESKATE_DATA_ROOT is the whole ReSkate user-data folder, not LocalAppData. A requested
// override is never replaced with the normal profile on failure. Keep ordinary defaults at
// their original locations; the Known Folder variant preserves older feature preferences.
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
    if (FAILED(result) || !value) throw std::runtime_error("Local AppData is unavailable");
    const std::filesystem::path local(value);
    CoTaskMemFree(value);
    return local / L"ReSkate";
}
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
// This configuration authorizes read-only feature selection only. No path is
// exposed: every file consumer must still use override_root()/root() for fresh
// junction, overlap and filesystem validation at its existing I/O boundary.
struct RuntimeDevelopmentConfiguration {
    std::wstring root, filename;
    std::optional<std::wstring> local;
};
struct RuntimeDevelopmentSelection {
    std::atomic<bool> attempted{}, revoked{};
    std::atomic<const RuntimeDevelopmentConfiguration*> published{};
    std::optional<RuntimeDevelopmentConfiguration> configuration;
};
inline RuntimeDevelopmentSelection& runtime_development_selection() noexcept {
    static RuntimeDevelopmentSelection value;
    return value;
}
// No allocation, filesystem query or per-call 64 KiB initialization. The
// process environment may change between calls; every observed mismatch is
// sticky. This is an identity observation, never filesystem freshness proof.
inline bool environment_matches(const wchar_t* name, const std::optional<std::wstring>& expected) noexcept {
    SetLastError(ERROR_SUCCESS);
    const auto required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) {
        const auto error = GetLastError();
        return expected ? expected->empty() && error == ERROR_SUCCESS : error == ERROR_ENVVAR_NOT_FOUND;
    }
    if (!expected || required > 32768 || required != expected->size() + 1) return false;
    thread_local wchar_t buffer[32768];
    const auto length = GetEnvironmentVariableW(name, buffer, required);
    return length == expected->size() && std::wstring_view(buffer, length) == *expected;
}
} // namespace detail

// Only a successful first initialization can select a runtime development
// configuration. Unknown/uninitialized/failed/observably changed means false.
// The optional expected root is input only, for a consumer's already validated
// session root; this API cannot supply an I/O destination.
inline bool runtime_development_selected(std::wstring_view filename = {}, std::wstring_view expected_root = {}) noexcept {
    struct PreserveError { DWORD value = GetLastError(); ~PreserveError() { SetLastError(value); } } error;
    auto& selection = detail::runtime_development_selection();
    if (selection.revoked.load(std::memory_order_acquire)) return false;
    const auto* configuration = selection.published.load(std::memory_order_acquire);
    if (!configuration) return false;
    // root has already passed the 180-character canonical path limit. Avoid
    // constructing an optional/string on this hot path.
    SetLastError(ERROR_SUCCESS);
    const auto required = GetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr, 0);
    wchar_t root[181];
    const bool root_matches = required == configuration->root.size() + 1 && required <= std::size(root) &&
        GetEnvironmentVariableW(L"RESKATE_DATA_ROOT", root, required) == configuration->root.size() &&
        std::wstring_view(root, configuration->root.size()) == configuration->root;
    if (!root_matches || !detail::environment_matches(L"LOCALAPPDATA", configuration->local)) {
        selection.revoked.store(true, std::memory_order_release);
        return false;
    }
    return !selection.revoked.load(std::memory_order_acquire) &&
        (filename.empty() || (filename.size() == configuration->filename.size() &&
            _wcsnicmp(filename.data(), configuration->filename.data(), filename.size()) == 0)) &&
        (expected_root.empty() || expected_root == configuration->root);
}

inline bool development_requested() noexcept {
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr, 0);
    return length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

// Public for focused path-validation tests. Resolves existing junctions as well as '..'.
inline std::filesystem::path validate_override(std::wstring_view value) {
    if (value.empty() || value.find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("RESKATE_DATA_ROOT requires an absolute development directory");
    const std::filesystem::path supplied(value);
    if (!supplied.is_absolute()) throw std::runtime_error("RESKATE_DATA_ROOT must be absolute");
    const auto path = detail::canonical(supplied);
    if (path == path.root_path()) throw std::runtime_error("RESKATE_DATA_ROOT cannot be a drive root");
    // The legacy game folder API has a MAX_PATH buffer. Reserve room for Game/Skate/data
    // and save names so its redirection cannot silently fall through to the live profile.
    if (path.native().size() > 180) throw std::runtime_error("RESKATE_DATA_ROOT is too long for game user-data paths");
    const auto reject_overlap = [&](const std::filesystem::path& original) {
        const auto normal = detail::canonical(original);
        if (detail::within(path, normal) || detail::within(normal, path))
            throw std::runtime_error("RESKATE_DATA_ROOT must be separate from the normal ReSkate user data");
    };
    const auto local = detail::environment(L"LOCALAPPDATA");
    if (local && !local->empty()) reject_overlap(std::filesystem::path(*local) / L"ReSkate");
    reject_overlap(detail::normal_known_root());
    return path;
}
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
inline std::filesystem::path launcher_settings(const std::filesystem::path& executable_directory) {
    if (const auto value = override_root()) return *value / L"ReSkateLauncher.settings.json";
    return executable_directory / L"ReSkateLauncher.settings.json";
}

// Must run before launcher/runtime logging, cache/profile reads or game resume. The check
// writes only beneath the explicit root and never creates or migrates the normal profile.
inline void initialize_development_root() {
    auto& selection = detail::runtime_development_selection();
    const bool first = !selection.attempted.exchange(true, std::memory_order_acq_rel);
    // Repeated bootstrap performs the original preflight, but can never arm a
    // missing/failed/revoked first selection or hide an observed env change.
    if (!first && !runtime_development_selected()) selection.revoked.store(true, std::memory_order_release);
    try {
        const auto local = first ? detail::environment(L"LOCALAPPDATA") : std::optional<std::wstring>{};
        const auto value = override_root();
        if (!value) { selection.revoked.store(true, std::memory_order_release); return; }
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
        if (first && !selection.revoked.load(std::memory_order_acquire)) {
            if (!detail::environment_matches(L"LOCALAPPDATA", local)) {
                selection.revoked.store(true, std::memory_order_release);
                return;
            }
            selection.configuration.emplace(detail::RuntimeDevelopmentConfiguration{value->native(), value->filename().native(), local});
            selection.published.store(&*selection.configuration, std::memory_order_release);
        }
    } catch (...) {
        selection.revoked.store(true, std::memory_order_release);
        throw;
    }
}

// Remove this launcher-only flag before parse_launch_options can forward unknown flags to
// Skate.exe. An inherited RESKATE_DATA_ROOT also works for launcher relaunch/crash helpers.
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
