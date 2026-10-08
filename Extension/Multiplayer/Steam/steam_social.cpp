#include "steam_social.h"
#include "steam_friend_join.h"
#include "Engine/Core/Platform/launcher_support.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
template<class T> T symbol(HMODULE module, const char *name) {
    const auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error("Steam social export unavailable");
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(address);
#pragma warning(pop)
}
std::string name(const char *value) {
    if (!value) return {};
    std::string result;
    for (unsigned i = 0; i < 128 && value[i]; ++i)
        if (static_cast<unsigned char>(value[i]) >= 32 && value[i] != 127) result += value[i];
    return result;
}
struct Api {
    HMODULE module{};
    int (*user_handle)(){};
    void *(*friends)(){};
    void *(*user)(){};
    std::uint64_t (*id)(void *){};
    bool (*logged_on)(void *){};
    const char *(*self_name)(void *){};
    int (*count)(void *, int){};
    std::uint64_t (*at)(void *, int, int){};
    const char *(*friend_name)(void *, std::uint64_t){};
    int (*presence)(void *, std::uint64_t){};
    struct Game { std::uint64_t id{}; std::uint32_t ip{}; std::uint16_t port{}, query{}; std::uint64_t lobby{}; };
    bool (*game)(void *, std::uint64_t, Game *){};
    const char *(*rich)(void *, std::uint64_t, const char *){};
    void load() {
        const auto loaded = GetModuleHandleW(L"steam_api64.dll");
        if (!loaded || loaded == module) return;
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return;
        path.resize(length); launcher::validate_steam_api_file(std::filesystem::path(path));
        user_handle = symbol<decltype(user_handle)>(loaded, "SteamAPI_GetHSteamUser");
        friends = symbol<decltype(friends)>(loaded, "SteamAPI_SteamFriends_v017");
        user = symbol<decltype(user)>(loaded, "SteamAPI_SteamUser_v023");
        id = symbol<decltype(id)>(loaded, "SteamAPI_ISteamUser_GetSteamID");
        logged_on = symbol<decltype(logged_on)>(loaded, "SteamAPI_ISteamUser_BLoggedOn");
#define BIND(member, suffix) member = symbol<decltype(member)>(loaded, "SteamAPI_ISteamFriends_" suffix)
        BIND(self_name, "GetPersonaName"); BIND(count, "GetFriendCount"); BIND(at, "GetFriendByIndex");
        BIND(friend_name, "GetFriendPersonaName"); BIND(presence, "GetFriendPersonaState");
        BIND(game, "GetFriendGamePlayed"); BIND(rich, "GetFriendRichPresence");
#undef BIND
        module = loaded;
    }
};
}
std::shared_ptr<const SteamSocialSnapshot> steam_social_snapshot() {
    static Api api;
    static std::mutex mutex;
    static auto current = std::make_shared<const SteamSocialSnapshot>();
    static ULONGLONG next{};
    // Each friend costs four calls into the Steam client, and the game thread asks for
    // this snapshot every frame. Read the list a few friends per clock tick (~16 ms)
    // and publish it once complete, so no caller pays for the whole list at once.
    static struct Refresh {
        bool active{};
        int index{}, count{};
        ULONGLONG tick{};
        void *friends{};
        SteamSocialSnapshot result;
    } refresh;
    constexpr int friends_per_tick = 8;
    std::lock_guard lock(mutex);
    const auto now = GetTickCount64();
    const auto publish = [&](SteamSocialSnapshot &result) {
        if (result.local != current->local || result.friends != current->friends) {
            result.revision = current->revision + 1;
            current = std::make_shared<const SteamSocialSnapshot>(std::move(result));
        }
    };
    if (!refresh.active) {
        if (now < next) return current;
        next = now + 10000;
        SteamSocialSnapshot result;
        if (launcher::offline_mode()) {
            result.local = {launcher::offline_steam_id(), launcher::offline_player_name, false, true};
            if (result.local != current->local) {
                result.revision = current->revision + 1;
                current = std::make_shared<const SteamSocialSnapshot>(std::move(result));
            }
            return current;
        }
        try {
            api.load();
            if (api.module && api.user_handle()) {
                const auto user = api.user(), friends = api.friends();
                if (user && friends) {
                    result.local = {api.id(user), name(api.self_name(friends)), api.logged_on(user), true};
                    // The local player is known now: publish it at once (with the friends
                    // already known) rather than after the whole list has been read.
                    if (result.local != current->local) {
                        SteamSocialSnapshot local = *current;
                        local.local = result.local;
                        local.revision = current->revision + 1;
                        current = std::make_shared<const SteamSocialSnapshot>(std::move(local));
                    }
                    // Immediate friends only; exclude blocked/requested/ignored accounts.
                    refresh.count = std::clamp(api.count(friends, 4), 0, 2048);
                    refresh.index = 0;
                    refresh.tick = 0;
                    refresh.friends = friends;
                    refresh.result = std::move(result);
                    refresh.result.friends.reserve(static_cast<std::size_t>(refresh.count));
                    refresh.active = true;
                }
            }
        } catch (...) { return current; }
        if (!refresh.active) {
            publish(result);
            return current;
        }
    }
    if (now == refresh.tick) return current;
    refresh.tick = now;
    try {
        if (!api.user_handle()) {
            refresh = {};
            return current;
        }
        auto &result = refresh.result;
        for (const auto end = std::min(refresh.count, refresh.index + friends_per_tick); refresh.index < end; ++refresh.index) {
            const auto id = api.at(refresh.friends, refresh.index, 4);
            if (!id || id == result.local.id) continue;
            const auto presence = api.presence(refresh.friends, id);
            Api::Game game;
            const bool playing = api.game(refresh.friends, id, &game) && (game.id & 0xffffff) == 3354750;
            // Where they skate: what their game says for this, or (a build before it did) the
            // joinable session it offers friends.
            std::uint64_t session{};
            if (playing) {
                session = steam_session_target(name(api.rich(refresh.friends, id, steam_session_key.data()))).value_or(0);
                if (!session) session = steam_join_target(name(api.rich(refresh.friends, id, "connect"))).value_or(0);
            }
            result.friends.push_back({id, name(api.friend_name(refresh.friends, id)), presence > 0 && presence < 7, playing, session});
        }
        if (refresh.index < refresh.count) return current;
        // The list changed during the pass, so an index may have shifted past a friend:
        // keep the ones read last time that this pass missed (a friend really removed
        // goes at the next pass).
        if (std::clamp(api.count(refresh.friends, 4), 0, 2048) != refresh.count)
            for (const auto &known : current->friends)
                if (std::none_of(result.friends.begin(), result.friends.end(),
                                 [&](const auto &read) { return read.id == known.id; }))
                    result.friends.push_back(known);
        // The list can change between ticks, shifting an index: keep each friend once.
        std::sort(result.friends.begin(), result.friends.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        result.friends.erase(std::unique(result.friends.begin(), result.friends.end(),
                                         [](const auto &a, const auto &b) { return a.id == b.id; }),
                             result.friends.end());
        std::sort(result.friends.begin(), result.friends.end(), [](const auto &a, const auto &b) {
            if (a.playing != b.playing) return a.playing;
            if (a.online != b.online) return a.online;
            return a.id < b.id;
        });
        publish(result);
    } catch (...) {}
    refresh = {};
    // A pass reads every friend (four Steam calls each) on the game thread: profiled at 0.4% of
    // the client update with a full list read every 2 s (2026-10-01). Every 10 s is plenty for
    // the friends list; the local player's own entry still refreshes at the start of each pass.
    next = now + 10000;
    return current;
}
}
