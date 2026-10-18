#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

// Domination's messages between players. They ride in the session's throwdown packets
// (PacketKind::throwdown, relayed opaque by hosts and dedicated servers to the same map and
// world), told apart from the throwdown relay's own messages by their first byte: the relay's
// kinds are 1-15, Domination's tag is 0xD0. The session hands them to Domination before the
// relay sees them (Extension/Multiplayer/Session/session.cpp). The sender is the packet's source.
//
// Little-endian: u8 tag 0xD0, u8 version, u8 type, u32 park, u32 round, then by type:
// - round (1): u64 starter, u8 action (1 running, 2 stopped, 3 the sender left it), u8 options (1 teams),
//   u16 duration (s), u32 elapsed (ms, as the sender timed it);
// - board (2): u16 count, then count claims: u8 spot, u64 player, f32 score, u32 ms (since the
//   round started, as the claimant timed it); at most max_board_entries;
// - teams (3): u8 count, then count of: u64 player, u8 team (0 or 1).
// Every participant sends the whole board it knows every few seconds, so a player who arrives
// late, or missed a message, ends up with the same board as everyone else.
namespace dingosdk::domination::wire {
inline constexpr std::uint8_t tag = 0xD0, version = 4;
inline constexpr std::size_t max_board_entries = 200, max_team_entries = 64;
enum Option : std::uint8_t { teams = 1 };
struct RoundMessage {
    std::uint32_t park{}, round{};
    std::uint64_t starter{};
    bool running{};
    std::uint8_t options{};
    std::uint16_t duration{}; // seconds to play (the countdown not counted)
    std::uint32_t elapsed{};  // ms since it started, as the sender timed it (stopped or left: when)
    bool left{};              // not running: the sender left it (it goes on for the others); else it was stopped
    bool operator==(const RoundMessage &) const = default;
};
struct Entry {
    std::uint8_t spot{};
    std::uint64_t player{};
    float score{};
    std::uint32_t ms{};
    bool operator==(const Entry &) const = default;
};
struct BoardMessage {
    std::uint32_t park{}, round{};
    std::vector<Entry> entries;
    bool operator==(const BoardMessage &) const = default;
};
struct TeamsMessage {
    std::uint32_t park{}, round{};
    std::vector<std::pair<std::uint64_t, std::uint8_t>> members;
    bool operator==(const TeamsMessage &) const = default;
};
using Message = std::variant<RoundMessage, BoardMessage, TeamsMessage>;
bool is_domination(std::span<const std::uint8_t> bytes) noexcept;
// The version a Domination message was written in (to tell a player on another build), if any.
std::optional<std::uint8_t> version_of(std::span<const std::uint8_t> bytes) noexcept;
// Throws std::invalid_argument for a board or team list longer than its limit.
std::vector<std::uint8_t> encode(const Message &message);
// Empty for anything malformed, of another version, or not Domination's.
std::optional<Message> decode(std::span<const std::uint8_t> bytes) noexcept;
}
