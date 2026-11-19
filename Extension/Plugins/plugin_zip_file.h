#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// A zip that is a file (reskate.zip.list, reskate.files.unzip): what the plugin API hands the game
// (plugin_game.h's zip_list and files_unzip), and the reader that lists and unpacks one
// (plugin_zip_file.cpp). A zip of hundreds of megabytes is never held in memory: its list is read
// from its directory at the end of the file, and unpacking streams each file to disk, off the
// plugin's thread. Nothing here knows what a zip holds.
namespace dingosdk::plugins::zip_file {
// What a zip may hold and unpack to, whatever it says of itself.
inline constexpr std::size_t most_files = 4096;
inline constexpr std::uint64_t most_file_bytes = 512ull << 20, most_bytes = 2048ull << 20;
// A plugin unpacks at most this many at once, and all plugins together this many.
inline constexpr std::size_t most_going = 2;
inline constexpr int most_going_in_all = 4;
// A name `only` gives is at most this long (no zip's name is longer), and a path in the data folder
// this long (plugin_api.h's data_path).
inline constexpr std::size_t most_name_bytes = 512, most_path_bytes = 200;

// A file of a zip, as its directory says: its name there ("/" between folders), its size unpacked
// and packed, and the CRC-32 of its bytes.
struct Entry {
    std::string name;
    std::uint64_t size{}, packed{};
    std::uint32_t crc{};
};
struct Request {
    std::string zip;    // the zip, in the plugin's data folder, as data_path checked it
    std::string folder; // where its files go: a folder there ("" the data folder itself, "<folder>:" a granted one's top)
    bool all{true};     // every file of it; otherwise the files `only` names, exactly as the zip names them
    std::vector<std::string> only;
    bool overwrite{};   // a file that is there already is replaced; otherwise it is an error and nothing is written
};
// A zip being unpacked. Whoever unpacks sets `why` (empty: every file was written) and `count` (how
// many), and then `done`; a later tick of the plugin calls back with them. `abandoned` is set by a
// plugin that stops meanwhile: the unpack stops at its next piece of a file and removes what it wrote.
// How many there are is counted, so all plugins together stay within most_going_in_all.
struct Job {
    Job() noexcept;
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    static int alive() noexcept;
    std::atomic<bool> done{}, abandoned{};
    std::string why;
    std::size_t count{};
};
// Where a request's paths are on disk, as the game placed them: the zip, the top of the folder its
// files go into (the data folder or a granted one: nothing is written outside it, whatever is linked
// into it), the folder itself, and how many more files that top may hold.
struct Place {
    std::filesystem::path zip, root, folder;
    std::size_t room{};
};

// ---- the reader (plugin_zip_file.cpp) ----------------------------------------------------------------
// Any thread; a call holds nothing of the caller's once it returns.

// The files of the zip at `file`, in its order, folders left out: read from its directory, so nothing
// is unpacked and a large zip lists as fast as a small one. Empty when read, else why not.
std::string list(const std::filesystem::path& file, std::vector<Entry>& entries);
// Unpacks the request's files into place.folder, each under its own name, the folders it names made.
// Before anything is written every file to unpack is checked: a name a data path may not have (an
// absolute one, "..", a drive, a backslash, a device's name), a name twice, a file past the caps, a
// file that is there already without `overwrite`. Each file is then streamed beside its place and
// checked against its CRC, and only when all are good are they moved into place: a damaged one stops
// the unpack and removes what this call wrote. `count` is how many files were written. Empty when
// done, else why not. `stop`, once true, ends it the same way.
std::string unpack(const Request& request, const Place& place, std::size_t& count, const std::atomic<bool>* stop = nullptr);
// The same on a thread of its own: `job` says how it went. Why it could not be started, or empty. Two
// unpacks never write into one folder, or into one inside the other's, at once.
std::string unpack_later(Request request, Place place, std::shared_ptr<Job> job);
}
