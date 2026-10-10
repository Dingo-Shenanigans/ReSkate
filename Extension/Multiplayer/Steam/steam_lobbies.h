#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include <memory>
#include <functional>

namespace dingosdk::multiplayer {
enum class LobbyCall { create, list, join };
struct LobbyResult {
    bool ok{};
    std::uint64_t lobby{};
    std::uint32_t count{}, result{};
};
// Narrow Steam adapter; the asynchronous lifecycle is tested with a fake backend.
struct LobbyApi {
    virtual ~LobbyApi() = default;
    virtual void open() = 0;
    virtual std::uint64_t create(unsigned capacity) = 0;
    virtual std::uint64_t search() = 0;
    virtual std::uint64_t join(std::uint64_t) = 0;
    virtual std::optional<LobbyResult> poll(std::uint64_t, LobbyCall) = 0;
    virtual void leave(std::uint64_t) = 0;
    virtual std::uint64_t at(int) = 0;
    virtual std::uint64_t owner(std::uint64_t) = 0;
    virtual std::string data(std::uint64_t, const char *) = 0;
    virtual bool data(std::uint64_t, const char *, const std::string &) = 0;
    virtual bool joinable(std::uint64_t, bool) = 0;
    virtual bool visibility(std::uint64_t, bool) = 0;
    virtual std::string name() = 0;
};
std::unique_ptr<LobbyApi> make_steam_lobby_api();
std::optional<MultiplayerLobby> read_lobby(std::uint64_t id, std::uint64_t owner,
                                           const std::function<std::string(const char *)> &get);
struct LobbyStatus {
    bool listed{}, searching{}, joining{}, searched{};
    std::string hosting, browser = "Refresh to find public ReSkate lobbies.";
    std::vector<MultiplayerLobby> rows;
};
class SteamLobbies {
  public:
    // `hidden_host`: whether a player's lobbies are left out of the browser (the team's bans).
    explicit SteamLobbies(std::unique_ptr<LobbyApi> api, std::function<bool(std::uint64_t)> hidden_host = {})
        : api_(std::move(api)), hidden_host_(std::move(hidden_host)) {}
    void host(const std::string &code, unsigned capacity = multiplayer_lobby_player_limit, bool password_required = false,
              std::string_view name = {});
    void update_host(bool ready, unsigned players, std::string_view map, std::uint64_t now);
    void refresh(std::uint64_t now);
    void join(std::uint64_t id, std::uint64_t local_id, std::uint64_t now);
    void join_friend(std::uint64_t id, std::uint64_t local_id, std::uint64_t now);
    void stop();
    void tick(std::uint64_t now);
    std::optional<MultiplayerLobby> take_join();
    const LobbyStatus &status() const { return state_; }
    std::uint64_t hosted_lobby() const { return state_.listed ? lobby_ : 0; }

  private:
    struct Pending {
        std::uint64_t call{}, since{};
        bool cancelled{};
    };
    std::unique_ptr<LobbyApi> api_;
    std::function<bool(std::uint64_t)> hidden_host_;
    LobbyStatus state_;
    Pending creating_, searching_, joining_;
    bool host_wanted_{}, failed_{}, ready_{}, password_required_{};
    bool friend_join_{};
    unsigned players_ = 1, capacity_ = multiplayer_lobby_player_limit;
    std::uint64_t lobby_{}, local_id_{}, last_poll_{}, last_search_{}, join_id_{}, join_owner_{};
    std::uint64_t owner_checked_{}; // update_host's last full publication and ownership check
    std::string code_, map_, host_name_, published_, join_map_, join_code_;
    std::optional<MultiplayerLobby> joined_;
    void close_lobby();
    void publish_host();
};
} // namespace dingosdk::multiplayer
