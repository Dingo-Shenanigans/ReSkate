#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Vfs/initfs.h"
#include "Engine/Vfs/content_cache_install.h"
#include "Engine/Vfs/world_layer_scan.h"
#include "launch.h"
#include "text_encoding.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/runtime.h"

#include <Windows.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <winternl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t app_id[] = L"3354750";
constexpr DWORD remote_wait_ms = 30000;
// ReSkate's initialization merges the Mods folder whenever a mod or the game
// changed, with a startup window showing its progress. How long that takes
// depends on the mods, so it is waited for to the end: a deadline only cut
// big merges off, and the next launch started them again from scratch. The
// wait still ends if the game process dies, since its threads end with it.
constexpr DWORD initialize_wait_ms = INFINITE;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{};
};

class ChildProcess {
public:
    explicit ChildProcess(PROCESS_INFORMATION process) noexcept : process_(process) {}
    ~ChildProcess() {
        if (terminate_ && process_.hProcess) {
            TerminateProcess(process_.hProcess, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process_.hProcess, 5000);
        }
        if (process_.hThread) CloseHandle(process_.hThread);
        if (process_.hProcess) CloseHandle(process_.hProcess);
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    HANDLE process() const noexcept { return process_.hProcess; }
    HANDLE primary_thread() const noexcept { return process_.hThread; }
    DWORD id() const noexcept { return process_.dwProcessId; }
    void release() noexcept { terminate_ = false; }
private:
    PROCESS_INFORMATION process_{};
    bool terminate_{true};
};

class RemoteAllocation {
public:
    RemoteAllocation(HANDLE process, std::size_t size) : process_(process) {
        value_ = VirtualAllocEx(process, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!value_) throw std::runtime_error("VirtualAllocEx failed");
    }
    ~RemoteAllocation() { if (value_) VirtualFreeEx(process_, value_, 0, MEM_RELEASE); }
    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;
    void* get() const noexcept { return value_; }
private:
    HANDLE process_{};
    void* value_{};
};

using dingosdk::launcher_text::utf8;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) return L"Unknown error";
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()),
        output.data(), length);
    return output;
}

std::wstring system_error(DWORD error) {
    wchar_t* allocated = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&allocated), 0, nullptr);
    std::wstring output = length && allocated ? std::wstring(allocated, length) : L"Unknown error";
    if (allocated) LocalFree(allocated);
    while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n' ||
                               output.back() == L' ' || output.back() == L'.')) output.pop_back();
    return output;
}

[[noreturn]] void win32_failure(std::wstring_view action, DWORD error = GetLastError()) {
    std::wostringstream message;
    message << action << L" failed (" << error << L"): " << system_error(error);
    throw std::runtime_error(utf8(message.str()));
}

fs::path module_path() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) win32_failure(L"GetModuleFileNameW");
    return fs::path(std::wstring(buffer.data(), length));
}

fs::path canonical_file(const fs::path& path, std::wstring_view label) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        throw std::runtime_error(utf8(std::wstring(label) + L" is missing: " + path.wstring()));
    }
    const auto canonical = fs::canonical(path, error);
    if (error) throw std::runtime_error(utf8(L"Cannot resolve " + std::wstring(label) + L": " + path.wstring()));
    return canonical;
}

bool same_file(const fs::path& first, const fs::path& second) {
    Handle first_file(CreateFileW(first.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle second_file(CreateFileW(second.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    BY_HANDLE_FILE_INFORMATION first_info{}, second_info{};
    return first_file && second_file &&
        GetFileInformationByHandle(first_file.get(), &first_info) &&
        GetFileInformationByHandle(second_file.get(), &second_info) &&
        first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
        first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
        first_info.nFileIndexLow == second_info.nFileIndexLow;
}

std::wstring quote_argument(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
    std::wstring output(1, L'\"');
    std::size_t slashes{};
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'\"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(character);
            slashes = 0;
        } else {
            output.append(slashes, L'\\');
            slashes = 0;
            output.push_back(character);
        }
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

void set_environment(const wchar_t* name, const wchar_t* value) {
    if (!SetEnvironmentVariableW(name, value))
        win32_failure(std::wstring(L"SetEnvironmentVariableW(") + name + L")");
}

std::optional<DWORD> registry_dword(const wchar_t* key, const wchar_t* name) {
    DWORD value{}, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::wstring registry_string(const wchar_t* key, const wchar_t* name) {
    std::wstring value(1024, L'\0');
    DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return {};
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

constexpr wchar_t steam_key[] = LR"(Software\Valve\Steam)";
constexpr wchar_t steam_process_key[] = LR"(Software\Valve\Steam\ActiveProcess)";
constexpr std::uint64_t steam_id_base = 76561197960265728ull;

// Under Proton the Steam client is the native Linux one. Proton's stand-in
// steam.exe sets ActiveProcess\pid but never ActiveUser, and the
// C:\Program Files (x86)\Steam it points SteamPath at holds no account files.
// Steam passes the real client's directory to compatibility tools as a Unix
// path in STEAM_COMPAT_CLIENT_INSTALL_PATH, which Wine's kernel32 maps to a
// DOS one. Empty on Windows, and when Proton was not started by Steam.
// Reported, with the cause and the fix, by BenjaminJAnderson in
// https://github.com/Dingo-Shenanigans/ReSkate/issues/5.
fs::path proton_steam_directory() {
    std::array<wchar_t, 32768> unix_path{};
    const auto length = GetEnvironmentVariableW(L"STEAM_COMPAT_CLIENT_INSTALL_PATH", unix_path.data(),
        static_cast<DWORD>(unix_path.size()));
    if (!length || length >= unix_path.size()) return {};
    using Convert = wchar_t*(__cdecl*)(const char*);
    const auto convert = reinterpret_cast<Convert>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
    if (!convert) return {};
    const auto dos_path = convert(utf8(std::wstring_view(unix_path.data(), length)).c_str());
    if (!dos_path) return {};
    fs::path directory(dos_path);
    HeapFree(GetProcessHeap(), 0, dos_path);
    return directory;
}

// The Steam client's directory, where config/loginusers.vdf lists its accounts.
fs::path steam_directory() {
    if (auto native = proton_steam_directory(); !native.empty()) return native;
    return registry_string(steam_key, L"SteamPath");
}

// The account config/loginusers.vdf marks as most recent, otherwise its first.
// Steam has written that key as both MostRecent and mostrecent.
std::optional<std::uint64_t> recent_steam_id(const fs::path& steam) {
    if (steam.empty()) return std::nullopt;
    std::ifstream file(steam / L"config" / L"loginusers.vdf");
    std::optional<std::uint64_t> first, recent, current;
    std::string line;
    while (std::getline(file, line)) {
        const auto open = line.find('"');
        const auto close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) continue;
        const auto token = line.substr(open + 1, close - open - 1);
        if (token.size() == 17 && token.starts_with("7656") &&
            token.find_first_not_of("0123456789") == std::string::npos) {
            current = std::stoull(token);
            if (!first) first = current;
        } else if (_stricmp(token.c_str(), "MostRecent") == 0 && current &&
                   line.find("\"1\"", close) != std::string::npos) {
            recent = current;
        }
    }
    return recent ? recent : first;
}

// The account the running Steam client is signed in to. Steam for Windows
// publishes it as ActiveUser. Proton does not, but there the launcher was
// started by the native client, which is signed in to its most recent account.
std::optional<std::uint64_t> active_steam_id() {
    if (const auto user = registry_dword(steam_process_key, L"ActiveUser"); user && *user)
        return steam_id_base + *user;
    return recent_steam_id(proton_steam_directory());
}

// The account Steam was last signed in to: the live session when Steam is
// running, otherwise the most recent entry in config/loginusers.vdf. Offline
// mode reports it so the game keeps using the same settings save.
std::optional<std::uint64_t> last_steam_id() {
    if (const auto active = active_steam_id()) return active;
    return recent_steam_id(steam_directory());
}

void configure_environment(const fs::path& logs) {
    set_environment(L"SteamAppId", app_id);
    set_environment(L"SteamGameId", app_id);
    set_environment(L"RESKATE_LOG_DIRECTORY", logs.c_str());
    set_environment(L"RESKATE_DEBUG_TEST_GLOBAL_OFFLINE", L"1");
}

template<class T>
T remote_read(HANDLE process, std::uintptr_t address, std::wstring_view label) {
    T output{};
    SIZE_T count{};
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), &output,
            sizeof(output), &count) || count != sizeof(output)) win32_failure(label);
    return output;
}

std::uintptr_t image_base(HANDLE process) {
    using Query = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) win32_failure(L"Resolve NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION information{};
    const auto status = query(process, ProcessBasicInformation, &information,
        sizeof(information), nullptr);
    if (status < 0 || !information.PebBaseAddress)
        throw std::runtime_error("NtQueryInformationProcess failed");
    const auto peb = reinterpret_cast<std::uintptr_t>(information.PebBaseAddress);
    return remote_read<std::uintptr_t>(process, peb + 0x10, L"Read child image base");
}

template<std::size_t N>
bool remote_matches(HANDLE process, std::uintptr_t address,
                    const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> actual{};
    SIZE_T count{};
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), actual.data(),
        actual.size(), &count) && count == actual.size() && actual == expected;
}

std::uintptr_t validate_child_image(HANDLE process, const fs::path& game) {
    std::vector<wchar_t> path(32768);
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
        win32_failure(L"QueryFullProcessImageNameW");
    if (!same_file(game, fs::path(std::wstring(path.data(), length))))
        throw std::runtime_error("Created process is not the validated Skate.exe");

    const auto base = image_base(process);
    const auto dos = remote_read<IMAGE_DOS_HEADER>(process, base, L"Read child DOS header");
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
        throw std::runtime_error("Created process has an invalid DOS header");
    const auto nt = remote_read<IMAGE_NT_HEADERS64>(process,
        base + static_cast<std::uintptr_t>(dos.e_lfanew), L"Read child PE header");
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != dingosdk::launcher::expected_game_image_size)
        throw std::runtime_error("Created process has the wrong loaded image identity");

    // The same code fingerprints ReSkate.dll checks before it initializes.
    namespace image = dingosdk::addr::runtime;
    if (!remote_matches(process, base + image::client_tick, image::client_tick_prefix))
        throw std::runtime_error("Created process failed the loaded-image fingerprint");
    for (const auto& check : image::image_checks)
        if (!remote_matches(process, base + check.rva, check.bytes))
            throw std::runtime_error("Created process failed the loaded-image fingerprint");
    return base;
}

struct RemoteModule {
    std::uintptr_t base{};
    DWORD size{};
    fs::path path;
};

std::vector<RemoteModule> remote_modules(HANDLE process) { // not Toolhelp: it turns non-ANSI path characters into '?'
    std::vector<HMODULE> handles(512);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto capacity = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
        DWORD needed{};
        if (!EnumProcessModulesEx(process, handles.data(), capacity, &needed, LIST_MODULES_DEFAULT)) {
            if (GetLastError() == ERROR_PARTIAL_COPY) continue; // the list changed while it was read
            win32_failure(L"EnumProcessModulesEx");
        }
        if (needed > capacity) { handles.resize(needed / sizeof(HMODULE) + 16); continue; }
        handles.resize(needed / sizeof(HMODULE));
        std::vector<RemoteModule> output;
        std::vector<wchar_t> path(32768);
        for (const auto handle : handles) {
            MODULEINFO info{};
            const auto length = GetModuleFileNameExW(process, handle, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size() || !GetModuleInformation(process, handle, &info, sizeof(info))) continue;
            output.push_back({reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage,
                              fs::path(std::wstring(path.data(), length))});
        }
        return output;
    }
    throw std::runtime_error("The child module list kept changing while it was read");
}

RemoteModule find_remote_module(HANDLE process, const fs::path& path) {
    for (const auto& module : remote_modules(process))
        if (same_file(path, module.path)) return module;
    throw std::runtime_error("Injected ReSkate.dll is not present in the child module list");
}

DWORD run_remote_thread(HANDLE process, std::uintptr_t start, void* parameter,
                        std::wstring_view label, DWORD timeout_ms = remote_wait_ms) {
    Handle thread(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(start), parameter, 0, nullptr));
    if (!thread) win32_failure(std::wstring(L"CreateRemoteThread(") + std::wstring(label) + L")");
    // Waited in slices, so a long wait leaves a trail in the log instead of
    // looking like a hang. The thread also ends if the child process dies.
    for (DWORD waited = 0;;) {
        const auto slice = timeout_ms == INFINITE ? remote_wait_ms : std::min(remote_wait_ms, timeout_ms - waited);
        const auto wait = WaitForSingleObject(thread.get(), slice);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) win32_failure(std::wstring(L"Wait for ") + std::wstring(label));
        waited += slice;
        if (timeout_ms != INFINITE && waited >= timeout_ms)
            throw std::runtime_error(utf8(std::wstring(label) + L" timed out after " +
                std::to_wstring(waited / 1000) + L" s"));
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::launcher, std::wstring(label) +
            L" still running after " + std::to_wstring(waited / 1000) + L" s (merging mods can take a few minutes)");
    }
    DWORD result{};
    if (!GetExitCodeThread(thread.get(), &result))
        win32_failure(std::wstring(L"GetExitCodeThread(") + std::wstring(label) + L")");
    return result;
}

RemoteModule inject_dll(HANDLE process, const fs::path& dll) {
    const auto path = dll.wstring();
    const auto byte_count = (path.size() + 1) * sizeof(wchar_t);
    RemoteAllocation remote_path(process, byte_count);
    SIZE_T written{};
    if (!WriteProcessMemory(process, remote_path.get(), path.c_str(), byte_count, &written) ||
        written != byte_count) win32_failure(L"WriteProcessMemory(ReSkate.dll path)");
    std::string hook_note;
    const auto loader = dingosdk::launcher::validated_remote_load_library(process, &hook_note);
    if (!hook_note.empty())
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::launcher, hook_note);
    std::wostringstream address;
    address << L"Starting ordinary LoadLibraryW injection at 0x" << std::hex << loader;
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::launcher, address.str());
    const auto loader_result = run_remote_thread(process, loader, remote_path.get(), L"LoadLibraryW");
    // A thread exit code is only 32 bits and cannot carry an x64 HMODULE. Confirm
    // success and obtain the full module base from the target's module list.
    const auto module = find_remote_module(process, dll);
    std::wostringstream loaded;
    loaded << L"ReSkate.dll loaded at 0x" << std::hex << module.base
           << L" (LoadLibraryW low result 0x" << loader_result << L')';
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::launcher, loaded.str());
    return module;
}

} // namespace

namespace dingosdk::launcher_app {

Session open_session(const std::string& log_level) {
    Session session;
    session.self = canonical_file(module_path(), L"ReSkateLauncher.exe");
    session.paths = launcher::sibling_paths(session.self);
    session.paths.logs = logging::log_directory(session.paths.directory);
    logging::Options logging_options;
    logging_options.directory = session.paths.logs;
    logging_options.truncate_file = true;
    logging_options.level = logging::parse_level(log_level).value_or(logging::Level::info);
    if (!logging::initialize(logging_options)) logging::write(logging::Level::warning,
        logging::Channel::launcher, "File logging is unavailable; debugger output remains active.");
    logging::write(logging::Level::info, logging::Channel::launcher, L"ReSkate launcher starting");
    logging::write(logging::Level::info, logging::Channel::launcher, L"Current launch log: " + (session.paths.logs / L"ReSkate.log").wstring());
    return session;
}

bool game_files_supported(const launcher::SiblingPaths& paths) {
    std::error_code error;
    if (!fs::is_regular_file(paths.game, error)) {
        logging::write(logging::Level::info, logging::Channel::launcher, L"Skate.exe is not installed in " + paths.directory.wstring());
        return false;
    }
    try {
        launcher::validate_game_file(paths.game);
        launcher::validate_steam_api_file(paths.steam_api);
        return true;
    } catch (const std::exception& exception) {
        logging::log(logging::Level::warning, logging::Channel::launcher,
            "Game files are not the supported build: {}", exception.what());
        return false;
    }
}

bool config_matches_build(const launcher_update::Config& config) {
    return config.game.skate_sha256 == supported_build::game_sha256 &&
           config.game.app_id == supported_build::steam_app_id &&
           config.game.depot_id == supported_build::steam_depot_id &&
           config.game.manifest_id == supported_build::steam_manifest_id;
}

void relaunch(const fs::path& self, const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(self.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
            self.parent_path().c_str(), &startup, &process)) win32_failure(L"Restart launcher");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

std::wstring steam_offline_reason() {
    const auto pid = registry_dword(steam_process_key, L"pid");
    if (!pid || !*pid) return L"no Steam client is registered as running (ActiveProcess pid is 0)";
    if (!active_steam_id()) return L"Steam reports no signed-in account (ActiveProcess ActiveUser is 0)";
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid));
    if (!process.get())
        return std::format(L"the registered Steam process {} cannot be opened (Windows error {})", *pid, GetLastError());
    DWORD code{};
    if (!GetExitCodeProcess(process.get(), &code) || code != STILL_ACTIVE)
        return std::format(L"the registered Steam process {} is no longer running", *pid);
    std::wstring image(MAX_PATH, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
        return std::format(L"the registered Steam process {} cannot be identified (Windows error {})", *pid,
                           GetLastError());
    image.resize(length);
    const auto name = fs::path(image).filename().wstring();
    if (_wcsicmp(name.c_str(), L"steam.exe") != 0)
        return std::format(L"the registered Steam process {} is {}, not steam.exe", *pid, name);
    return {};
}

bool steam_signed_in() {
    return steam_offline_reason().empty();
}

std::string steam_persona_name() {
    if (!steam_signed_in()) return {};
    const auto user = active_steam_id();
    if (!user) return {};
    const auto id = std::to_string(*user);
    // loginusers.vdf: "<SteamID64>" { ... "PersonaName" "<name>" ... }, UTF-8 with VDF escapes.
    const auto quoted = [](std::string_view line) {
        std::vector<std::string> tokens;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '"') continue;
            std::string token;
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                token.push_back(line[i]);
            }
            tokens.push_back(std::move(token));
        }
        return tokens;
    };
    std::string name;
    if (const auto steam = steam_directory(); !steam.empty()) {
        std::ifstream file(steam / L"config" / L"loginusers.vdf");
        bool account{};
        for (std::string line; std::getline(file, line);) {
            const auto tokens = quoted(line);
            if (tokens.size() == 1 && tokens[0].size() == 17 && tokens[0].find_first_not_of("0123456789") == std::string::npos)
                account = tokens[0] == id;
            else if (account && tokens.size() == 2 && tokens[0] == "PersonaName") {
                name = tokens[1];
                break;
            }
        }
    }
    if (name.empty()) name = utf8(registry_string(steam_key, L"LastGameNameUsed"));
    return name;
}

// Skate.exe hard-imports MiniDumpWriteDump from the Windows API set
// api-ms-win-core-debug-minidump-l1-1-0.dll. Windows resolves that set through
// its API set schema, so no such file exists on disk; Wine/Proton does not map
// it, leaving the import unresolved so the game aborts in the loader before
// injection (seen as "Cannot sample the executable primary thread", Windows
// error 5, once the suspended thread tears down). dbghelp.dll exports
// MiniDumpWriteDump under the same name, so a copy of it beside Skate.exe under
// the API set's file name satisfies the import. On Windows the schema handles
// this import without a shim, so do not create one there.
void ensure_minidump_apiset_shim(const fs::path& game_directory) noexcept {
    try {
        // Windows resolves this API set itself. Only Wine/Proton needs a file
        // beside Skate.exe; copying a system DLL under an API-set name on
        // Windows creates an unnecessary executable that looks suspicious.
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!(ntdll && GetProcAddress(ntdll, "wine_get_version")) &&
            !(kernel32 && GetProcAddress(kernel32, "wine_get_dos_file_name"))) return;
        const auto shim = game_directory / L"api-ms-win-core-debug-minidump-l1-1-0.dll";
        std::error_code ec;
        if (fs::exists(shim, ec)) return;
        std::array<wchar_t, MAX_PATH> system{};
        const auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!length || length >= system.size()) return;
        const auto source = fs::path(std::wstring(system.data(), length)) / L"dbghelp.dll";
        if (!fs::exists(source, ec)) return;
        if (fs::copy_file(source, shim, fs::copy_options::none, ec) && !ec)
            logging::write(logging::Level::info, logging::Channel::launcher,
                L"Created api-ms-win-core-debug-minidump-l1-1-0.dll from dbghelp.dll so Skate.exe's "
                L"MiniDumpWriteDump import resolves under Wine/Proton");
        else if (ec)
            logging::log(logging::Level::warning, logging::Channel::launcher,
                "Could not create the MiniDumpWriteDump API-set shim beside Skate.exe: {}", ec.message());
    } catch (...) {}
}

DWORD start_game(const Session& session, const launcher::LaunchOptions& options,
                 const std::function<void(DWORD)>& created) {
    const auto& self = session.self;
    auto paths = session.paths;
    paths.game = canonical_file(paths.game, L"Skate.exe");
    paths.dll = canonical_file(paths.dll, L"ReSkate.dll");
    paths.steam_api = canonical_file(paths.steam_api, L"steam_api64.dll");

    std::error_code path_error;
    if (!fs::equivalent(paths.game.parent_path(), paths.directory, path_error) || path_error ||
        !fs::equivalent(paths.dll.parent_path(), paths.directory, path_error) || path_error ||
        !fs::equivalent(paths.steam_api.parent_path(), paths.directory, path_error) || path_error)
        throw std::runtime_error(
            "Skate.exe, ReSkate.dll, and the original steam_api64.dll must be beside ReSkateLauncher.exe");

    // Satisfy Skate.exe's MiniDumpWriteDump API-set import under Wine/Proton.
    ensure_minidump_apiset_shim(paths.directory);

    logging::write(logging::Level::info, logging::Channel::launcher, L"Validating exact Skate.exe SHA-256 and PE identity");
    launcher::validate_game_file(paths.game);
    logging::write(logging::Level::info, logging::Channel::launcher, L"Validating the original sibling steam_api64.dll");
    launcher::validate_steam_api_file(paths.steam_api);
    logging::write(logging::Level::info, logging::Channel::launcher, L"Launcher SHA-256: " + widen(launcher::sha256_file(self)));
    logging::write(logging::Level::info, logging::Channel::launcher, L"Runtime SHA-256: " + widen(launcher::sha256_file(paths.dll)));
    const auto dll_info = launcher::inspect_pe_file(paths.dll);
    if (!dll_info.pe64 || dll_info.machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(dll_info.characteristics & IMAGE_FILE_DLL) ||
        !(dll_info.characteristics & IMAGE_FILE_EXECUTABLE_IMAGE))
        throw std::runtime_error("ReSkate.dll is not an x64 Windows DLL");
    const auto initialize_rva = launcher::exported_function_rva(
        paths.dll, "DingoSDKDebugInitialize");
    if (initialize_rva >= dll_info.image_size)
        throw std::runtime_error("DingoSDKDebugInitialize is outside the DLL image");

    // Park thumbnails are read from the game's own data now; the pack older
    // launchers downloaded is no longer used.
    {
        std::array<wchar_t, 32768> local{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
        std::error_code ignored;
        if (length && length < local.size())
            fs::remove(fs::path(local.data()) / L"ReSkate" / L"profiles" / L"offline" / L"ReSkate-Object-Previews.bin", ignored);
    }
    // Catalogues (item names, challenges, entitlements) are read from the live
    // game's content cache for this build, installed once into Local AppData.
    const auto cache = content_cache::ensure_installed();
    using CacheStatus = content_cache::InstallStatus;
    if (cache.status == CacheStatus::installed) {
        logging::write(logging::Level::info, logging::Channel::assets, "Game content cache ready.");
    } else if (cache.status == CacheStatus::downloaded) {
        logging::write(logging::Level::info, logging::Channel::assets, "Game content cache downloaded and installed.");
    } else {
        logging::log(logging::Level::error, logging::Channel::assets,
            "Game content cache {} (HTTP {}; Windows error {}).",
            cache.status == CacheStatus::busy ? "is being installed by another launcher" : "could not be installed",
            cache.http_status, cache.error);
        throw std::runtime_error(cache.status == CacheStatus::busy ?
            "Another ReSkate launcher is installing the game content cache. Try again when it finishes." :
            "ReSkate needs to download the game content cache once. Check your internet connection and try again.");
    }
    // World layers are read from the level data; the first launch of a build
    // scans it here so the game does not wait on it. The runtime rescans if
    // this cache is missing, so a failure is not fatal.
    try {
        const auto layers = world_layer_scan::load_or_scan(paths.directory, world_layer_scan::cache_file());
        logging::log(logging::Level::info, logging::Channel::world, "World layers ready: {} layers.", layers.layers.size());
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::world, "World layers could not be read: {}", error.what());
    }
    configure_environment(paths.logs);
    bool loose_files = options.loose_files && initfs::loose_files_preference(paths.directory);
    if (loose_files) {
        try {
            fs::create_directories(paths.directory / L"scripts" / L"Custom");
            const auto exported = initfs::export_files(paths.game,
                initfs::data_directory(paths.directory, options.game_arguments), paths.directory, true);
            if (exported.already_exported) {
                logging::write(logging::Level::info, logging::Channel::assets,
                    "Using existing scripts/ and config/ export; edits and removed files are preserved.");
            } else {
                logging::log(logging::Level::info, logging::Channel::assets,
                    "InitFS exported: {} Lua, {} configs; {} created, {} existing files preserved, {} skipped.",
                    exported.scripts, exported.configs, exported.created, exported.preserved, exported.skipped);
            }
        } catch (const std::exception& failure) {
            // An install under Program Files is the usual reason. Skate plays
            // without loose scripts; only editing them is lost.
            loose_files = false;
            logging::log(logging::Level::warning, logging::Channel::assets,
                "Loose files are off for this launch: scripts/ and config/ could not be exported ({}). Skate still "
                "starts; editing them needs a folder ReSkate is allowed to write to.", failure.what());
        }
    }
    set_environment(L"RESKATE_LOOSE_FILES", loose_files ? L"1" : L"0");
    set_environment(L"RESKATE_LOG_CONSOLE", options.window_console ? L"1" : L"0");
    set_environment(L"RESKATE_LOG_LEVEL", widen(options.log_level).c_str());
    set_environment(L"RESKATE_FORCE_WINDOWED", options.force_windowed ? L"1" : L"0");
    set_environment(L"RESKATE_WINDOW_WIDTH", options.force_windowed ? std::to_wstring(options.width).c_str() : nullptr);
    set_environment(L"RESKATE_WINDOW_HEIGHT", options.force_windowed ? std::to_wstring(options.height).c_str() : nullptr);
    std::wstring command = quote_argument(paths.game.wstring());
    for (const auto& argument : options.game_arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    set_environment(L"RESKATE_GPU_DIAGNOSTICS", options.gpu_diagnostics ? L"1" : L"0");
    // -map: the game loads it once it is up, as the console's "load" would (client_tick.cpp),
    // ahead of any startup commands already asked for.
    if (!options.map.empty()) {
        std::wstring commands = L"load " + options.map;
        std::wstring already(4096, L'\0');
        const auto length = GetEnvironmentVariableW(L"RESKATE_STARTUP_COMMANDS", already.data(), 4096);
        if (length && length < 4096) commands += L";" + already.substr(0, length);
        set_environment(L"RESKATE_STARTUP_COMMANDS", commands.c_str());
    }
    set_environment(L"RESKATE_DISCORD", options.discord ? L"1" : L"0");
    if (options.menu_key == options.console_key)
        throw std::runtime_error("The menu and console need different keys. Change one in Settings.");
    set_environment(L"RESKATE_MENU_KEY", std::to_wstring(options.menu_key).c_str());
    set_environment(L"RESKATE_CONSOLE_KEY", std::to_wstring(options.console_key).c_str());
    const auto steam_reason = options.offline ? std::wstring{} : launcher_app::steam_offline_reason();
    const bool offline = options.offline || !steam_reason.empty();
    set_environment(L"RESKATE_OFFLINE", offline ? L"1" : L"0");
    const auto steam_id = offline ? last_steam_id() : std::nullopt;
    set_environment(L"RESKATE_OFFLINE_STEAM_ID", steam_id ? std::to_wstring(*steam_id).c_str() : nullptr);
    if (offline)
        logging::write(logging::Level::info, logging::Channel::launcher, options.offline ?
            std::wstring(L"Offline mode: requested; Steam is not used") :
            L"Offline mode: Steam is not running or not signed in: " + steam_reason);
    logging::write(logging::Level::info, logging::Channel::launcher, std::wstring(L"Graphics options: DRED=") +
        (options.gpu_diagnostics ? L"enabled" : L"default"));
    if (options.force_windowed)
        logging::write(logging::Level::info, logging::Channel::launcher, L"Requested windowed resolution: " + std::to_wstring(options.width) +
            L"x" + std::to_wstring(options.height));
    else logging::write(logging::Level::info, logging::Channel::launcher, L"Display settings: using the game's saved preferences");
    command += launcher::windowed_arguments(options);
    const auto mod_arguments = launcher::mod_data_arguments(paths.directory, options.game_arguments);
    command += mod_arguments;
    if (!mod_arguments.empty()) logging::write(logging::Level::info, logging::Channel::launcher, L"ModData folder detected; loading ModData/Default");
    logging::write(logging::Level::info, logging::Channel::launcher, L"Child command: " + command);
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(paths.game.c_str(), command_buffer.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, paths.directory.c_str(),
            &startup, &process)) win32_failure(L"CreateProcessW(Skate.exe)");
    ChildProcess child(process);
    if (created) created(child.id());
    logging::write(logging::Level::info, logging::Channel::launcher, L"Created suspended Skate.exe process " + std::to_wstring(child.id()));

    const auto game_base = validate_child_image(child.process(), paths.game);
    std::wostringstream validated;
    validated << L"Loaded Skate.exe image validated at 0x" << std::hex << game_base;
    logging::write(logging::Level::info, logging::Channel::launcher, validated.str());

    auto loader_gate = launcher::prepare_loader_for_injection(
        child.process(), child.primary_thread(), child.id(), game_base);
    std::wostringstream loader_ready;
    loader_ready << L"Windows loader ready at executable entrypoint 0x" << std::hex
                 << loader_gate.entrypoint() << std::dec << L"; gated "
                 << loader_gate.thread_count() << L" existing thread(s)";
    if (const auto exiting = loader_gate.exiting_thread_count())
        loader_ready << L", skipped " << exiting << L" already exiting";
    logging::write(logging::Level::info, logging::Channel::launcher, loader_ready.str());

    const auto remote_dll = inject_dll(child.process(), paths.dll);
    const auto initialize_address = remote_dll.base + initialize_rva;
    if (initialize_address < remote_dll.base || initialize_rva >= remote_dll.size)
        throw std::runtime_error("Remote DingoSDKDebugInitialize address is invalid");
    std::wostringstream initializing;
    initializing << L"Calling DingoSDKDebugInitialize at 0x" << std::hex << initialize_address;
    logging::write(logging::Level::info, logging::Channel::launcher, initializing.str());
    const auto initialized = run_remote_thread(child.process(), initialize_address, nullptr,
        L"DingoSDKDebugInitialize", initialize_wait_ms);
    if (!initialized) throw std::runtime_error("DingoSDKDebugInitialize returned FALSE");
    logging::write(logging::Level::info, logging::Channel::launcher, L"ReSkate.dll initialization confirmed");

    loader_gate.resume();
    child.release();
    logging::write(logging::Level::info, logging::Channel::launcher, L"Skate.exe resumed; launcher complete");
    return child.id();
}

} // namespace dingosdk::launcher_app
