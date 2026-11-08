#include "steam_avatars.h"
#include "Engine/Core/Platform/launcher_support.h"
#include <Windows.h>
#include <filesystem>
#include <mutex>
#include <string>

namespace dingosdk::multiplayer {
namespace {
template<class T> T symbol(HMODULE module, const char *name) {
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(GetProcAddress(module, name));
#pragma warning(pop)
}
// The flat exports of the steam_api64.dll the game ships (build 20260929): the same interface
// versions steam_social.cpp and steam_lobby_api.cpp use.
struct Api {
    HMODULE module{};
    ULONGLONG next_try{};
    int (*user_handle)(){};
    void *(*friends)(){};
    void *(*utils)(){};
    int (*medium_avatar)(void *, std::uint64_t){};
    int (*large_avatar)(void *, std::uint64_t){};
    void *(*user)(){};
    std::uint64_t (*own_id)(void *){};
    bool (*image_size)(void *, int, std::uint32_t *, std::uint32_t *){};
    bool (*image_rgba)(void *, int, std::uint8_t *, int){};
    bool (*overlay_enabled)(void *){};
    void (*overlay_to_user)(void *, const char *, std::uint64_t){};
};
std::mutex api_mutex;
Api api; // under the mutex

// Under the mutex: whether there is a Steam session to ask. The game's steam_api64.dll is checked
// as steam_social.cpp checks it, and looked for again every few seconds until the game has loaded it.
bool steam_up() {
    if (launcher::offline_mode()) return false;
    if (!api.module) {
        const auto now = GetTickCount64();
        if (now < api.next_try) return false;
        api.next_try = now + 5000;
        const auto loaded = GetModuleHandleW(L"steam_api64.dll");
        if (!loaded) return false;
        try {
            std::wstring path(32768, L'\0');
            const auto length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) return false;
            path.resize(length);
            launcher::validate_steam_api_file(std::filesystem::path(path));
        } catch (...) {
            return false;
        }
        Api found;
        found.user_handle = symbol<decltype(found.user_handle)>(loaded, "SteamAPI_GetHSteamUser");
        found.friends = symbol<decltype(found.friends)>(loaded, "SteamAPI_SteamFriends_v017");
        found.utils = symbol<decltype(found.utils)>(loaded, "SteamAPI_SteamUtils_v010");
        found.medium_avatar = symbol<decltype(found.medium_avatar)>(loaded, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
        found.large_avatar = symbol<decltype(found.large_avatar)>(loaded, "SteamAPI_ISteamFriends_GetLargeFriendAvatar");
        found.user = symbol<decltype(found.user)>(loaded, "SteamAPI_SteamUser_v023");
        found.own_id = symbol<decltype(found.own_id)>(loaded, "SteamAPI_ISteamUser_GetSteamID");
        found.image_size = symbol<decltype(found.image_size)>(loaded, "SteamAPI_ISteamUtils_GetImageSize");
        found.image_rgba = symbol<decltype(found.image_rgba)>(loaded, "SteamAPI_ISteamUtils_GetImageRGBA");
        found.overlay_enabled = symbol<decltype(found.overlay_enabled)>(loaded, "SteamAPI_ISteamUtils_IsOverlayEnabled");
        found.overlay_to_user = symbol<decltype(found.overlay_to_user)>(loaded, "SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
        if (!found.user_handle || !found.friends || !found.utils || !found.medium_avatar || !found.image_size || !found.image_rgba ||
            !found.overlay_enabled || !found.overlay_to_user || !found.large_avatar || !found.user || !found.own_id)
            return false;
        found.module = loaded;
        api = found;
    }
    return api.user_handle() != 0;
}
}

namespace {
// Under the mutex, with Steam up: the avatar's pixels from its image handle.
// 0: they have no avatar; -1: Steam has not downloaded it yet, and now starts to.
bool avatar_pixels(void *utils, int image, SteamAvatar &out) {
    if (image <= 0) return false;
    std::uint32_t width{}, height{};
    if (!api.image_size(utils, image, &width, &height) || !width || !height || width > 256 || height > 256) return false;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
    if (!api.image_rgba(utils, image, rgba.data(), static_cast<int>(rgba.size()))) return false;
    out.rgba = std::move(rgba);
    out.width = width, out.height = height;
    return true;
}
}

bool steam_own_avatar(SteamAvatar &out) {
    std::lock_guard lock(api_mutex);
    try {
        if (!steam_up()) return false;
        void *friends = api.friends(), *utils = api.utils(), *user = api.user();
        if (!friends || !utils || !user) return false;
        const auto id = api.own_id(user);
        return id && avatar_pixels(utils, api.large_avatar(friends, id), out);
    } catch (...) {
        return false;
    }
}

bool steam_friend_avatar(std::uint64_t id, SteamAvatar &out) {
    if (!id) return false;
    std::lock_guard lock(api_mutex);
    try {
        if (!steam_up()) return false;
        void *friends = api.friends(), *utils = api.utils();
        if (!friends || !utils) return false;
        return avatar_pixels(utils, api.medium_avatar(friends, id), out);
    } catch (...) {
        return false;
    }
}
bool steam_open_profile(std::uint64_t id) {
    if (!id) return false;
    std::lock_guard lock(api_mutex);
    try {
        if (!steam_up()) return false;
        void *friends = api.friends(), *utils = api.utils();
        if (!friends || !utils || !api.overlay_enabled(utils)) return false;
        api.overlay_to_user(friends, "steamid", id);
        return true;
    } catch (...) {
        return false;
    }
}
}
