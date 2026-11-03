#pragma once
#include "Engine/Vfs/peer_cosmetics.h"
#include <cstdint>
#include <string>
#include <vector>

// Cosmetics fetched because another player wears them (Engine/Vfs/peer_cosmetics.h), while the
// game runs: a package is looked up on Thunderstore (listed as cosmetics or skateboards, not
// deprecated, NSFW or rejected), downloaded from thunderstore.io, installed into Mods at the bottom
// of the load order, and taken out again unless it is cosmetics and nothing else. Its items are in
// the game from the next level load (a live merge without a reload: reloading in a session would
// end it). Players say which package each item they wear is from (Appearance::sources): the
// menu fetches one when asked, and the tick on its own when the policy says to. A package is
// fetched for an item; once merged, the item has to be from it, or it is deleted at the next
// launch and not fetched again.
//
// The menu keeps them: when they are deleted (the policy, applied at launch), keeping one (it is
// the player's own from then on, as if installed from the launcher), or deleting one at the next
// launch. Wearing one of its items, or seeing anyone wear one, counts as using it.
namespace dingosdk::peer_cosmetics_fetch {
// Starts fetching `package` ("Owner-Name" or "Owner-Name-1.2.3") for the player named `from`,
// who wears the item `key` from it (empty: none to check, from the console). One bigger than
// `most_bytes` (0: the largest fetched for anyone, 1 GB) is not downloaded. Returns what
// happened to the request; the outcome shows in status() and the log.
std::string fetch(const std::string& package, const std::string& from, const std::string& key = {}, std::uint64_t most_bytes = 0);
struct Status {
    bool busy{};
    std::string package;     // the one in hand, or the last
    std::uint64_t received{}, total{};
    std::string line;        // what it is doing, or how the last one ended
};
Status status();

// How big a package's download is, for its Fetch button: looked up on Thunderstore in the
// background the first time it is asked for (one at a time), then remembered for the game.
struct Size {
    bool known{};
    std::uint64_t bytes{};   // 0 when Thunderstore does not say
    std::string problem;     // why it would not be fetched (not cosmetics, not there), if it would not
};
Size size(const std::string& package);

struct Fetched {
    std::string folder, title, version, from;
    std::int64_t fetched{}, seen{};
    std::uint64_t bytes{};
    bool remove{};
};
struct View {
    mods::peer_cosmetics::Policy policy;
    std::vector<Fetched> mods; // most recently seen first
    std::uint64_t bytes{};
};
// The store as the menu shows it: read again at most every two seconds, sizes every 30 s.
View view();
bool set_policy(const mods::peer_cosmetics::Policy& policy);
// Forgets a fetched mod: it is the player's own from now on. False when it is not a fetched one.
bool keep(const std::string& folder);
// Deletes a fetched mod at the next launch (or not, with false).
bool remove_at_next_launch(const std::string& folder, bool remove);

// Client thread, every tick: the live merge a finished fetch asked for, checking that the item
// a package was fetched for is from it once merged, the next package other players wear to
// fetch on its own (policy.automatic: one at a time, each at most once a game, at most
// automatic_limit a game), and marking the fetched mods anyone wore since the last pass as seen (looked at once a minute; a mod's date is written
// again once it is an hour old). Nothing of the skater is read while a level loads.
void tick(bool loading);
inline constexpr std::size_t automatic_limit = 16;
} // namespace dingosdk::peer_cosmetics_fetch
