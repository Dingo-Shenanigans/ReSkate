#pragma once

#include <optional>
#include <string>
#include <string_view>

// One Thunderstore package looked up by name, for the game: the map a multiplayer host is on
// and this player does not have (Extension/Assets/map_download.h). No network here; the caller
// fetches details_url() and hands the answer to choose().
namespace dingosdk::thunderstore_package {

struct Name {
    std::string owner, name;
    std::string version;   // "1.2.3", or empty for the newest
    // The Mods folder the package installs as, and how its files name it.
    [[nodiscard]] std::string folder() const { return owner + '-' + name; }
    bool operator==(const Name&) const = default;
};
// "Owner-Name" or "Owner-Name-1.2.3", as a host names a map's package
// (Extension/Multiplayer/Net/protocol.h: valid_map_package). Nothing for anything else.
std::optional<Name> parse_name(std::string_view package);

// /api/experimental/package/<owner>/<name>/: what Thunderstore says of the package.
std::wstring details_url(const Name& name);
// Where a version of it downloads from. Always thunderstore.io, whatever an answer says.
std::wstring download_url(const Name& name, std::string_view version);

struct Choice {
    bool ok{};
    std::string version;   // the one to download: the one asked for, else the newest
    std::string newest;    // the newest, to fall back on when the one asked for is gone
    // What its page says of it, for the player: one line of plain text, cut to a few lines' worth.
    std::string description;
    // Its picture, a PNG on Thunderstore's own hosts; empty when the answer names anything else.
    std::string icon;
    std::string reason;    // why not, for the player
};
// Reads that answer. The package is taken only when it is the one asked for, is listed in
// `community` as a map (the "Maps" category), is not deprecated or marked NSFW there and was
// not rejected by the community's moderators.
Choice choose(std::string_view json, const Name& wanted, std::string_view community);

} // namespace dingosdk::thunderstore_package
