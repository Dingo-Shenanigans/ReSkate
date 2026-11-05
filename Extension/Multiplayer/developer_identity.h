#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace dingosdk::multiplayer {
// Who the ReSkate backend lists in each of its Steam ID categories, and who it
// has banned. The lists are kept in its admin panel and read from
// api.reskate.dev, so they change without a release. Steam authenticates these
// identities. Each category has its tag in chat and on nametags (player_role)
// and its animation on the marked hoodie and board (developer_hoodie_material.h).
// The last list is not of players: the dedicated servers the ReSkate team runs, which the
// server browser puts first. A server is on it by the Steam ID its login token gives it.
// And one of servers the team has blocked: the browser does not show them and the game does
// not join them. And one of players who may not host a lobby (banned_host, below).
enum class IdentityList : std::uint8_t { developer, homie, content_creator, centrix, banned, official_server, staff, blocked_server, banned_host, count };
using IdentityLists = std::array<std::vector<std::uint64_t>, static_cast<std::size_t>(IdentityList::count)>;
inline constexpr std::string_view identity_lists_url = "https://api.reskate.dev/api/v1/steam-ids";

// The game's way of reading them (the dedicated server's is Server/global_bans.h). Called
// every client tick: reads the lists in the background, at startup and every ten
// minutes after (a minute after a failure). Offline mode reads them too, for the
// player's own hoodie and board.
void refresh_identity_lists() noexcept;
// Any thread. Nobody is listed until the first answer has arrived.
bool identity_listed(std::uint64_t id, IdentityList list) noexcept;
// Shared by the developer tag and the rainbow hoodie and board.
inline bool reskate_developer(std::uint64_t id) noexcept { return identity_listed(id, IdentityList::developer); }
// The lists that give a tag, a developer's before the staff's before Centrix's before a content
// creator's before a homie's: the order a player's tag and items come from when they are on
// several. Most players are in none.
inline constexpr std::array<IdentityList, 5> tag_lists{IdentityList::developer, IdentityList::staff, IdentityList::centrix,
                                                       IdentityList::content_creator, IdentityList::homie};
// Their names in the profile and in the mark-tag-list command, as the backend names them.
inline constexpr std::array<std::string_view, 5> tag_list_names{"dev", "staff", "centrix", "content_creator", "homie"};
inline std::optional<IdentityList> tag_list_named(std::string_view name) noexcept {
    for (std::size_t i = 0; i < tag_lists.size(); ++i)
        if (tag_list_names[i] == name) return tag_lists[i];
    return std::nullopt;
}
inline std::string_view tag_list_name(IdentityList list) noexcept {
    for (std::size_t i = 0; i < tag_lists.size(); ++i)
        if (tag_lists[i] == list) return tag_list_names[i];
    return {};
}
// The category a player's items come from, and their tag unless they chose another
// (identity_tag): the first of tag_lists they are on.
inline std::optional<IdentityList> identity_mark(std::uint64_t id) noexcept {
    for (const auto list : tag_lists)
        if (identity_listed(id, list)) return list;
    return std::nullopt;
}
// No choice of tag: the first list by identity_mark's order.
inline constexpr std::uint8_t usual_tag = 0xff;
// The tag a player shows: the list they chose (Appearance::tag, an IdentityList's number) while
// they are on it. A list they are not on, one without a tag, or no choice (usual_tag, and every
// older build) is identity_mark's. What their items may do still comes from identity_mark, so
// showing a homie's tag does not take a developer's rainbow away, nor give a homie's gold.
inline std::optional<IdentityList> identity_tag(std::uint64_t id, std::uint8_t chosen) noexcept {
    for (const auto list : tag_lists)
        if (static_cast<std::uint8_t>(list) == chosen && identity_listed(id, list)) return list;
    return identity_mark(id);
}
// The local player's own choices to go without their tag, and without their coloured items
// (the Skater page's STYLE tab). Their appearance tells everyone else (Appearance::hide_tag,
// Appearance::hide_items).
inline std::atomic<bool> own_tag{true}, own_items{true};
inline bool own_tag_shown() noexcept { return own_tag.load(std::memory_order_relaxed); }
inline void show_own_tag(bool shown) noexcept { own_tag.store(shown, std::memory_order_relaxed); }
inline bool own_items_shown() noexcept { return own_items.load(std::memory_order_relaxed); }
inline void show_own_items(bool shown) noexcept { own_items.store(shown, std::memory_order_relaxed); }
// And which of their lists' tags shows, for a player on more than one (Appearance::tag);
// usual_tag for identity_mark's.
inline std::atomic<std::uint8_t> own_tag_list{usual_tag};
inline std::uint8_t own_tag_choice() noexcept { return own_tag_list.load(std::memory_order_relaxed); }
inline void choose_own_tag(std::uint8_t list) noexcept { own_tag_list.store(list, std::memory_order_relaxed); }
// A global ban is enforced by whoever a player would play with: a host and a dedicated server
// turn them away, and a guest leaves a lobby they host, which their own game does not start
// either (session_receive.cpp, Server/server_host.cpp). A server can opt out ("global_bans"),
// so a banned player's own game still lets them try to join. Nobody is banned while the lists
// cannot be read.
inline bool reskate_banned(std::uint64_t id) noexcept { return identity_listed(id, IdentityList::banned); }
// A dedicated server the ReSkate team runs. Steam vouches for the ID: only the holder of the
// server's login token can sign in as it.
inline bool official_server(std::uint64_t id) noexcept { return identity_listed(id, IdentityList::official_server); }
inline bool blocked_server(std::uint64_t id) noexcept { return identity_listed(id, IdentityList::blocked_server); }
inline constexpr std::string_view blocked_server_notice = "This server has been blocked by the ReSkate team.";
// A player the team has stopped from hosting lobbies ("banned_hosts"); they still play on
// servers and in other players' lobbies. Their own game does not host, and since that is theirs
// to change, everyone else's does the rest: the lobby browser leaves their lobbies out and the
// game does not join one or stay in one.
inline bool banned_host(std::uint64_t id) noexcept { return identity_listed(id, IdentityList::banned_host); }
inline constexpr std::string_view banned_host_notice = "You are banned from hosting ReSkate lobbies. You can still join servers and other players' lobbies.";
inline constexpr std::string_view banned_host_lobby_notice = "This player has been banned from hosting lobbies by the ReSkate team.";
// Whether the server browser shows only dedicated servers signed in with a Steam login token
// (protocol.h: persistent_server_steam_id), which the backend switches on and off beside the
// lists ("server_tokens_required"). Such a server keeps its Steam ID, so a block holds on it.
// Players' own lobbies, servers on the player's network and servers joined by code are not
// affected. Off until the backend says otherwise.
inline std::atomic<bool> server_tokens_rule{};
inline bool server_tokens_required() noexcept { return server_tokens_rule.load(std::memory_order_relaxed); }
// What the API's answer says about that; false when it says nothing. Throws on an answer
// that is not JSON.
bool parse_server_tokens_required(std::string_view json);
inline constexpr std::string_view banned_notice = "You are banned from ReSkate multiplayer.";

// The API's answer, {"categories":{"dev":["7656119..."],"homie":[],...},"banned":[],
// "official_servers":["8556839..."],"blocked_servers":[],"banned_hosts":[]}, as sorted lists. A category this build does not know
// is ignored and a list the answer leaves out is empty; an entry that is not a player's
// SteamID64 (for the servers: a token server's) throws.
IdentityLists parse_identity_lists(std::string_view json);
// Puts sorted lists in use. False when they are the ones already in use.
bool publish_identity_lists(IdentityLists lists);
} // namespace dingosdk::multiplayer
