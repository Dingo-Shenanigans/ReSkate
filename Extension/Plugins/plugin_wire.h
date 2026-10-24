#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A plugin's message to the same plugin on other players' games, sent in the session's throwdown
// packets. Throwdown kinds are 1-15 and 0xD0 is an earlier game-mode tag, still kept apart; plugins' is
// 0xD1, and 0xD2 for what the session's authority says (the server copy of a plugin, on the
// lobby's host or the dedicated server), which only the authority may send. Little-endian: u8 tag,
// u8 version, u32 channel (fnv1a of the plugin's lowercase name), u64 the player it is for (0:
// everyone, 1: the authority), then the payload. The host fills in all but the payload, so a plugin
// cannot pretend to be another or say who sent it (the session says that).
namespace dingosdk::plugins::wire {
inline constexpr std::uint8_t tag = 0xD1, authority_tag = 0xD2, version = 1;
inline constexpr std::size_t header = 14;
// `to` for the authority: no SteamID64 is this small.
inline constexpr std::uint64_t authority = 1;

inline std::uint32_t channel_of(std::string_view name) {
    std::uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash ^= static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        hash *= 16777619u;
    }
    return hash;
}
inline bool is_plugin(std::span<const std::uint8_t> bytes) noexcept {
    return !bytes.empty() && (bytes[0] == tag || bytes[0] == authority_tag);
}
inline bool from_authority(std::span<const std::uint8_t> bytes) noexcept { return !bytes.empty() && bytes[0] == authority_tag; }
inline std::uint64_t to_of(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t to = 0;
    if (bytes.size() >= header)
        for (int i = 0; i < 8; ++i) to |= static_cast<std::uint64_t>(bytes[6 + i]) << (8 * i);
    return to;
}
// Whether a relay (the lobby's host, the dedicated server) passes a guest's packet on to everyone:
// not a plugin message for the authority alone, nor one claiming to be the authority's.
inline bool relayed(std::span<const std::uint8_t> bytes) noexcept {
    return !is_plugin(bytes) || (!from_authority(bytes) && to_of(bytes) != authority);
}

inline std::vector<std::uint8_t> encode(std::uint32_t channel, std::uint64_t to, std::string_view payload, bool authority_says = false) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(header + payload.size());
    bytes.push_back(authority_says ? authority_tag : tag);
    bytes.push_back(version);
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(channel >> (8 * i)));
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(to >> (8 * i)));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
struct Message {
    std::uint32_t channel{};
    std::uint64_t to{};
    std::string payload;
    bool authority{}; // said by the session's authority
};
// Empty for anything that is not a plugin message of this version.
inline std::optional<Message> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header || !is_plugin(bytes) || bytes[1] != version) return std::nullopt;
    Message message;
    message.authority = from_authority(bytes);
    for (int i = 0; i < 4; ++i) message.channel |= static_cast<std::uint32_t>(bytes[2 + i]) << (8 * i);
    for (int i = 0; i < 8; ++i) message.to |= static_cast<std::uint64_t>(bytes[6 + i]) << (8 * i);
    message.payload.assign(reinterpret_cast<const char*>(bytes.data() + header), bytes.size() - header);
    return message;
}
}
