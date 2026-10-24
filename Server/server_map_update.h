#pragma once
#include <chrono>
#include <filesystem>
#include <future>
#include <set>
#include <string>
#include <vector>

// Keeping the server's custom maps at their newest Thunderstore version ("maps.latest_versions").
// The server reads two files of a map mod: reskate-levels.json (its levels) and manifest.json
// (its version, which players are held to: protocol.h: map_package_name). So nothing is
// downloaded: a map mod in Mods whose folder is a Thunderstore package (Owner-Name) is asked
// about, and when Thunderstore's newest version is not the one in its manifest.json, that
// version is written there. Players then fetch the newest as they join.
//
// The level list is left as it is. A new version that adds, removes or renames levels needs its
// reskate-levels.json copied in by the server's owner, as when the map was first set up.
namespace dingosdk::server {
struct MapUpdate {
    std::string folder, from, to; // the mod folder, its version before and Thunderstore's newest
    bool updated{};               // the newest version is in its manifest.json
    bool waiting{};               // left for later: its map is being played
    std::string problem;          // why it could not be looked up or written, or empty
};
// Looks every map mod in `mods` up and writes the newest version where it differs. `in_play`: a
// mod folder to leave as it is this time. Asks Thunderstore, so it can take a few seconds a
// map; made for a worker thread.
std::vector<MapUpdate> update_maps(const std::filesystem::path &mods, const std::string &in_play);
// The version a mod folder's manifest.json gives (nothing when it has none), and writing one
// there: everything else in the file is kept, and a folder without one gets a file with the
// version alone. Throws when the file cannot be written.
std::string map_version(const std::filesystem::path &mod);
void set_map_version(const std::filesystem::path &mod, const std::string &version);

// Whether a required plugin ("plugins.required") is one players can be asked for: listed as a
// plugin in ReSkate's Thunderstore community, not deprecated, not rejected. Empty when it is,
// else why not. `reached` is false when Thunderstore could not be asked (it is then kept: the
// players' games check it again themselves). Asks Thunderstore; for a worker thread.
std::string required_plugin_problem(const std::string &package, bool &reached);

// The run loop's side of that: the list checked once it changes (and at start), on a worker,
// and the packages to leave out of what players are told.
class RequiredPluginCheck {
  public:
    struct Done {
        std::vector<std::string> lines;   // for the log
        std::set<std::string> refused;    // not plugins on Thunderstore: not asked of players
        bool finished{};
    };
    Done tick(const std::vector<std::string> &required);

  private:
    std::future<Done> work_;
    std::vector<std::string> checked_;
    bool started_{};
};

// The run loop's side: a check when one is due or asked for, on a worker, and what it found.
class MapUpdater {
  public:
    struct Done {
        std::vector<std::string> lines; // for the log
        bool changed{};                 // a map's version changed: read Mods again
    };
    // Every turn of the loop. `enabled` and `minutes`: the config's; `asked`: "maps update".
    Done tick(bool enabled, unsigned minutes, const std::filesystem::path &mods, const std::string &in_play, bool asked);

  private:
    std::future<std::vector<MapUpdate>> work_;
    std::chrono::steady_clock::time_point next_{};
    bool asked_{};
    std::set<std::string> said_; // problems said once, not at every check
};
} // namespace dingosdk::server
