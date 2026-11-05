#pragma once
#include "received.h"
#include "sharing_rules.h"
#include "synchros_wire.h"
#include "wearer.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Skate Synchros between this game and each other player in the session, both ways at once on one
// side connection (SteamTransport::open_side): this player as a viewer asking for their outfit, and
// as a wearer answering them (synchros_wire.h has the messages).
//
// Viewer:  connected -> Hello (proofs of every code and group held)
//          Welcome(allowed) -> Want (the worn items this PC cannot show)
//          Contents -> remembered in the cache; Fetch what is not there yet
//          Piece... -> kept once whole; when nothing is missing, outfit_ready
// Wearer:  Hello -> Welcome (whether a proof matched: sharing_rules.h)
//          Want -> Contents;  Fetch -> Piece... (paced to the side connection's queue)
// Anything malformed, out of order or refused ends that player's exchange.
namespace dingosdk::multiplayer::synchros {

// The side connection, as the exchange needs it.
class Channel {
public:
    virtual ~Channel() = default;
    virtual bool send(std::uint64_t peer, std::span<const std::uint8_t> bytes) = 0;
    // Bytes waiting to go to `peer`; negative when unknown.
    [[nodiscard]] virtual std::int64_t pending(std::uint64_t peer) const = 0;
    virtual void close(std::uint64_t peer) = 0;
};

// The game, as the exchange needs it.
struct Game {
    // The item keys a player in the session has on (their outfit packet).
    std::function<std::set<std::string>(std::uint64_t)> worn_by;
    // Whether this PC can show an item already (the game's own, or an installed mod's).
    std::function<bool(std::string_view)> can_show;
    // What this player has on.
    std::function<std::set<std::string>()> wearing;
    // Whether a key is one of the game's own items (never sent).
    std::function<bool(std::string_view)> game_item;
    // Every file of `wearer`'s items is in the cache now: their outfit can be made into a mod.
    std::function<void(std::uint64_t)> outfit_ready;
    // A line for the log.
    std::function<void(const std::string&)> note;
};

class Exchange final {
public:
    Exchange(Settings& settings, Cache& cache, Closet& closet, Channel& channel, Game game, std::uint64_t me);

    // A new session: what was granted for the last one goes.
    void begin(std::uint64_t session);
    void connected(std::uint64_t peer);
    void closed(std::uint64_t peer);
    void received(std::uint64_t peer, std::span<const std::uint8_t> bytes);
    // Sends what is waiting, paced; call every frame or so.
    void tick();
    // A player's outfit changed (their outfit packet): asked for again if they let this player in.
    void outfit_changed(std::uint64_t peer);
    // This player's outfit changed: what each viewer may still fetch follows it.
    void own_outfit_changed();
    // Blocking: their exchange ends, both ways.
    void blocked(std::uint64_t peer);
    // The settings changed (a code entered, a group joined, sharing or showing switched): each
    // player connected now is asked again, and told again whether they may see this player's.
    void settings_changed();

    struct Status {
        bool welcomed{};         // they let this player see their outfit
        bool sharing{};          // this player lets them see theirs
        std::size_t items{}, files_missing{};
        std::size_t custom_shaders{};   // their items left out for custom shaders (Withheld::custom_shaders)
        std::uint64_t bytes_received{};
        std::string refused;     // why it stopped, if it did
    };
    [[nodiscard]] Status status(std::uint64_t peer) const;
    // Whether the settings changed (a player paired with this player's code) since last asked.
    [[nodiscard]] bool take_settings_changed() noexcept;

    // Bytes a wearer keeps waiting in the side connection's queue at most.
    static constexpr std::int64_t queue_limit = 256 * 1024;
    // Files asked for in one Fetch.
    static constexpr std::size_t fetch_batch = 256;

private:
    struct Peer {
        bool linked{};             // the side connection is up
        // As a viewer.
        bool hello_sent{}, welcomed{};
        std::vector<std::uint8_t> asked_with;   // the last Hello as sent, so a change is noticed
        std::set<std::string> asked;
        std::vector<ItemContents> listing;
        std::vector<ContentId> to_fetch;        // not asked for yet
        std::map<ContentId, File> expected;     // asked for, by id
        std::map<ContentId, Arriving> arriving;
        std::uint64_t bytes_received{};
        std::set<std::string> custom_shaders;   // their items withheld for custom shaders
        bool ready_told{};
        // As a wearer.
        bool hello_seen{}, sharing{};
        Proofs proofs;             // the viewer's, kept to judge again when the settings change
        std::unique_ptr<Outfit> outfit;
        std::string refused;
    };
    void send(std::uint64_t peer, const Message& message);
    void hello(std::uint64_t peer, Peer& p, bool again);
    void judge(std::uint64_t peer, Peer& p);
    void refuse(std::uint64_t peer, const std::string& why);
    void ask(std::uint64_t peer, Peer& p);
    void fetch_more(std::uint64_t peer, Peer& p);
    void check_ready(std::uint64_t peer, Peer& p);
    void note(const std::string& line) const;

    Settings& settings_;
    Cache& cache_;
    Closet& closet_;
    Channel& channel_;
    Game game_;
    std::uint64_t me_{}, session_{};
    SessionGrants grants_;
    std::map<std::uint64_t, Peer> peers_;
    bool settings_changed_{};
};

} // namespace dingosdk::multiplayer::synchros
