#pragma once
#include "Engine/Game/Multiplayer/session_model.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::multiplayer {
// Parses a dedicated server's game tags (Server/steam_server.cpp) into a
// browser row. Empty when the tags are not a compatible ReSkate server.
std::optional<MultiplayerLobby> read_server_tags(std::string_view tags, std::uint64_t steam_id);

// Dedicated ReSkate servers from Steam's server list for Skate. Rows come from
// Steam's master list, so a server shows even when a NAT hides its query port;
// what a server answers directly (name, ping) replaces the tag copy.
// Not shown: a server the ReSkate team has blocked, and, while the team requires login
// tokens (developer_identity.h), one that signed in to Steam without one, unless it is on
// the player's own network.
// Steam answers a search made soon after another with an empty internet list (seen
// 2026-10-06: every refresh within about 40 s of a full one). A search that lists no
// internet server therefore keeps the servers the last one found, for a while.
class SteamServerBrowser {
  public:
    ~SteamServerBrowser();
    void refresh(std::uint64_t now);
    void tick(std::uint64_t now);
    bool searching() const { return !searches_.empty(); }
    const std::vector<MultiplayerLobby> &rows() const { return rows_; }
    const MultiplayerLobby *find(std::uint64_t id) const;

  private:
    struct Search {
        void *request{}, *response{};
        bool internet{};
    };
    struct Found {
        MultiplayerLobby row;
        // Every address it was listed at: its public one, and a LAN one nearby.
        std::vector<std::pair<std::uint32_t, std::uint16_t>> addresses;
        bool answered{};
        std::uint64_t search{}, listed{}; // the search that last listed it, and when
        bool lan{};                       // answered on the player's own network
    };
    void *servers_{};
    std::vector<Search> searches_; // the internet and LAN lists
    std::map<std::uint64_t, Found> found_;
    std::uint64_t started_{}, next_poll_{};
    std::uint64_t search_{};  // counts refreshes
    bool internet_listed_{}; // this search's internet list had a server in it
    std::vector<MultiplayerLobby> rows_;
    void release();
    void read(std::uint64_t now);
};
} // namespace dingosdk::multiplayer
