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
// end it). For now the console starts a fetch (`outfits fetch`); the session will, once players
// say which package each item they wear is from.
//
// The menu keeps them: when they are deleted (the policy, applied at launch), keeping one (it is
// the player's own from then on, as if installed from the launcher), or deleting one at the next
// launch. Wearing one of its items, or seeing anyone wear one, counts as using it.
namespace dingosdk::peer_cosmetics_fetch {
// Starts fetching `package` ("Owner-Name" or "Owner-Name-1.2.3") for the player named `from`.
// Returns what happened to the request; the outcome shows in status() and the log.
std::string fetch(const std::string& package, const std::string& from);
struct Status {
    bool busy{};
    std::string package;     // the one in hand, or the last
    std::uint64_t received{}, total{};
    std::string line;        // what it is doing, or how the last one ended
};
Status status();

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

// Client thread, every tick: the live merge a finished fetch asked for, and marking the fetched
// mods anyone wore since the last pass as seen (looked at once a minute; a mod's date is written
// again once it is an hour old). Nothing of the skater is read while a level loads.
void tick(bool loading);
} // namespace dingosdk::peer_cosmetics_fetch
