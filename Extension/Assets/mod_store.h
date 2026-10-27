#pragma once
#include "mod_store_view.h"
#include "Extension/Assets/live_mods.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The in-game Mod Manager (the pause menu's Mod Manager tab, Extension/UI/Overlay/mod_manager_page.inl):
// Thunderstore's listing for ReSkate's community, the installed mods, and a queue that downloads and
// installs packages with what they need, updates them and uninstalls them, as the launcher's Mods
// panel does. Everything that reads the disk or the network runs on a thread of its own; the page
// reads snapshots, every frame if it likes.
//
// A change takes effect as any change to the Mods folder does in game: plugins within two seconds
// (the plugin host looks for them), everything else when the mods are applied (live_mods.h), which
// the page offers. The game holds the files of mods it has loaded open, so an update or uninstall
// of one of those cannot be made while it runs: it waits in Mods/.reskate-pending and the launcher
// makes it before the next start (Launcher/mod_manager.h apply_pending).
namespace dingosdk::mod_store {

// The listing, as it was last read: at once from the copy kept on disk
// (%LOCALAPPDATA%\ReSkate\thunderstore\listing-<community>.json), and from Thunderstore when that is
// over 10 minutes old.
struct Listing {
    std::vector<ts::Package> packages;
    std::uint64_t fetched{};     // when this copy came from Thunderstore (Unix seconds); 0: none yet
    bool fetching{};
    std::string error;           // why the last fetch failed; empty when it did not
    unsigned serial{};           // changes with every new copy
};
// Any thread. Starts a fetch when one is due.
std::shared_ptr<const Listing> listing();
// Fetches it again now (unless a fetch is under way).
void refresh();

// An installed mod as the page shows it, its state as the Mods page in the ReSkate menu tells it.
enum class State { loaded, disabled, apply_to_load, apply_to_unload, left_out, not_loaded, set_aside, update_waiting, removal_waiting };
const char* state_label(State state);
struct Mod {
    std::string folder, title, author, version, description;
    std::vector<std::string> contents;   // "Map", "Plugin"... (mod_list.h mod_contents)
    bool enabled{}, in_effect{};
    State state{};
    std::string why;                     // what the state means for this mod, when it needs saying
    bool gone{};                         // its folder was deleted, but it is in effect until applied
};
struct Mods {
    std::vector<Mod> mods;               // in load order, highest first
    ts::Installed versions;              // every installed folder and its version
    std::size_t changes{};               // what the Apply button would apply
    live_mods::ApplyEffect effect{};     // and what it does to the level being played
    std::size_t waiting{};               // updates and uninstalls waiting for the next start
    std::string issue;                   // mods.json could not be read
    bool present{};                      // the Mods folder exists
    // Read after the last apply had finished: only then do `changes` say what applying would still
    // change (a reading from before or during one still has the applied ones in it).
    bool settled{};
    unsigned serial{};
};
// Any thread. Read on a thread of its own: again every 1.5 s while asked for, and at once after a
// change made here. Null until the first read is done.
std::shared_ptr<const Mods> installed();

// Switches a mod on or off in mods.json (on a thread of its own). Takes effect when applied.
void set_enabled(const std::string& folder, bool on);
// Applies the mods as they are now (live_mods::apply), reloading the level when that is needed for
// the change to show. Returns what happened, for the page.
std::string apply();

// The queue: jobs (an install of some packages, an uninstall) run one at a time, in the order they
// were asked for; one can be asked for while another runs.
struct Progress {
    bool busy{};                 // a job is in hand, or waits its turn
    enum class Step { starting, downloading, installing, uninstalling } step{};
    std::string activity;      // "Downloading Vancouver Plaza (2 of 3)"
    float fraction{-1};        // of the one in hand; below 0 when not known
    // The package in hand (Namespace-Name; the folder, uninstalling), its name, the version going
    // in and its icon's URL; which of the job's packages it is, of how many (from 0); while it
    // downloads, how much has come of how much (0: not known); and the packages still to come.
    std::string package, title, version, icon;
    std::size_t at{}, of{};
    std::uint64_t received{}, total{};
    std::vector<std::string> queued;   // and every package of the jobs waiting their turn (an uninstall's folder)
    std::size_t waiting{};             // jobs waiting their turn after the one in hand
    std::string result;        // what the last job did, for the page; empty while busy
    bool failed{};             // the last job did not do all it was asked to
    unsigned serial{};         // changes when a job ends
};
Progress progress();
// Queues an install of the packages (Namespace-Name each, as in the listing) at their newest version,
// with the packages they need (worked out when the job starts). An installed one is updated. Empty,
// or why it was not queued (a package not listed, or already queued or in hand).
std::string install(const std::vector<std::string>& packages);
// Queues an uninstall of Mods/<folder>: to the Recycle Bin, or at the next start when the game holds
// its files.
std::string uninstall(const std::string& folder);
// Whether a package (Namespace-Name) is being installed now, or waits in the queue.
bool queued(std::string_view package);
// Stops the job in hand and empties the queue; what is installed stays.
void cancel();

// A package's README, fetched on a thread of its own the first time it is asked for; null until
// then (and `failed` when it could not be had).
struct Readme {
    ts::Readme text;
    bool failed{};
};
std::shared_ptr<const Readme> readme(const ts::Package& package);

// "YYYY-MM-DD" today (UTC), for the front page.
std::string today();
}
