#pragma once
#include "Engine/Game/World/network_objects.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::server {
// Console lines about what players are doing: throwdowns (drops placed, queues, starts,
// turns, results) and objects placed or removed. Read from the packets the server relays
// anyway; nothing here changes what it sends.
class ActivityLog {
  public:
    using Log = std::function<void(const std::string &)>;
    // A connected player's name, or empty.
    using Name = std::function<std::string(std::uint64_t)>;
    // `announce` (optional) receives the lines worth telling the players in chat: a drop placed.
    ActivityLog(Log log, Name name, Log announce = {})
        : log_(std::move(log)), name_(std::move(name)), announce_(std::move(announce)) {}

    // One relayed throwdown message (Extension/Throwdowns/throwdown_wire.h) from `sender`;
    // `at` is where the sender skates (a drop is placed where its leader stands), if known.
    // `now` in microseconds.
    void throwdown(std::uint64_t sender, std::span<const std::uint8_t> message, const std::array<float, 3> *at,
                   std::uint64_t now);
    // What `owner` shares with everyone went from `before` to `after`.
    void objects(std::uint64_t owner, const std::vector<NetworkObject> &before, const std::vector<NetworkObject> &after);
    void left(std::uint64_t player);
    // Every throwdown went with the world (map change), or logging was turned off.
    void clear() { throwdowns_.clear(); }
    // Ends throwdowns that went quiet: a finished one sends nothing more.
    void tick(std::uint64_t now);
    // The players of the throwdowns running now, but those who quit theirs.
    std::set<std::uint64_t> playing() const;
    // Whether two players are in one throwdown now: its leader, its queue or its players, but
    // those who quit.
    bool together(std::uint64_t a, std::uint64_t b) const;

  private:
    struct Throwdown {
        std::string series;
        std::vector<std::uint64_t> queue;   // joined before the start (not the leader)
        std::vector<std::uint64_t> players; // from the start, in the leader's order
        std::set<std::uint64_t> quit;
        bool started{};
        std::uint64_t last{};                             // last message about it
        std::map<std::uint64_t, std::int64_t> total;      // Jam: running total; Spot Battle: best turn
        std::map<std::uint64_t, std::int64_t> turn;       // Spot Battle: this turn's points so far
        std::map<std::uint64_t, std::array<unsigned, 2>> tries; // S.K.A.T.E.: landed, missed
        std::uint64_t leading{}, lead_logged{};           // Jam
    };
    using Key = std::pair<std::uint64_t, std::uint32_t>; // leader, the leader's number for it

    std::string name(std::uint64_t player);
    std::string title(const Key &key, const Throwdown &t); // "Zee's S.K.A.T.E."
    void finish(const Key &key, const Throwdown &t);

    Log log_;
    Name name_;
    Log announce_;
    std::map<std::uint64_t, std::string> names_; // everyone seen, for lines after they left
    std::map<Key, Throwdown> throwdowns_;
    std::map<std::uint64_t, std::uint64_t> announced_; // leader -> their last drop told in chat
};
} // namespace dingosdk::server
