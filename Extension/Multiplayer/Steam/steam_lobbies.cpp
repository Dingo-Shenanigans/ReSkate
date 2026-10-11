#include "steam_lobbies.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/Build/supported_build.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace dingosdk::multiplayer {
namespace {
bool safe_text(std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
} // namespace
std::optional<MultiplayerLobby> read_lobby(std::uint64_t id, std::uint64_t owner,
                                           const std::function<std::string(const char *)> &get) {
    if (!id || get("rs_kind") != "reskate-co-skate" ||
        get("rs_protocol") != std::to_string(protocol_version) ||
        get("rs_build") != supported_build::game_sha256 || get("rs_open") != "1")
        return {};
    MultiplayerLobby row{id, owner, get("rs_name"), get("rs_map"), get("rs_code"), false, 1};
    const auto code = parse_invite(row.code);
    auto number = [&](const char *key, int &out) {
        const auto text = get(key);
        const auto value = std::from_chars(text.data(), text.data() + text.size(), out);
        return value.ec == std::errc{} && value.ptr == text.data() + text.size();
    };
    // GetLobbyOwner is only valid AFTER joining. Browsers read the advertised
    // host from metadata; the join completion checks against the actual owner.
    if (!code || (owner && code->steam_id != owner) || !safe_text(row.map, 256) ||
        !safe_text(row.name, 128) || !number("rs_players", row.players) ||
        !number("rs_capacity", row.capacity) || row.capacity < 2 ||
        row.capacity > static_cast<int>(max_players) || row.players < 1 || row.players >= row.capacity)
        return {};
    const auto password = get("rs_password");
    if (password != "0" && password != "1")
        return {};
    row.password_required = password == "1";
    row.owner = code->steam_id;
    return row;
}
void SteamLobbies::close_lobby() {
    if (const auto lobby = std::exchange(lobby_, 0)) {
        api_->joinable(lobby, false);
        api_->leave(lobby);
    }
    state_.listed = false;
    published_.clear();
}
void SteamLobbies::stop() {
    host_wanted_ = ready_ = false;
    creating_.cancelled = joining_.cancelled = true;
    state_.joining = false;
    joined_.reset();
    close_lobby();
    state_.hosting.clear();
}
void SteamLobbies::host(const std::string &code, unsigned capacity, bool password_required, std::string_view name) {
    require(name.empty() || safe_text(name, 128), "Lobby names must be at most 128 bytes with no control characters.");
    stop();
    const auto invite = parse_invite(code);
    require(invite.has_value(), "Host join code is invalid.");
    require(capacity >= 2 && capacity <= multiplayer_lobby_player_limit, "Invalid lobby capacity.");
    password_required_ = password_required;
    capacity_ = capacity;
    players_ = 1;
    code_ = code;
    host_name_ = name;
    local_id_ = invite->steam_id;
    host_wanted_ = true;
    failed_ = false;
    state_.hosting = "Public lobby: waiting for a loaded map.";
}
void SteamLobbies::publish_host() {
    if (!lobby_)
        return;
    require(api_->owner(lobby_) == local_id_, "Steam lobby ownership changed. Restart hosting.");
    const bool open = ready_ && players_ < capacity_;
    const auto signature = map_ + code_ + std::to_string(players_) + "/" + std::to_string(capacity_) +
                           (open ? "open" : "closed");
    if (signature == published_)
        return;
    // Hide first, then publish a complete compatible record before exposing it.
    require(api_->joinable(lobby_, false), "Steam could not close the lobby listing.");
    state_.listed = false;
    const std::pair<const char *, std::string> values[]{
        {"rs_kind", "reskate-co-skate"},
        {"rs_protocol", std::to_string(protocol_version)},
        {"rs_build", std::string(supported_build::game_sha256)},
        {"rs_name", host_name_},
        {"rs_map", map_},
        {"rs_code", code_},
        {"rs_players", std::to_string(players_)},
        {"rs_capacity", std::to_string(capacity_)},
        {"rs_password", password_required_ ? "1" : "0"},
        {"rs_open", open ? "1" : "0"}};
    for (const auto &[key, value] : values)
        require(api_->data(lobby_, key, value), "Steam could not publish the lobby details.");
    require(api_->visibility(lobby_, true), "Steam could not make the lobby public.");
    if (open)
        require(api_->joinable(lobby_, true), "Steam could not open the lobby listing.");
    state_.listed = open;
    state_.hosting = !ready_ ? "Host is changing maps; the public listing will reopen after loading."
                    : !open ? "Public lobby is full; it will reopen when a player leaves."
                           : "Public lobby is listed on Steam.";
    published_ = signature;
}
void SteamLobbies::update_host(bool ready, unsigned players, std::string_view map, std::uint64_t now) {
    if (!host_wanted_ || failed_)
        return;
    players = std::clamp(players, 1U, capacity_);
    // The session calls this every frame. With nothing changed since the listing was
    // published there is nothing to write; still confirm ownership once a second.
    if (lobby_ && ready && ready_ && players == players_ && map == map_ && !published_.empty() &&
        now >= owner_checked_ && now - owner_checked_ < 1000000)
        return;
    ready_ = ready;
    players_ = players;
    map_ = map;
    try {
        if (!ready_) {
            if (lobby_ && (state_.listed || !published_.empty())) {
                require(api_->joinable(lobby_, false), "Steam could not pause the lobby listing.");
                require(api_->data(lobby_, "rs_open", "0"), "Steam could not pause lobby discovery.");
                state_.listed = false;
                published_.clear();
                state_.hosting = "Host is changing maps; existing players stay connected.";
            }
            return;
        }
        require(safe_text(map_, 256), "Load a supported map before publishing a lobby.");
        if (!lobby_ && !creating_.call) {
            api_->open();
            if (host_name_.empty()) host_name_ = api_->name();
            if (!safe_text(host_name_, 128))
                host_name_ = "ReSkate session";
            creating_ = {api_->create(capacity_), now, false};
            require(creating_.call != 0, "Steam could not create a lobby.");
            state_.hosting = "Creating a public Steam lobby...";
        }
        publish_host();
        owner_checked_ = now;
    } catch (const std::exception &e) {
        failed_ = true;
        close_lobby();
        state_.hosting = std::string("Public listing failed: ") + e.what() + " Join code still works.";
    }
}
void SteamLobbies::refresh(std::uint64_t now) {
    if (searching_.call || (last_search_ && now - last_search_ < 5000000))
        return;
    try {
        api_->open();
        searching_ = {api_->search(), now, false};
        require(searching_.call != 0, "Steam could not request the lobby list.");
        last_search_ = now;
        state_.searching = true;
        state_.rows.clear();
        state_.browser = "Searching Steam for open ReSkate lobbies...";
    } catch (const std::exception &e) {
        state_.browser = e.what();
    }
}
void SteamLobbies::join(std::uint64_t id, std::uint64_t local_id, std::uint64_t now) {
    require(!joining_.call, "A previous lobby request is still finishing.");
    require(id != 0, "Select a lobby before joining.");
    api_->open();
    const auto row =
        std::find_if(state_.rows.begin(), state_.rows.end(), [id](const auto &v) { return v.id == id; });
    require(row != state_.rows.end(), "Lobby is no longer in the browser. Refresh the list.");
    require(row->owner != local_id, "You cannot join your own lobby.");
    friend_join_ = false;
    join_map_ = row->map;
    join_owner_ = row->owner;
    join_code_ = row->code;
    join_id_ = id;
    joining_ = {api_->join(id), now, false};
    require(joining_.call != 0, "Steam could not join the lobby.");
    state_.joining = true;
    state_.browser = "Checking that the lobby is still open...";
}
void SteamLobbies::join_friend(std::uint64_t id, std::uint64_t local_id, std::uint64_t now) {
    require(!joining_.call, "A previous lobby request is still finishing.");
    require(id && local_id, "That Steam lobby is unavailable.");
    api_->open();
    friend_join_ = true;
    local_id_ = local_id;
    join_id_ = id;
    join_owner_ = 0;
    join_map_.clear(); join_code_.clear();
    joining_ = {api_->join(id), now, false};
    require(joining_.call != 0, "Steam could not join the lobby.");
    state_.joining = true;
    state_.browser = "Checking your friend's lobby...";
}
void SteamLobbies::tick(std::uint64_t now) {
    if (now - last_poll_ < 100000)
        return;
    last_poll_ = now;
    auto poll = [&](Pending &pending, LobbyCall kind, auto &&done) {
        if (!pending.call)
            return;
        if (const auto result = api_->poll(pending.call, kind)) {
            const bool cancelled = pending.cancelled;
            pending = {};
            done(*result, cancelled);
        } else if (!pending.cancelled && now - pending.since > 25000000) {
            // Do not forget timed-out create/join calls: a late successful result
            // still joins a lobby, so drain it and leave even after cancellation.
            pending.cancelled = true;
            if (kind == LobbyCall::create) {
                failed_ = true;
                state_.hosting = "Public listing timed out. Join code still works; restart hosting to retry.";
            } else {
                state_.searching = kind == LobbyCall::list ? false : state_.searching;
                state_.joining = kind == LobbyCall::join ? false : state_.joining;
                state_.browser = "Steam lobby request timed out. Waiting for Steam to finish cleanup.";
            }
        }
    };
    poll(creating_, LobbyCall::create, [&](const LobbyResult &result, bool cancelled) {
        if (cancelled || !host_wanted_) {
            if (result.ok && result.lobby)
                api_->leave(result.lobby);
            return;
        }
        if (!result.ok || !result.lobby) {
            failed_ = true;
            state_.hosting =
                "Steam lobby creation failed (" + std::to_string(result.result) + "). Join code still works.";
            return;
        }
        lobby_ = result.lobby;
        // Metadata/visibility are published by update_host after current map and
        // connection readiness have been evaluated on this client tick.
    });
    poll(searching_, LobbyCall::list, [&](const LobbyResult &result, bool cancelled) {
        state_.searching = false;
        if (cancelled)
            return;
        state_.searched = true;
        state_.rows.clear();
        if (!result.ok) {
            state_.browser = "Steam lobby search failed. Refresh to retry.";
            return;
        }
        // Lobbies named with bad words are never listed, like dedicated servers; the host
        // can still share the join code.
        for (std::uint32_t i = 0; i < std::min(result.count, 50U); ++i) {
            const auto id = api_->at(static_cast<int>(i));
            const auto row = read_lobby(id, 0, [&](const char *key) { return api_->data(id, key); });
            if (row && !text::contains_bad_words(row->name) && !(hidden_host_ && hidden_host_(row->owner)) &&
                std::none_of(state_.rows.begin(), state_.rows.end(),
                             [id](const auto &v) { return v.id == id; }))
                state_.rows.push_back(*row);
        }
        state_.browser = state_.rows.empty()
                             ? "No compatible public lobbies found."
                             : std::to_string(state_.rows.size()) + " open public lobbies found.";
    });
    poll(joining_, LobbyCall::join, [&](const LobbyResult &result, bool cancelled) {
        state_.joining = false;
        struct Leave {
            LobbyApi &api;
            std::uint64_t id;
            ~Leave() {
                if (id)
                    api.leave(id);
            }
        } leave{*api_, result.lobby};
        if (cancelled)
            return;
        if (!result.ok || result.lobby != join_id_) {
            state_.browser = "Lobby is unavailable or full. Refresh and try again.";
            return;
        }
        const auto owner = api_->owner(result.lobby);
        const auto row =
            read_lobby(result.lobby, owner, [&](const char *key) { return api_->data(result.lobby, key); });
        if (!owner || !row || (friend_join_ ? owner == local_id_ :
            owner != join_owner_ || row->code != join_code_ || map_hash(row->map) != map_hash(join_map_))) {
            state_.browser = "Lobby changed or closed. Refresh the list before joining.";
            return;
        }
        if (friend_join_ && row->password_required) {
            state_.browser = "This lobby needs a password. Join it from the server browser.";
            return;
        }
        // The lobby is a discovery directory. Actual membership/capacity follows
        // the P2P connection, including guests who use the direct join code.
        joined_ = row;
        state_.browser = "Lobby verified. Connecting to the host...";
    });
}
std::optional<MultiplayerLobby> SteamLobbies::take_join() { return std::exchange(joined_, {}); }
} // namespace dingosdk::multiplayer
