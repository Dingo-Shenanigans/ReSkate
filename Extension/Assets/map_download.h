#pragma once
#include "Extension/Multiplayer/Session/session.h"
#include <cstdint>
#include <string>
#include <string_view>

// A map a multiplayer session needs and this PC does not have, fetched from Thunderstore
// while the game runs. The host says which package its map is from (session.h: MapNeed). The
// player is asked; on yes the package is downloaded, installed into Mods and applied as a live
// mod (live_mods.h). The session stays connected meanwhile and loads the map once it is there;
// on no, or when the map cannot be had, the session is left.
//
// Only maps come in this way. The package has to be listed as a map in ReSkate's Thunderstore
// community, not deprecated and not rejected by its moderators; what is installed has to be a
// map and nothing else (level bundles and their registration: no cosmetics, no settings, no
// scripts), or it is taken out again. Everything is downloaded from thunderstore.io, whatever
// a host says.
namespace dingosdk::map_download {
enum class Stage {
    idle,
    checking,    // asking Thunderstore about the package; nothing is shown yet
    asking,      // the card asks: download, or not
    downloading,
    installing,
    applying,    // the live merge
    joining,     // the map is in the game: the session is loading it (or is joined again)
};
struct View {
    Stage stage{};
    std::string map, package;   // "Vancouver Plaza", "Sandos-Vancouver_Plaza 2.0.0"
    std::string author, version, description; // "Sandos", "2.0.0", and what its Thunderstore page says of it
    // applying: the live merge's steps done of all, and the one in hand.
    std::size_t step{}, steps{};
    std::string step_name;
    bool server{};              // a dedicated server's map, else a player's lobby
    bool moved{};               // the session changed to it with the player in it (else: joining it)
    bool installed{};           // asking: the mod is here but switched off, so nothing is downloaded
    bool choice{true};          // asking: the answer a controller has picked (true: download)
    std::uint64_t received{}, total{};   // downloading; total 0 when the size is not known
    std::uint32_t yes_bind{}, no_bind{}; // the vote binds, which answer the card
};
// Client thread: a session just ended over this map. Ignored while another is in hand.
void offer(const multiplayer::MapNeed& need);
// Any thread. Yes downloads (when asking); no declines, or stops a download under way.
void answer(bool download) noexcept;
bool asking() noexcept;
// A controller's pick between the two answers, and what it is.
void pick(bool download) noexcept;
bool picked() noexcept;
void set_binds(std::uint32_t yes, std::uint32_t no) noexcept;
View view();
// Client thread, every tick: starts the live apply and the join when their turn comes.
void tick();
// The Thunderstore package an enabled mod's level comes from (its folder and manifest), for a
// host to tell its guests; empty when the level is the game's own or the mod is not a package.
std::string package_of(std::string_view asset);
}
