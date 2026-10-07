#include "steam_transport.h"
#include "steam_lanes.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Engine/Core/Platform/launcher_support.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <chrono>
#include <dlfcn.h>
#endif
#ifdef _WIN32
#pragma warning(push, 0)
#endif
#include <isteamnetworkingsockets.h>
#ifdef _WIN32
#pragma warning(pop)
#endif
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
constexpr int virtual_port = 37;
#ifdef _WIN32
using NativeModule = HMODULE;
#else
using NativeModule = void *;
using ULONGLONG = std::uint64_t;
inline std::uint64_t tick_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
#ifndef GetTickCount64
#define GetTickCount64 tick_ms
#endif
#endif
template <class T> T symbol(NativeModule module, const char *name) {
#ifdef _WIN32
    const auto result = GetProcAddress(module, name);
#else
    const auto result = dlsym(module, name);
#endif
    if (!result)
        throw std::runtime_error(std::string("Missing Steam export: ") + name);
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(result);
#pragma warning(pop)
#else
    T value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
#endif
}
std::string bounded(const char *text, std::size_t size) {
    std::size_t length{};
    while (length < size && text[length])
        ++length;
    return std::string(text, length);
}
} // namespace
struct SteamTransport::Impl {
    void *sockets{};
    void *friends{};
    const char *(*persona_name)(void *, std::uint64_t){};
    bool (*request_name)(void *, std::uint64_t, bool){};
    ULONGLONG name_refresh{};

    TransportStatus state;
    SteamLanes lanes;
    HSteamListenSocket listener{};
    struct Link {
        HSteamNetConnection handle{};
        ULONGLONG connecting_since{};
        bool connected{};
        std::string name;
        bool prioritized{};
        // Steam's real-time status, read at most once per poll() frame and shared by
        // telemetry and every fresh send of that frame.
        SteamNetConnectionRealTimeStatus_t status{};
        std::array<SteamNetConnectionRealTimeLaneStatus_t, lane_priorities.size()> lanes{};
        std::uint64_t status_frame{};
        bool status_ok{};
        bool recheck{}; // a status callback named this connection: read its state this poll
    };
    std::map<std::uint64_t, Link> links;
    // How the connections Steam closed ended (take_closed). A handful at most wait here.
    std::map<std::uint64_t, std::string> closed;
    // What each connection may send a second, and Steam's call that changes an open one.
    int send_rate = connection_send_rate, listen_rate = connection_send_rate; // now, and when the listen socket opened
    void *utils{};
    bool (*set_config)(void *, ESteamNetworkingConfigValue, ESteamNetworkingConfigScope, intptr_t,
                       ESteamNetworkingConfigDataType, const void *){};
    // Gives one open connection the current send rate. A connection a listen socket
    // accepts starts with the rate the socket was opened with, so each one is given it.
    bool apply_rate(HSteamNetConnection handle) {
        if (!handle || !set_config || !utils) return false;
        bool ok = true;
        const auto set = [&](ESteamNetworkingConfigValue option, int32 value) {
            ok &= set_config(utils, option, k_ESteamNetworkingConfig_Connection, static_cast<intptr_t>(handle),
                             k_ESteamNetworkingConfig_Int32, &value);
        };
        // The minimum never above the maximum on the way, whatever the rate was before.
        set(k_ESteamNetworkingConfig_SendRateMin, min_send_rate);
        set(k_ESteamNetworkingConfig_SendRateMax, send_rate);
        set(k_ESteamNetworkingConfig_SendRateMin, send_rate);
        return ok;
    }
    // "ping 48 ms, quality 99%/97%, 0 B queued": what was last measured of a link.
    static std::string measured(const Link &link) {
        if (!link.status_ok) return "link not measured";
        const auto &s = link.status;
        const auto percent = [](float quality) { return quality < 0 ? std::string("?") : std::to_string(static_cast<int>(quality * 100.f + .5f)) + "%"; };
        return "ping " + std::to_string(s.m_nPing) + " ms, quality " + percent(s.m_flConnectionQualityLocal) + " here / " +
               percent(s.m_flConnectionQualityRemote) + " there, " + std::to_string(s.m_cbPendingReliable + s.m_cbPendingUnreliable) +
               " B queued (" + std::to_string(queue_time(s) / 1000) + " ms), " +
               std::to_string(static_cast<int>(s.m_flOutBytesPerSec / 1024.f + .5f)) + " KB/s out, " +
               std::to_string(static_cast<int>(s.m_flInBytesPerSec / 1024.f + .5f)) + " KB/s in";
    }
    std::uint64_t frame{};
    ULONGLONG next_measure{}, next_sweep{};
    struct Name {
        std::string text;
        ULONGLONG expires{};
    };
    std::map<std::uint64_t, Name> names;
    unsigned capacity = max_players;
    std::uint64_t host_id{};
    std::set<std::uint64_t> allowed;
    std::mutex events_mutex;
    std::deque<SteamNetConnectionStatusChangedCallback_t> events;
    static inline std::atomic<Impl *> callback_owner{};
    HSteamListenSocket (*listen)(void *, int, int, const SteamNetworkingConfigValue_t *){};
    HSteamNetConnection (*connect)(void *, const SteamNetworkingIdentity *, int, int,
                                   const SteamNetworkingConfigValue_t *){};
    EResult (*accept)(void *, HSteamNetConnection){};
    bool (*close)(void *, HSteamNetConnection, int, const char *, bool){};
    bool (*close_listener)(void *, HSteamListenSocket){};
    bool (*get_info)(void *, HSteamNetConnection, SteamNetConnectionInfo_t *){};
    EResult (*real_time)(void *, HSteamNetConnection, SteamNetConnectionRealTimeStatus_t *, int,
                         SteamNetConnectionRealTimeLaneStatus_t *){};
    EResult (*send_message)(void *, HSteamNetConnection, const void *, uint32, int, int64 *){};
    int (*receive_messages)(void *, HSteamNetConnection, SteamNetworkingMessage_t **, int){};
    void (*release_message)(SteamNetworkingMessage_t *){};
    void (*run_callbacks)(void *){};
    bool (*socket_pair)(void *, HSteamNetConnection *, HSteamNetConnection *, bool,
                        const SteamNetworkingIdentity *, const SteamNetworkingIdentity *){};

    static void changed(SteamNetConnectionStatusChangedCallback_t *event) noexcept {
        auto *p = callback_owner.load(std::memory_order_acquire);
        if (!p || !event)
            return;
        bool full{};
        try {
            std::lock_guard lock(p->events_mutex);
            full = p->events.size() >= 64;
            if (!full)
                p->events.push_back(*event);
        } catch (...) {
            full = true;
        }
        // The callback only queues transport state. It never touches the game.
        if (full && event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && p->close)
            p->close(p->sockets, event->m_hConn, 4001, "ReSkate callback queue full", false);
    }
    SteamNetworkingConfigValue_t callback_option() {
        SteamNetworkingConfigValue_t option{};
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                      reinterpret_cast<void *>(&changed));
        return option;
    }
    std::array<SteamNetworkingConfigValue_t, 5> options() {
        std::array<SteamNetworkingConfigValue_t, 5> out{};
        out[0] = callback_option();
        // This connection's send rate. Steam asks for the minimum and the maximum to be set
        // to the same value: with only the maximum raised a connection stays at the 256 KB/s
        // default, which a busy server fills for every player. Do not alter the game's
        // global Steam settings.
        listen_rate = send_rate;
        out[1].SetInt32(k_ESteamNetworkingConfig_SendRateMax, send_rate);
        out[3].SetInt32(k_ESteamNetworkingConfig_SendRateMin, send_rate);
        // Room for what must arrive. Steam refuses a reliable message once 512 KB wait, its
        // default, and a busy connection already holds half of that in poses: the refusal
        // then ends the session for a player whose connection was only slow.
        out[4].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 4 * 1024 * 1024);
        // Every connection goes through Steam's relays, which is how players who are not
        // Steam friends already connect: no player's IP address is shared with the others
        // in a session, whatever each one's own Steam setting is.
        out[2].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
                        k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable);
        return out;
    }
    void read_status(Link &link) {
        link.status = {};
        link.lanes = {};
        const auto count = link.prioritized ? static_cast<int>(link.lanes.size()) : 0;
        link.status_ok = real_time(sockets, link.handle, &link.status, count, count ? link.lanes.data() : nullptr) ==
                         k_EResultOK;
        link.status_frame = frame;
    }
    static std::span<const SteamNetConnectionRealTimeLaneStatus_t> lane_status(const Link &link) {
        return {link.lanes.data(), link.prioritized ? link.lanes.size() : 0};
    }
    // The connection list the session reads every frame. No Steam calls.
    void publish_links() {
        state.connected = false;
        state.peers.clear();
        state.peer_id = 0;
        state.peer_name.clear();
        for (const auto &[id, link] : links) {
            state.peers.push_back({id, link.connected});
            if (!state.peer_id) {
                state.peer_id = id;
                state.peer_name = link.name;
            }
            state.connected |= link.connected;
        }
    }
    // Connection quality for diagnostics and the upload budget: a few times a second.
    void measure() {
        state.telemetry = false;
        state.ping_ms = state.send_rate = state.pending_bytes = 0;
        state.queue_us = 0;
        state.cosmetic_queue_us = state.prioritized_connections = 0;
        state.outgoing_bps = state.incoming_bps = 0;
        state.delivery_local = state.delivery_remote = -1;
        for (auto &entry : links) {
            auto &link = entry.second;
            if (!link.connected) continue;
            if (link.prioritized) ++state.prioritized_connections;
            if (link.status_frame != frame) read_status(link);
            const auto &info = link.status;
            if (!link.status_ok || info.m_eState != k_ESteamNetworkingConnectionState_Connected)
                continue;
            state.telemetry = true;
            state.ping_ms = std::max(state.ping_ms, info.m_nPing);
            state.send_rate += info.m_nSendRateBytesPerSecond;
            state.pending_bytes += info.m_cbPendingReliable + info.m_cbPendingUnreliable;
            const auto queues = lane_status(link);
            state.queue_us = std::max(state.queue_us, lane_queue_time(info, queues, TrafficLane::gameplay));
            state.cosmetic_queue_us = std::max(state.cosmetic_queue_us,
                lane_queue_time(info, queues, TrafficLane::cosmetics));
            state.outgoing_bps += info.m_flOutBytesPerSec;
            state.incoming_bps += info.m_flInBytesPerSec;
            if (info.m_flConnectionQualityLocal >= 0)
                state.delivery_local = state.delivery_local < 0
                                           ? info.m_flConnectionQualityLocal
                                           : std::min(state.delivery_local, info.m_flConnectionQualityLocal);
            if (info.m_flConnectionQualityRemote >= 0)
                state.delivery_remote =
                    state.delivery_remote < 0
                        ? info.m_flConnectionQualityRemote
                        : std::min(state.delivery_remote, info.m_flConnectionQualityRemote);
        }
    }
    // The checks send() makes before Steam sees a message.
    bool admit(Link &link, std::span<const std::uint8_t> bytes, bool fresh, TrafficLane lane) {
        if (!link.connected || bytes.size() > max_packet)
            return false;
        if (fresh) {
            if (link.status_frame != frame) read_status(link);
            if (link.status_ok && lane_congested(link.status, lane_status(link), lane)) {
                ++state.skipped;
                return false; // Let the next current pose/audio state replace this one.
            }
        }
        return true;
    }
    static int send_flags(bool reliable, TrafficLane lane) {
        return reliable ? (lane == TrafficLane::cosmetics ? k_nSteamNetworkingSend_Reliable
                                                          : k_nSteamNetworkingSend_ReliableNoNagle)
                        : k_nSteamNetworkingSend_UnreliableNoDelay;
    }
    bool record(EResult result, bool reliable, std::span<const std::uint8_t> bytes) {
        if (result != k_EResultOK) {
            if (result == k_EResultIgnored && !reliable) {
                ++state.skipped;
                return false;
            }
            ++state.dropped;
            ++state.send_failures;
            return false;
        }
        ++state.sent;
        state.sent_bytes += bytes.size();
        state.raw_sent_bytes += wire_original_size(bytes);
        return true;
    }
};
SteamTransport::SteamTransport() : impl_(std::make_unique<Impl>()) {}
SteamTransport::~SteamTransport() {
    stop();
    Impl::callback_owner.store(nullptr);
}
// Binds the networking calls for `sockets` (the user's or a game server's).
bool SteamTransport::bind(void *library, void *sockets, void *networking_utils) {
    auto &p = *impl_;
    const auto module = static_cast<NativeModule>(library);
    p.sockets = sockets;
    if (!p.sockets)
        throw std::runtime_error("Steam Networking Sockets v012 is unavailable.");
#define BIND(member, suffix)                                                                                     p.member = symbol<decltype(p.member)>(module, "SteamAPI_ISteamNetworkingSockets_" suffix)
    BIND(listen, "CreateListenSocketP2P");
    BIND(connect, "ConnectP2P");
    BIND(accept, "AcceptConnection");
    BIND(close, "CloseConnection");
    BIND(close_listener, "CloseListenSocket");
    BIND(get_info, "GetConnectionInfo");
    BIND(real_time, "GetConnectionRealTimeStatus");
    BIND(send_message, "SendMessageToConnection");
    BIND(receive_messages, "ReceiveMessagesOnConnection");
    BIND(run_callbacks, "RunCallbacks");
    BIND(socket_pair, "CreateSocketPair");
#undef BIND
    p.release_message =
        symbol<decltype(p.release_message)>(module, "SteamAPI_SteamNetworkingMessage_t_Release");
    SteamNetworkingIdentity identity{};
    if (!symbol<bool (*)(void *, SteamNetworkingIdentity *)>(
            module, "SteamAPI_ISteamNetworkingSockets_GetIdentity")(p.sockets, &identity) ||
        identity.m_eType != k_ESteamNetworkingIdentityType_SteamID || !identity.GetSteamID64())
        throw std::runtime_error("Steam identity is unavailable.");
    p.state.local_id = identity.GetSteamID64();
    if (!networking_utils)
        throw std::runtime_error("Steam networking utilities are unavailable.");
    // Older Steam adapters retain the existing single-lane send path.
    try {
        p.lanes.utils = networking_utils;
        p.lanes.configure = symbol<decltype(p.lanes.configure)>(module,
            "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes");
        p.lanes.allocate = symbol<decltype(p.lanes.allocate)>(module,
            "SteamAPI_ISteamNetworkingUtils_AllocateMessage");
        p.lanes.send = symbol<decltype(p.lanes.send)>(module,
            "SteamAPI_ISteamNetworkingSockets_SendMessages");
    } catch (...) {
        p.lanes = {};
    }
    p.utils = networking_utils;
    try {
        p.set_config = symbol<decltype(p.set_config)>(module, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    } catch (...) {
        p.set_config = nullptr; // open connections keep their rate; new ones get the new one
    }
    symbol<void (*)(void *)>(module,
                             "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess")(networking_utils);
    symbol<ESteamNetworkingAvailability (*)(void *)>(
        module, "SteamAPI_ISteamNetworkingSockets_InitAuthentication")(p.sockets);
    Impl *expected = nullptr;
    if (!Impl::callback_owner.compare_exchange_strong(expected, &p) && expected != &p)
        throw std::runtime_error("Another ReSkate transport already owns callbacks.");
    p.state.ready = true;
    p.state.detail = "Steam ready. Relay authentication may still be connecting.";
    return true;
}
bool SteamTransport::open() {
    auto &p = *impl_;
    if (p.state.ready)
        return true;
#ifdef _WIN32
    try {
        if (launcher::offline_mode())
            throw std::runtime_error("Multiplayer is unavailable in offline mode. Start Steam and relaunch ReSkate.");
        const auto module = GetModuleHandleW(L"steam_api64.dll");
        if (!module)
            throw std::runtime_error("Steam DLL is not loaded. Start ReSkate with Steam running.");
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size())
            throw std::runtime_error("Cannot verify Steam DLL location.");
        path.resize(length);
        launcher::validate_steam_api_file(std::filesystem::path(path));
        const auto user = symbol<int (*)()>(module, "SteamAPI_GetHSteamUser")();
        if (!user)
            throw std::runtime_error(
                "The game has not initialized Steam. Keep Steam online and restart ReSkate.");
        void *utils = symbol<void *(*)()>(module, "SteamAPI_SteamUtils_v010")();
        if (!utils || symbol<uint32 (*)(void *)>(module, "SteamAPI_ISteamUtils_GetAppID")(utils) != 3354750)
            throw std::runtime_error("Steam app identity does not match skate.");
        // Cosmetic lookup is optional; missing Friends support must not stop P2P.
        try {
            p.friends = symbol<void *(*)()>(module, "SteamAPI_SteamFriends_v017")();
            p.persona_name =
                symbol<decltype(p.persona_name)>(module, "SteamAPI_ISteamFriends_GetFriendPersonaName");
            p.request_name =
                symbol<decltype(p.request_name)>(module, "SteamAPI_ISteamFriends_RequestUserInformation");
        } catch (...) {
            p.friends = nullptr;
        }
        void *sockets = nullptr;
        for (const char *name : {"SteamAPI_SteamNetworkingSockets_SteamAPI_v013",
                                 "SteamAPI_SteamNetworkingSockets_SteamAPI_v012"}) {
            try {
                sockets = symbol<void *(*)()>(module, name)();
                break;
            } catch (...) {
            }
        }
        if (!sockets) throw std::runtime_error("Missing Steam export: SteamAPI_SteamNetworkingSockets_SteamAPI");
        return bind(module, sockets,
                    symbol<void *(*)()>(module, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004")());
    } catch (const std::exception &e) {
        p.state.detail = e.what();
        return false;
    }
#else
    // Client game path is Windows-only. Dedicated servers use open_game_server().
    p.state.detail = "Client multiplayer is only supported on Windows.";
    return false;
#endif
}
bool SteamTransport::open_game_server(void *library) {
    auto &p = *impl_;
    if (p.state.ready)
        return true;
    try {
        const auto module = static_cast<NativeModule>(library);
        const auto user = symbol<int (*)()>(module, "SteamGameServer_GetHSteamUser")();
        if (!user)
            throw std::runtime_error("The Steam game server is not initialized.");
        const auto find = symbol<void *(*)(int, const char *)>(module, "SteamInternal_FindOrCreateGameServerInterface");
        // Game servers have no friends list; names come from each player's hello.
        p.friends = nullptr;
        // Sockets v012 (Skate's Windows DLL) vs v013 (current SDK): same methods
        // used here, v013 is a superset. Try new first, fall back to old.
        void *sockets = nullptr;
        for (const char *name : {"SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013",
                                 "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012"}) {
            try {
                sockets = symbol<void *(*)()>(module, name)();
                break;
            } catch (...) {
            }
        }
        if (!sockets) throw std::runtime_error("Missing Steam export: SteamAPI_SteamGameServerNetworkingSockets_SteamAPI");
        return bind(module, sockets, find(user, "SteamNetworkingUtils004"));
    } catch (const std::exception &e) {
        p.state.detail = e.what();
        return false;
    }
}
void SteamTransport::disconnect(std::uint64_t id, const char *reason) {
    auto &p = *impl_;
    const auto found = p.links.find(id);
    if (found == p.links.end())
        return;
    const auto handle = found->second.handle;
    p.links.erase(found);
    // Closed from this side, unless Steam's own account of the end is already kept.
    if (p.closed.size() >= 256) p.closed.clear();
    p.closed.try_emplace(id, std::string("closed by this side: ") + (reason ? reason : ""));
    if (handle && p.close)
        p.close(p.sockets, handle, 1000, reason, false);
    p.state.detail = reason;
    p.publish_links();
}
void SteamTransport::stop() {
    auto &p = *impl_;
    const auto listener = p.listener;
    p.listener = 0;
    p.state.hosting = false;
    p.allowed.clear();
    p.host_id = 0;
    if (listener && p.close_listener)
        p.close_listener(p.sockets, listener);
    while (!p.links.empty())
        disconnect(p.links.begin()->first, "Disconnected.");
    // Drain pending incoming handles before discarding callbacks.
    if (p.state.ready)
        poll();
    // Telemetry is measured a few times a second: without links, clear it now rather
    // than show the old session's until the next measurement.
    p.measure();
    p.next_measure = 0;
}
bool SteamTransport::host(unsigned capacity) {
    stop();
    if (!open())
        return false;
    auto &p = *impl_;
    if (capacity < 2 || capacity > max_players)
        return false;
    p.capacity = capacity;
    auto options = p.options();
    p.listener = p.listen(p.sockets, virtual_port, static_cast<int>(options.size()), options.data());
    p.state.hosting = p.listener != 0;
    p.state.detail = p.listener ? "Waiting for players to join." : "Steam could not open the P2P listener.";
    return p.listener != 0;
}
bool SteamTransport::join(std::uint64_t id) {
    stop();
    if (!open())
        return false;
    auto &p = *impl_;
    if (id == p.state.local_id) {
        p.state.detail = "Use Local Echo to test on one machine, or join from another Steam account.";
        return false;
    }
    p.host_id = id;
    p.allowed.insert(id);
    p.capacity = max_players;
    auto options = p.options();
    // Guests listen only for identities admitted in the host's reliable roster.
    // Failure to listen leaves the host-forwarded path usable.
    p.listener = p.listen(p.sockets, virtual_port, static_cast<int>(options.size()), options.data());
    const bool connected = connect_peer(id);
    p.state.detail = connected ? "Connecting through Steam..." : "Steam rejected the connection request.";
    return connected;
}
bool SteamTransport::connect_peer(std::uint64_t id) {
    auto &p = *impl_;
    if (!p.state.ready || p.state.hosting || !p.allowed.contains(id) || id == p.state.local_id)
        return false;
    if (p.links.contains(id))
        return true;
    SteamNetworkingIdentity identity{};
    identity.SetSteamID64(id);
    auto options = p.options();
    const auto connection =
        p.connect(p.sockets, &identity, virtual_port, static_cast<int>(options.size()), options.data());
    p.closed.erase(id);
    if (connection)
        p.links.emplace(id, Impl::Link{connection, GetTickCount64(), false, name(id),
                                      p.lanes.setup(p.sockets, connection)});
    p.publish_links();
    return connection != 0;
}
void SteamTransport::allow_peers(std::span<const Member> members) {
    auto &p = *impl_;
    if (p.state.hosting || !p.host_id)
        return;
    p.allowed.clear();
    p.allowed.insert(p.host_id);
    for (const auto &m : members)
        if (m.id != p.state.local_id)
            p.allowed.insert(m.id);
    for (auto it = p.links.begin(); it != p.links.end();) {
        const auto id = (it++)->first;
        if (!p.allowed.contains(id))
            disconnect(id, "Player left the host roster.");
    }
}
void SteamTransport::poll() {
    auto &p = *impl_;
    if (!p.state.ready)
        return;
    ++p.frame;
    p.run_callbacks(p.sockets);
    const auto now = GetTickCount64();
    // Link names only label the first connection in diagnostics; the session keeps its own.
    if (now >= p.name_refresh) {
        p.name_refresh = now + 30000;
        for (auto &[id, link] : p.links)
            link.name = name(id);
    }
    std::deque<SteamNetConnectionStatusChangedCallback_t> events;
    {
        std::lock_guard lock(p.events_mutex);
        events.swap(p.events);
    }
    for (const auto &event : events) {
        if (event.m_info.m_eState != k_ESteamNetworkingConnectionState_Connecting ||
            !event.m_info.m_hListenSocket) {
            // A tracked connection changed state: read it below in this poll.
            for (auto &entry : p.links)
                if (entry.second.handle == event.m_hConn)
                    entry.second.recheck = true;
            continue;
        }
        const auto id = event.m_info.m_identityRemote.GetSteamID64();
        const auto existing = p.links.find(id);
        if (existing != p.links.end() && existing->second.handle == event.m_hConn)
            continue;
        if (event.m_info.m_hListenSocket != p.listener || !p.listener || !id || id == p.state.local_id ||
            (!p.state.hosting && !p.allowed.contains(id)) || p.links.size() >= p.capacity - 1 ||
            existing != p.links.end() ||
            event.m_info.m_identityRemote.m_eType != k_ESteamNetworkingIdentityType_SteamID) {
            p.close(p.sockets, event.m_hConn, 4002, "ReSkate session full or unavailable", false);
            continue;
        }
        if (p.accept(p.sockets, event.m_hConn) != k_EResultOK) {
            p.close(p.sockets, event.m_hConn, 4003, "Cannot accept connection", false);
            continue;
        }
        p.closed.erase(id);
        p.links.emplace(id, Impl::Link{event.m_hConn, now, false, name(id),
                                      p.lanes.setup(p.sockets, event.m_hConn)});
        if (p.send_rate != p.listen_rate) p.apply_rate(event.m_hConn);
    }
    // Connected links report changes through the status callback (above), so read their
    // state only then, plus once a second in case a callback was dropped.
    const bool sweep = now >= p.next_sweep;
    if (sweep)
        p.next_sweep = now + 1000;
    for (auto it = p.links.begin(); it != p.links.end();) {
        const auto id = it->first;
        auto &link = (it++)->second;
        if (link.connected && !link.recheck && !sweep)
            continue;
        link.recheck = false;
        SteamNetConnectionInfo_t info{};
        if (!p.get_info(p.sockets, link.handle, &info)) {
            disconnect(id, "Steam connection disappeared.");
            continue;
        }
        if (info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
            if (!link.connected && !link.prioritized) link.prioritized = p.lanes.setup(p.sockets, link.handle);
            link.connected = true;
            p.state.detail = "Steam connected.";
        } else if (info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                   info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            const auto detail = bounded(info.m_szEndDebug, sizeof(info.m_szEndDebug));
            // Steam's end reasons: 1xxx the application closed it, 3xxx a problem at this end,
            // 4xxx one at the other end (4001: it stopped answering), 5xxx anything else.
            if (p.closed.size() >= 256) p.closed.clear();
            p.closed[id] = std::string(info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? "closed by their game"
                                                                                                    : "connection lost") +
                           ", Steam reason " + std::to_string(info.m_eEndReason) + (detail.empty() ? std::string{} : " \"" + detail + "\"") +
                           "; last " + Impl::measured(link);
            disconnect(id, detail.empty() ? "Steam connection closed." : detail.c_str());
        } else if (now - link.connecting_since > 20000)
            disconnect(id, "Steam connection timed out.");
    }
    if (now >= p.next_measure) {
        p.next_measure = now + 250;
        p.measure();
    }
    p.publish_links();
}
std::string SteamTransport::take_closed(std::uint64_t id) {
    auto &p = *impl_;
    const auto found = p.closed.find(id);
    if (found == p.closed.end()) return {};
    auto text = std::move(found->second);
    p.closed.erase(found);
    return text;
}
int SteamTransport::send_rate() const { return impl_->send_rate; }
bool SteamTransport::set_send_rate(int rate) {
    auto &p = *impl_;
    if (rate < min_send_rate || rate > max_send_rate) return false;
    p.send_rate = rate;
    bool all = true;
    for (auto &[id, link] : p.links)
        if (link.handle) all &= p.apply_rate(link.handle);
    return all;
}
std::int64_t SteamTransport::pending(std::uint64_t id) const {
    const auto found = impl_->links.find(id);
    if (found == impl_->links.end() || !found->second.status_ok) return 0;
    return std::int64_t{found->second.status.m_cbPendingReliable} + found->second.status.m_cbPendingUnreliable;
}
std::string SteamTransport::link_report(std::uint64_t id) {
    auto &p = *impl_;
    const auto found = p.links.find(id);
    if (found == p.links.end()) return "no connection";
    if (found->second.handle && found->second.connected) p.read_status(found->second);
    return Impl::measured(found->second);
}
std::vector<TransportLink> SteamTransport::links() {
    auto &p = *impl_;
    std::vector<TransportLink> out;
    out.reserve(p.links.size());
    for (auto &[id, link] : p.links) {
        TransportLink row;
        row.id = id;
        row.connected = link.connected;
        if (link.handle && link.connected) p.read_status(link);
        if (link.connected && link.status_ok) {
            const auto &s = link.status;
            row.measured = true;
            row.ping_ms = s.m_nPing;
            row.pending_bytes = s.m_cbPendingReliable + s.m_cbPendingUnreliable;
            row.send_rate = s.m_nSendRateBytesPerSecond;
            row.quality_local = s.m_flConnectionQualityLocal;
            row.quality_remote = s.m_flConnectionQualityRemote;
            row.out_bps = s.m_flOutBytesPerSec;
            row.in_bps = s.m_flInBytesPerSec;
            row.queue_us = queue_time(s);
            SteamNetConnectionInfo_t info{};
            if (p.get_info && p.get_info(p.sockets, link.handle, &info)) {
                // A location's ID is its name's characters packed into a number.
                const auto named = [](SteamNetworkingPOPID id) {
                    std::string text;
                    for (const unsigned shift : {16U, 8U, 0U, 24U})
                        if (const auto c = static_cast<char>((id >> shift) & 0xff); c > 32 && c < 127) text.push_back(c);
                    return text;
                };
                row.relay = named(info.m_idPOPRelay);
                row.remote_relay = named(info.m_idPOPRemote);
            }
        }
        out.push_back(row);
    }
    return out;
}
std::string SteamTransport::name(std::uint64_t id) {
    auto &p = *impl_;
    if (!p.friends || !id)
        return {};
    // Each lookup is two calls into the Steam client. Names change rarely: ask again
    // after 10 s, or after 1 s while Steam does not know the name yet.
    const auto now = GetTickCount64();
    if (const auto cached = p.names.find(id); cached != p.names.end() && now < cached->second.expires)
        return cached->second.text;
    p.request_name(p.friends, id, true);
    const auto *value = p.persona_name(p.friends, id);
    auto text = value ? bounded(value, 128) : std::string{};
#ifdef _WIN32
    while (!text.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                 static_cast<int>(text.size()), nullptr, 0))
        text.pop_back();
#else
    // Minimal UTF-8 truncation: drop trailing continuation bytes (game servers
    // never reach here anyway; names come from hello messages).
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0xC0) text.pop_back();
#endif
    for (auto &c : text)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            c = ' ';
    if (text == "[unknown]")
        text.clear();
    if (p.names.size() >= 1024)
        p.names.clear();
    p.names[id] = {text, now + (text.empty() ? 1000U : 10000U)};
    return text;
}
bool SteamTransport::send(std::uint64_t id, std::span<const std::uint8_t> bytes, bool reliable, bool fresh,
                          TrafficLane lane) {
    auto &p = *impl_;
    const auto found = p.links.find(id);
    if (found == p.links.end() || !p.admit(found->second, bytes, fresh, lane))
        return false;
    const int flags = Impl::send_flags(reliable, lane);
    const auto result = found->second.prioritized
        ? p.lanes.transmit(p.sockets, found->second.handle, bytes, flags, lane)
        : p.send_message(p.sockets, found->second.handle, bytes.data(), static_cast<uint32>(bytes.size()), flags, nullptr);
    return p.record(result, reliable, bytes);
}
void SteamTransport::send_batch(std::span<TransportSend> messages) {
    auto &p = *impl_;
    std::vector<SteamNetworkingMessage_t *> batch;
    std::vector<std::size_t> owners;
    batch.reserve(messages.size());
    owners.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        auto &m = messages[i];
        m.sent = false;
        const auto found = p.links.find(m.id);
        if (found == p.links.end() || !p.admit(found->second, m.bytes, m.fresh, m.lane))
            continue;
        const int flags = Impl::send_flags(m.reliable, m.lane);
        const auto &link = found->second;
        if (!link.prioritized || m.bytes.empty()) {
            // Without lanes (older Steam adapters) each message goes out as send() sends it.
            const auto result = link.prioritized
                ? p.lanes.transmit(p.sockets, link.handle, m.bytes, flags, m.lane)
                : p.send_message(p.sockets, link.handle, m.bytes.data(), static_cast<uint32>(m.bytes.size()), flags,
                                 nullptr);
            m.sent = p.record(result, m.reliable, m.bytes);
            continue;
        }
        auto *message = p.lanes.allocate(p.lanes.utils, static_cast<int>(m.bytes.size()));
        if (!message) {
            m.sent = p.record(k_EResultLimitExceeded, m.reliable, m.bytes);
            continue;
        }
        std::memcpy(message->m_pData, m.bytes.data(), m.bytes.size());
        message->m_conn = link.handle;
        message->m_nFlags = flags;
        message->m_idxLane = static_cast<uint16>(m.lane);
        batch.push_back(message);
        owners.push_back(i);
    }
    if (batch.empty())
        return;
    std::vector<int64> results(batch.size());
    // As in SteamLanes::transmit: Steam owns/frees every message on success and failure.
    p.lanes.send(p.sockets, static_cast<int>(batch.size()), batch.data(), results.data(), true);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        auto &m = messages[owners[i]];
        const auto result = results[i] > 0 ? k_EResultOK
                          : results[i] < 0 ? static_cast<EResult>(-results[i]) : k_EResultFail;
        m.sent = p.record(result, m.reliable, m.bytes);
    }
}
std::vector<TransportMessage> SteamTransport::receive() {
    auto &p = *impl_;
    std::vector<TransportMessage> result;
    for (auto it = p.links.begin(); it != p.links.end();) {
        const auto id = it->first;
        const auto &link = (it++)->second;
        if (!link.connected)
            continue;
        // Everything that has arrived, not one batch a call: a dedicated server sends a busy
        // session's player a few thousand messages a second, and what a slow frame leaves
        // behind would wait in Steam, later and later, taking memory until it is read.
        bool failed{};
        for (unsigned batch = 0; batch < 64 && !failed; ++batch) {
            SteamNetworkingMessage_t *messages[128]{};
            const int count = p.receive_messages(p.sockets, link.handle, messages, 128);
            if (count < 0) {
                failed = true;
                break;
            }
            for (int i = 0; i < count; ++i) {
                auto *message = messages[i];
                if (!message)
                    continue;
                struct Release {
                    Impl &p;
                    SteamNetworkingMessage_t *m;
                    ~Release() { p.release_message(m); }
                } release{p, message};
                if (message->m_cbSize <= 0 || message->m_cbSize > static_cast<int>(max_packet) ||
                    !message->m_pData) {
                    ++p.state.dropped;
                    ++p.state.invalid_messages;
                    continue;
                }
                const auto *data = static_cast<const std::uint8_t *>(message->m_pData);
                result.push_back({id, {data, data + message->m_cbSize},
                                  message->m_usecTimeReceived > 0 ? static_cast<std::uint64_t>(message->m_usecTimeReceived) : 0});
                ++p.state.received;
                p.state.received_bytes += static_cast<std::uint64_t>(message->m_cbSize);
            }
            if (count < 128) break;
        }
        if (failed) disconnect(id, "Steam receive failed.");
    }
    return result;
}
bool SteamTransport::socket_test() {
    auto &p = *impl_;
    if (!open())
        return false;
    HSteamNetConnection a{}, b{};
    if (!p.socket_pair(p.sockets, &a, &b, false, nullptr, nullptr)) {
        p.state.detail = "Steam socket-pair creation failed.";
        return false;
    }
    struct Close {
        Impl &p;
        HSteamNetConnection a, b;
        ~Close() {
            p.close(p.sockets, a, 1000, "Test complete", false);
            p.close(p.sockets, b, 1000, "Test complete", false);
        }
    } close{p, a, b};
    Packet probe;
    probe.kind = PacketKind::away;
    probe.session = 1;
    probe.epoch = 1;
    const auto bytes = encode(probe);
    const bool prioritized = p.lanes.setup(p.sockets, a);
    for (unsigned lane = 0; lane < (prioritized ? 3U : 1U); ++lane) {
        const auto result = prioritized
            ? p.lanes.transmit(p.sockets, a, bytes, k_nSteamNetworkingSend_ReliableNoNagle, static_cast<TrafficLane>(lane))
            : p.send_message(p.sockets, a, bytes.data(), static_cast<uint32>(bytes.size()),
                             k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
        if (result != k_EResultOK) {
            p.state.detail = "Steam socket-pair send failed.";
            return false;
        }
        SteamNetworkingMessage_t *message{};
        const int count = p.receive_messages(p.sockets, b, &message, 1);
        if (count != 1 || !message) {
            p.state.detail = "Socket-pair receive not ready; run the test again.";
            return false;
        }
        const bool ok = message->m_cbSize == static_cast<int>(bytes.size()) && message->m_pData &&
                        message->m_idxLane == lane && std::memcmp(message->m_pData, bytes.data(), bytes.size()) == 0;
        p.release_message(message);
        if (!ok) {
            p.state.detail = "Steam socket-pair payload or lane mismatch.";
            return false;
        }
    }
    p.state.detail = prioritized ? "Steam socket-pair round trip passed on all three priority lanes."
                                : "Steam socket-pair round trip passed (single-lane fallback).";
    return true;
}
const TransportStatus &SteamTransport::status() const { return impl_->state; }
} // namespace dingosdk::multiplayer
