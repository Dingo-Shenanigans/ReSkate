#include "steam_friend_join.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <array>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
template<class T> T symbol(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error("Steam friend joining is unavailable.");
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(address);
#pragma warning(pop)
}
std::string bounded(const char* text) {
    if (!text) return {};
    std::size_t length{};
    while (length < 256 && text[length]) ++length;
    return length < 256 ? std::string(text, length) : std::string{};
}
struct JoinEvent { std::uint64_t lobby{}, friend_id{}, received{}; bool rich{}; };
struct Inbox { std::mutex mutex; std::deque<JoinEvent> events; };
Inbox& inbox() { static auto* value = new Inbox; return *value; }
void enqueue(std::uint64_t lobby, std::uint64_t friend_id, bool rich) noexcept {
    if (!lobby) return;
    try {
        auto& queue = inbox();
        std::lock_guard lock(queue.mutex);
        if (queue.events.size() < 8) queue.events.push_back({lobby, friend_id, GetTickCount64(), rich});
    } catch (...) { }
}
// Steam's public CCallbackBase ABI: three virtual methods, byte flags, then
// callback ID. No virtual destructor. See Valve's steam_api_common.h.
class CallbackBase {
public:
    virtual void Run(void*) = 0;
    virtual void Run(void*, bool, std::uint64_t) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    std::uint8_t flags{};
    int callback{};
protected:
    ~CallbackBase() = default;
};
static_assert(sizeof(CallbackBase) == 16);
struct LobbyJoin { std::uint64_t lobby, friend_id; };
struct PresenceJoin { std::uint64_t friend_id; char connect[256]; };
static_assert(sizeof(LobbyJoin) == 16 && sizeof(PresenceJoin) == 264);
class LobbyCallback final : public CallbackBase {
    void Run(void* payload) override {
        if (payload) { const auto& event = *static_cast<LobbyJoin*>(payload); enqueue(event.lobby, event.friend_id, false); }
    }
    void Run(void* payload, bool failed, std::uint64_t) override { if (!failed) Run(payload); }
    int GetCallbackSizeBytes() override { return sizeof(LobbyJoin); }
};
class PresenceCallback final : public CallbackBase {
    void Run(void* payload) override {
        if (!payload) return;
        const auto& event = *static_cast<PresenceJoin*>(payload);
        std::size_t length{};
        while (length < sizeof(event.connect) && event.connect[length]) ++length;
        if (length == sizeof(event.connect)) return;
        if (const auto lobby = steam_join_target(std::string_view(event.connect, length))) enqueue(*lobby, event.friend_id, true);
    }
    void Run(void* payload, bool failed, std::uint64_t) override { if (!failed) Run(payload); }
    int GetCallbackSizeBytes() override { return sizeof(PresenceJoin); }
};
struct Api {
    HMODULE module{};
    void* friends{};
    std::uint64_t local_id{};
    int (*user_handle)(){};
    bool (*set)(void*, const char*, const char*){};
    const char* (*get)(void*, std::uint64_t, const char*){};
    LobbyCallback lobby_callback;
    PresenceCallback presence_callback;
    std::uint64_t next_open{}, next_presence{};
    std::string owned, previous, session;
    bool reported_error{};
    void open() {
        const auto loaded = GetModuleHandleW(L"steam_api64.dll");
        if (!loaded || module) return;
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return;
        path.resize(length); launcher::validate_steam_api_file(std::filesystem::path(path));
        user_handle = symbol<decltype(user_handle)>(loaded, "SteamAPI_GetHSteamUser");
        if (!user_handle()) return;
        auto* utils = symbol<void* (*)()>(loaded, "SteamAPI_SteamUtils_v010")();
        if (!utils || symbol<std::uint32_t (*)(void*)>(loaded, "SteamAPI_ISteamUtils_GetAppID")(utils) != 3354750)
            throw std::runtime_error("Steam friend joining has a different app identity.");
        friends = symbol<void* (*)()>(loaded, "SteamAPI_SteamFriends_v017")();
        auto* user = symbol<void* (*)()>(loaded, "SteamAPI_SteamUser_v023")();
        if (!friends || !user) return;
        local_id = symbol<std::uint64_t (*)(void*)>(loaded, "SteamAPI_ISteamUser_GetSteamID")(user);
        set = symbol<decltype(set)>(loaded, "SteamAPI_ISteamFriends_SetRichPresence");
        get = symbol<decltype(get)>(loaded, "SteamAPI_ISteamFriends_GetFriendRichPresence");
        const auto register_callback = symbol<void (*)(CallbackBase*, int)>(loaded, "SteamAPI_RegisterCallback");
        // Instances live for the process lifetime, like the runtime DLL. Rely
        // on the game's callback pump; never drain or replace its callbacks.
        register_callback(&lobby_callback, 333);
        register_callback(&presence_callback, 337);
        module = loaded;
    }
    void presence(const std::string& desired) {
        const auto current = bounded(get(friends, local_id, "connect"));
        if (desired.empty()) {
            if (!owned.empty() && current == owned && !set(friends, "connect", previous.c_str())) return;
            owned.clear(); previous.clear();
        } else if (current != desired) {
            if (current != owned && !current.starts_with(steam_join_prefix)) previous = current;
            if (set(friends, "connect", desired.c_str())) owned = desired;
        } else owned = desired;
    }
};
}
void tick_steam_friend_join() noexcept {
    if (launcher::offline_mode()) return;
    static auto* api = new Api;
    const auto now = GetTickCount64();
    try {
        if (!api->module && now >= api->next_open) { api->next_open = now + 5000; api->open(); }
        if (!api->module || !api->user_handle()) return;
        std::deque<JoinEvent> events;
        { auto& queue = inbox(); std::lock_guard lock(queue.mutex); events.swap(queue.events); }
        for (const auto& event : events) {
            if (now - event.received > 10000) continue;
            // Ignore the base game's lobby events unless the friend explicitly
            // advertised this lobby using ReSkate's namespaced connect value.
            if (!event.rich && (!event.friend_id || steam_join_target(bounded(api->get(api->friends, event.friend_id, "connect"))) != event.lobby)) continue;
            queue_command("join-friend-lobby", std::to_string(event.lobby), {});
        }
        if (now >= api->next_presence) {
            api->next_presence = now + 2000;
            api->presence(steam_join_presence(model()));
            // Only written when it changes; cleared (an empty value removes the key) on leaving.
            if (auto session = steam_session_presence(model()); session != api->session &&
                api->set(api->friends, steam_session_key.data(), session.c_str()))
                api->session = std::move(session);
        }
    } catch (const std::exception& error) {
        if (!api->reported_error) {
            api->reported_error = true;
            logging::write(logging::Level::warning, logging::Channel::ui, error.what());
        }
    } catch (...) { }
}
}
