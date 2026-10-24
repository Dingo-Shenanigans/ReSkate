#pragma once
#include "Extension/Multiplayer/Session/session.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What a multiplayer session needs and this PC does not have, fetched from Thunderstore while
// the game runs: the map, when the host says which package its map is from (session.h:
// MapNeed), and the plugins a dedicated server requires. The player is asked once, on one card,
// for all of it; on yes each package is downloaded and installed into Mods, and those with game
// data are applied as a live mod (live_mods.h). The session stays connected meanwhile and goes
// on once everything is there; on no, or when anything cannot be had, the session is left.
// The session waits 30 minutes on its local monotonic clock for the whole of it (checking,
// confirmation, download, installation, applying and joining); confirmation gets two of them.
// A download that has begun is not cut short when the session stops waiting: it finishes,
// installs and is applied, and the player is told to join again. A cancelled worker drains
// before another attempt can begin.
//
// Only maps and plugins come in this way, whatever a host says. A map has to be listed as a map
// in ReSkate's Thunderstore community and install as a map and nothing else (level bundles and
// their registration: no cosmetics, no settings, no scripts). A plugin has to be listed as a
// plugin there and install as one (a reskate-plugin.json that names Luau code to run); it may
// bring the game data it uses, which the live merge checks as it checks any mod's. A package
// that is deprecated or was rejected by the community's moderators is not fetched, and one that
// installs as anything else is taken out again. Everything is downloaded from thunderstore.io.
namespace dingosdk::map_download {
enum class Stage {
    idle,
    checking,    // asking Thunderstore about the packages; nothing is shown yet
    asking,      // the card asks: download, or not
    downloading,
    installing,
    applying,    // the live merge
    joining,     // everything is in the game: the session is going on (or is joined again)
};
struct View {
    Stage stage{};
    std::string map, package;   // "Vancouver Plaza", "Sandos-Vancouver_Plaza 2.0.0"; empty when only plugins are needed
    std::string author, version, description; // "Sandos", "2.0.0", and what its Thunderstore page says of it
    // applying: the live merge's steps done of all, and the one in hand.
    std::size_t step{}, steps{};
    std::string step_name;
    bool server{};              // a dedicated server's map, else a player's lobby
    bool moved{};               // the session changed to it with the player in it (else: joining it)
    bool installed{};           // asking: the mod is here but switched off, so nothing is downloaded
    std::string have;           // the version installed here, when the host's is another and replaces it
    bool choice{true};          // asking: the answer a controller has picked (true: download)
    std::uint64_t bytes{};               // asking: what all of it downloads (Content-Length); 0 when no server said
    std::uint64_t received{}, total{};   // downloading the one in hand; total 0 when the size is not known
    std::uint32_t yes_bind{}, no_bind{}; // the vote binds, which answer the card
    // Everything asked for, the map first when it is needed: what the card lists.
    struct Item {
        std::string name;        // the map's name, or the plugin's ("FPV Drone")
        std::string author, version, description;
        std::string have;        // the version installed here, when it is replaced
        bool plugin{}, installed{};
        std::uint64_t bytes{};   // 0 when not known, or nothing is downloaded
    };
    std::vector<Item> items;
    std::size_t current{};       // downloading, installing: the one in hand, an index into items
};
// Client thread: a session is waiting on what it needs. Ignored while another is in hand.
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
// Whether an enabled mod here has the level, from a package or not (session.h: LevelCheck).
bool has_level(std::string_view asset);
// Which of a dedicated server's required plugins are not ready here: not installed, switched
// off, not a plugin, or older than the version named (session.h: PluginCheck). A plugin counts
// as the package when its folder is the package's (Owner-Name), or its manifest's name and
// author are the package's.
std::vector<std::string> missing_plugins(const std::vector<std::string>& required);
// Reads the table package_of answers from, on a thread of its own, if it has not been (so the
// first map offer does not wait on the Mods folder). Cheap to call each frame; tick() does.
void warm_packages() noexcept;
}
