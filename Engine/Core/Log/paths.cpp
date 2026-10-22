#include "logging_internal.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include <ShlObj.h>
#include <array>
#include <algorithm>
#include <stdexcept>

namespace dingosdk::logging {
namespace {
std::wstring environment(const wchar_t* name) {
    std::array<wchar_t, 32768> value{};
    const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    return count && count < value.size() ? std::wstring(value.data(), count) : std::wstring{};
}
}
std::filesystem::path log_directory(const std::filesystem::path& game_directory) {
    detail::PreserveError preserve;
    const auto attempt = [&](const std::filesystem::path& path) -> std::filesystem::path {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) return {};
        const auto probe = CreateFileW((path / std::format(L".write-check-{}", GetCurrentProcessId())).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (probe == INVALID_HANDLE_VALUE) return {};
        CloseHandle(probe);
        return path;
    };
    if (const auto isolated = user_data::override_root()) {
        if (auto path = attempt(*isolated / L"logs"); !path.empty()) return path;
        throw std::runtime_error("Cannot create the isolated log directory");
    }
    if (auto path = attempt(game_directory / L"logs"); !path.empty()) return path;
    PWSTR local{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local))) {
        const auto root = std::filesystem::path(local) / L"ReSkate" / L"logs";
        CoTaskMemFree(local);
        if (auto path = attempt(root); !path.empty()) return path;
    }
    throw std::runtime_error("Cannot create a log directory beside the game or in Local AppData");
}
Options options_from_environment() {
    detail::PreserveError preserve;
    Options options;
    if (const auto isolated = user_data::override_root()) options.directory = *isolated / L"logs";
    else options.directory = environment(L"RESKATE_LOG_DIRECTORY");
    if (options.directory.empty()) {
        options.truncate_file = true;
        std::array<wchar_t, 32768> module{};
        const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!count || count >= module.size()) throw std::runtime_error("Cannot resolve log directory");
        options.directory = log_directory(std::filesystem::path(module.data()).parent_path());
    }
    if (const auto level = parse_level(detail::utf8(environment(L"RESKATE_LOG_LEVEL")))) options.level = *level;
    options.external_console = environment(L"RESKATE_LOG_CONSOLE") == L"1";
    return options;
}
namespace detail {
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string clean(std::string_view text) {
    constexpr std::size_t limit = 8192;
    std::string result; result.reserve(std::min(text.size(), limit));
    for (std::size_t i = 0; i < text.size() && result.size() < limit; ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch == 0x1b) {
            if (i + 1 < text.size() && text[i + 1] == '[') {
                i += 2;
                while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
            }
            continue;
        }
        if (ch == '\r') continue;
        result += ch >= 0x20 || ch == '\n' || ch == '\t' ? static_cast<char>(ch) : '?';
    }
    if (text.size() > limit && result.size() == limit) {
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) result.pop_back();
        if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xc0) result.pop_back();
        result += "... [truncated]";
    }
    return result;
}
}
}
