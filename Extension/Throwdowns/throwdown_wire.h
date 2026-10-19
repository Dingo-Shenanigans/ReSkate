#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Messages that link the players' own throwdowns (analysis/throwdowns-native-mp.md).
// Carried opaque in PacketKind::throwdown; the sender is the packet's source.
namespace dingosdk::multiplayer {
struct ThrowdownMessage {
    enum class Kind : std::uint8_t {
        offer = 1, // the leader opened a queue: everyone else shows it hosted by the leader
        close = 2, // the leader's queue is gone without this player (cancelled, or started without them)
        join = 3,  // the sender joined the leader's queue
        leave = 4, // the sender left it
        start = 5, // the leader's queue started
        score = 6, // the sender's running score in the leader's throwdown (Jam)
        row = 7,   // one leaderboard write of the sender's, as its client made it (Spot Battle)
        turn_end = 8, // the sender's value-th turn ended
        attempt = 9,  // the sender's S.K.A.T.E. attempt in its value-th turn (add = landed)
        // Coop challenges (analysis/coop-challenges-mp.md). `leader` started challenge `challenge`
        // of series `series`; `id` is its own number for this run.
        challenge_start = 10,   // order = the participants (leader first); add = contest mode
        challenge_optout = 11,  // the sender does not take part (declined, busy, or its copy failed)
        challenge_attempt = 12, // the sender finished an attempt: its AttemptFinishedForCriteria arrays
        challenge_slam = 13,    // the sender hit slam target `value` (its Identifier)
        challenge_leave = 14,   // the sender quit its copy
        // Party beacons (analysis/party-re/beacons.md): the sender's beacon in this world, `id` its
        // revision. add = it stands at `location` (placed or moved); else it was removed.
        beacon = 15
    };
    Kind kind = Kind::offer;
    std::uint64_t leader{}; // Steam ID of the player hosting the throwdown
    std::uint32_t id{};     // the leader's own number for it
    // offer: mode and the leader's CreateThrowdownQueueParams as recorded from its
    // UpdateThrowdownQueueInfo (placement) and HostSetThrowdownParameters (settings).
    // start: the leader's final settings (S.K.A.T.E.: the setup frozen for the match; else empty).
    std::string series;
    std::vector<std::uint8_t> placement, settings;
    // offer: the players already in the leader's queue (so a late viewer's copy has them);
    // start: the participants, in the leader's order (never empty).
    std::vector<std::uint64_t> order;
    std::int32_t value{};             // score, row
    std::uint8_t board{};             // row: the leaderboard's index in the event's leaderboard manager
    bool add{};                       // row: added to the row (else the row is set to value); attempt: landed
    std::array<std::uint8_t, 28> trick{}; // attempt: the CompositeTrickRecord, bit-exact
    std::string challenge;                // challenge_start: the challenge's Id (e.g. Plot-014-OTS-03)
    // challenge_attempt: CriteriaData[] (0x14 bytes each) and SentIndexes[] (i32 each), bit-exact.
    std::vector<std::uint8_t> criteria, indexes;
    // beacon: the LinearTransform it stands at (right, up, forward, translation; 4 floats each).
    std::array<float, 16> location{};
    // offer, start, attempt: the S.K.A.T.E. setup's eight trick-category switches (0 for other modes).
    std::uint8_t rules_mask{};
    // attempt: the S.K.A.T.E. trick detector's normalized result as JSON (<= 3500 bytes); empty
    // when no detector judged the attempt.
    std::vector<std::uint8_t> detected;
    bool operator==(const ThrowdownMessage &) const = default;
};
constexpr std::size_t max_throwdown_series = 48, max_throwdown_params = 1536, max_throwdown_order = 32,
                      max_throwdown_boards = 16, max_challenge_id = 96, max_challenge_criteria = 64,
                      challenge_criteria_size = 0x14, max_challenge_players = 4,
                      max_detected_attempt = 3500;
bool valid_throwdown(const ThrowdownMessage &) noexcept;
// Throws std::invalid_argument for an invalid message.
std::vector<std::uint8_t> encode_throwdown(const ThrowdownMessage &);
std::optional<ThrowdownMessage> decode_throwdown(std::span<const std::uint8_t>) noexcept;
} // namespace dingosdk::multiplayer
