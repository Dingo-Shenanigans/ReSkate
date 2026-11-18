#include "user_data_redirect.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/user_data_paths.h"

#include <Windows.h>
#include <ShlObj.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

#ifndef CSIDL_FOLDER_MASK
#define CSIDL_FOLDER_MASK 0x00ff
#endif

namespace dingosdk {
namespace {

using FolderPath = HRESULT (WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
using KnownFolderPath = HRESULT (WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

std::atomic<FolderPath> original_folder_path{};
std::atomic<KnownFolderPath> original_known_folder_path{};
std::uintptr_t game_begin{}, game_end{};

bool from_game(const void* return_address) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(return_address);
    return address >= game_begin && address < game_end;
}

// How logs name the redirect target: without the Windows user name (logs travel with crash
// reports), and without path::string(), which throws on characters outside the ANSI code page.
const char* shown_target() noexcept {
    return user_data::development_requested() ? R"(%RESKATE_DATA_ROOT%\Game)" : R"(%LOCALAPPDATA%\ReSkate\Game)";
}

// Logs each distinct folder the game asks for once, so any other live-game
// location that still leaks through is visible in ReSkate.log.
void note(const char* api, unsigned long folder, bool redirected) {
    static std::mutex mutex;
    static std::set<std::pair<const char*, unsigned long>> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.emplace(api, folder).second) return;
    }
    logging::log(logging::Level::info, logging::Channel::runtime, "Game user-data lookup: {} 0x{:x}{}",
        api, folder, redirected ? std::string(" -> ") + shown_target() : "");
}

HRESULT WINAPI folder_path(HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path) {
    const auto result = original_folder_path.load()(owner, folder, token, flags, path);
    if (!from_game(_ReturnAddress())) return result;
    const bool local = (folder & CSIDL_FOLDER_MASK) == CSIDL_LOCAL_APPDATA;
    // A launch with a separate user-data root fails the lookup rather than hand the game the
    // player's own LocalAppData.
    const bool isolated = local && user_data::development_requested();
    try {
        note("SHGetFolderPathW", static_cast<unsigned long>(folder & CSIDL_FOLDER_MASK), local && SUCCEEDED(result));
        if (!local || FAILED(result) || !path) return result;
        const auto target = redirected_local_app_data().wstring();
        if (target.size() >= MAX_PATH) {
            if (isolated) { path[0] = L'\0'; return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); }
            return result;
        }
        wcscpy_s(path, MAX_PATH, target.c_str());
    } catch (...) {
        if (isolated) { if (path) path[0] = L'\0'; return E_FAIL; }
    }
    return result;
}

HRESULT WINAPI known_folder_path(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* path) {
    const auto result = original_known_folder_path.load()(id, flags, token, path);
    if (!from_game(_ReturnAddress())) return result;
    const bool local = IsEqualGUID(id, FOLDERID_LocalAppData) != FALSE;
    const bool isolated = local && user_data::development_requested();
    try {
        note("SHGetKnownFolderPath", id.Data1, local && SUCCEEDED(result));
        if (!local || FAILED(result) || !path) return result;
        const auto target = redirected_local_app_data().wstring();
        const auto bytes = (target.size() + 1) * sizeof(wchar_t);
        const auto copy = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (!copy) {
            if (isolated) { CoTaskMemFree(*path); *path = nullptr; return E_OUTOFMEMORY; }
            return result;
        }
        std::memcpy(copy, target.c_str(), bytes);
        CoTaskMemFree(*path);
        *path = copy;
    } catch (...) {
        if (isolated) { if (path) { CoTaskMemFree(*path); *path = nullptr; } return E_FAIL; }
    }
    return result;
}

template<class Fn>
void hook(HMODULE module, const char* name, void* replacement, std::atomic<Fn>& original) {
    const auto target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target) throw std::runtime_error(std::string("shell32 is missing ") + name);
    void* relay{};
    const auto create = hook_prepare(target, replacement, &relay);
    if (create != HookOk) throw std::runtime_error(std::string("Cannot hook ") + name + ": " + hook_status_string(create));
    original.store(reinterpret_cast<Fn>(relay));
    const auto enable = hook_enable(target);
    if (enable != HookOk) throw std::runtime_error(std::string("Cannot enable ") + name + ": " + hook_status_string(enable));
}

} // namespace

fs::path redirected_local_app_data() {
    static const auto path = [] {
        auto root = user_data::root() / L"Game";
        fs::create_directories(root);
        return root;
    }();
    return path;
}

bool start_user_data_redirect(std::string& error) noexcept {
    try {
        // Checked again in the game (the launcher did it first), before anything uses the folder.
        user_data::initialize_development_root();
        const auto game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game + dos->e_lfanew);
        game_begin = game;
        game_end = game + nt->OptionalHeader.SizeOfImage;
        redirected_local_app_data();
        const auto shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!shell) throw std::runtime_error("Cannot load shell32.dll");
#pragma warning(push)
#pragma warning(disable: 4191)
        hook(shell, "SHGetFolderPathW", reinterpret_cast<void*>(&folder_path), original_folder_path);
        hook(shell, "SHGetKnownFolderPath", reinterpret_cast<void*>(&known_folder_path), original_known_folder_path);
#pragma warning(pop)
        error = shown_target();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace dingosdk
