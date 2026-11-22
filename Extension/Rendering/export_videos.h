#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The videos the game exports from its replay editor (MP4 or WebM, in its own Videos folder under the local app data
// ReSkate gives it), listed for plugins and moved for them into a folder the player picks with Windows' folder picker,
// which ReSkate keeps for each plugin (plugin_video_folders.json beside the plugin approvals). A plugin names a video
// by its file's name alone: it never has a path, and never reads or writes a file of the player's. (reskate.videos)
namespace dingosdk::export_videos {
struct Video {
    std::string name; // UTF-8
    std::uint64_t bytes{};
    double written{}; // when it was last written: seconds since 1970
    bool ready{};     // the game has let it go (it is not being written)
};
// The game's videos, newest first (looked at again at most twice a second).
std::vector<Video> list();
// The folder the player picked for the plugin's videos ("" none), as text to show.
std::string folder(std::string_view plugin);
// Windows' folder picker over the game, on a thread of its own: the folder picked becomes the plugin's. "" when it
// opened, else why not.
std::string pick_folder(std::string_view plugin, std::string_view title);
void forget_folder(std::string_view plugin);
// File Explorer on the plugin's folder (the game's Videos folder while it has none).
std::string open_folder(std::string_view plugin);
// One of the game's videos moved into the plugin's folder (a name in it not taken yet), on a thread of its own: to
// another drive it is copied and the game's file deleted. "" when it started, else why not.
std::string move(std::string_view plugin, std::string_view name);
struct Moved {
    std::string name, to, why; // `to`: its name in the folder; `why`: "" moved, else why it stays where the game put it
};
// How the plugin's moves have ended since it last asked.
std::vector<Moved> moved(std::string_view plugin);
// The plugin stopped: what its moves came to is let go (its folder is kept for when it is back).
void forget(std::string_view plugin);
}
