#include "steam_restart_guard.h"
#include "offline_steam.h"

#include "Engine/Core/Platform/launcher_support.h"

#include "Engine/Core/Hooks/hooks.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace dingosdk {
namespace {

void module_anchor() {}

bool __cdecl restart_not_needed(std::uint32_t) noexcept { return false; }

std::atomic<void*> guarded_target{};

fs::path module_path(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("Module path is unavailable");
    buffer.resize(length);
    return fs::path(buffer);
}

} // namespace

bool install_steam_restart_guard(HMODULE steam_module, std::string& error) noexcept {
    try {
        if (!steam_module) throw std::runtime_error("Steam module is null");
        const auto target = GetProcAddress(steam_module, "SteamAPI_RestartAppIfNecessary");
        if (!target) throw std::runtime_error("Original Steam API is missing SteamAPI_RestartAppIfNecessary");
        const auto current = guarded_target.load();
        if (current == reinterpret_cast<void*>(target)) {
            error.clear();
            return true;
        }
        if (current) {
            error = "The Steam restart guard is already installed on a different module";
            return false;
        }
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto create = hook_prepare(reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(&restart_not_needed), nullptr);
#pragma warning(pop)
        if (create != HookOk) {
            error = "Cannot create the Steam restart guard (Detours hook service status " +
                std::to_string(create) + ")";
            return false;
        }
        const auto enable = hook_enable(reinterpret_cast<void*>(target));
        if (enable != HookOk) {
            hook_remove(reinterpret_cast<void*>(target));
            error = "Cannot enable the Steam restart guard (Detours hook service status " +
                std::to_string(enable) + ")";
            return false;
        }
        guarded_target.store(reinterpret_cast<void*>(target));
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool start_steam_restart_guard(std::string& error) noexcept {
    try {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&module_anchor), &self))
            throw std::runtime_error("Cannot locate ReSkate.dll");
        // Development only (RESKATE_DEV_GAME_DIR): this DLL was built outside the game folder, so its
        // sibling files are the game's own.
        const auto expected = (GetEnvironmentVariableW(L"RESKATE_DEV_GAME_DIR", nullptr, 0)
            ? module_path(nullptr) : module_path(self)).parent_path() / L"steam_api64.dll";
        launcher::validate_steam_api_file(expected);

        HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
        if (steam) {
            std::error_code equivalent_error;
            if (!fs::equivalent(expected, module_path(steam), equivalent_error) || equivalent_error)
                throw std::runtime_error("Loaded steam_api64.dll is not ReSkate.dll's verified sibling");
        } else {
            steam = LoadLibraryExW(expected.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!steam) throw std::runtime_error("Cannot load verified sibling steam_api64.dll");
        }
        if (!install_steam_restart_guard(steam, error)) return false;
        return !launcher::offline_mode() || offline_steam::install(steam, error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace dingosdk
