#pragma once
#include "steam_transport.h"
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <isteamnetworkingsockets.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <cstring>
#include <limits>

namespace dingosdk::multiplayer {
// Gameplay uses lane zero to avoid extra lane-index bytes on frequent packets.
// Control goes first; gameplay/cosmetics share remaining bandwidth 16:1 so an
// outfit can still finish while players keep moving. Unused shares are available.
inline constexpr std::array<int, 4> lane_priorities{1, 0, 1, 1};
inline constexpr std::array<uint16, 4> lane_weights{16, 1, 1, 4};
struct SteamLanes {
    void *utils{};
    EResult (*configure)(void *, HSteamNetConnection, int, const int *, const uint16 *){};
    SteamNetworkingMessage_t *(*allocate)(void *, int){};
    void (*send)(void *, int, SteamNetworkingMessage_t **, int64 *, bool){};
    bool setup(void *sockets, HSteamNetConnection connection) const {
        return utils && configure && allocate && send &&
            configure(sockets, connection, static_cast<int>(lane_priorities.size()),
                      lane_priorities.data(), lane_weights.data()) == k_EResultOK;
    }
    EResult transmit(void *sockets, HSteamNetConnection connection,
                     std::span<const std::uint8_t> bytes, int flags, TrafficLane lane) const {
        if (bytes.empty() || bytes.size() > max_packet) return k_EResultInvalidParam;
        auto *message = allocate(utils, static_cast<int>(bytes.size()));
        if (!message) return k_EResultLimitExceeded;
        std::memcpy(message->m_pData, bytes.data(), bytes.size());
        message->m_conn = connection;
        message->m_nFlags = flags;
        message->m_idxLane = static_cast<uint16>(lane);
        int64 result{};
        // Steam owns/frees the allocation on both success and failure. Never
        // access or release message after this call, and never retry it here.
        send(sockets, 1, &message, &result, true);
        return result > 0 ? k_EResultOK : result < 0 ? static_cast<EResult>(-result) : k_EResultFail;
    }
};
inline std::uint64_t lane_queue_time(const SteamNetConnectionRealTimeStatus_t &info,
    std::span<const SteamNetConnectionRealTimeLaneStatus_t> lanes, TrafficLane lane) {
    if (info.m_eState != k_ESteamNetworkingConnectionState_Connected) return 0;
    const auto index = static_cast<std::size_t>(lane);
    const auto time = index < lanes.size() ? lanes[index].m_usecQueueTime : info.m_usecQueueTime;
    // Steam uses INT64_MAX when it cannot estimate the queue, including while
    // connecting. This is not congestion; the pending-byte guard still applies.
    if (time == std::numeric_limits<SteamNetworkingMicroseconds>::max()) return 0;
    return static_cast<std::uint64_t>(std::max<SteamNetworkingMicroseconds>(0, time));
}
// A whole connection's queue time, with the same care for Steam's "cannot estimate" value.
inline std::uint64_t queue_time(const SteamNetConnectionRealTimeStatus_t &info) {
    return lane_queue_time(info, {}, TrafficLane::gameplay);
}
inline bool lane_congested(const SteamNetConnectionRealTimeStatus_t &info,
    std::span<const SteamNetConnectionRealTimeLaneStatus_t> lanes, TrafficLane lane) {
    if (info.m_eState != k_ESteamNetworkingConnectionState_Connected) return false;
    const auto index = static_cast<std::size_t>(lane);
    const auto pending = index < lanes.size()
        ? std::int64_t{lanes[index].m_cbPendingReliable} + lanes[index].m_cbPendingUnreliable
        : std::int64_t{info.m_cbPendingReliable} + info.m_cbPendingUnreliable;
    return lane_queue_time(info, lanes, lane) > 75000 || pending > 256 * 1024;
}
} // namespace dingosdk::multiplayer
