#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Whether Thunderstore lets a mod's plugin run. A mod folder named as a Thunderstore package
// (Owner-Name, as the launcher installs one, or Owner-Name-1.2.3 from a zip unpacked by hand) runs
// its plugin only while ReSkate's community on Thunderstore lists that package as a plugin (its
// "Plugins" category) and its moderators have not rejected it. Packages that came disguised as
// cosmetics shipped plugins that frightened players (full-screen scares, sound turned up), so
// Luau code only runs from where Thunderstore says it is there. A package that was on Thunderstore
// and has been taken down stops too. The mod's other content (cosmetics, maps) still loads.
//
// A folder that is not named as a package, or a package Thunderstore has never had (a plugin being
// made, before it is uploaded), is not Thunderstore's to say and runs as before, once the player
// has turned it on (plugin_approval.h).
//
// The answers are kept in a file and asked again every few hours. A package not looked up yet waits
// for its first answer; when Thunderstore cannot be reached (offline, or it is down) the check is
// skipped and the plugin runs, until an answer comes. The game asks thunderstore.io itself
// (plugin_runtime.cpp); this is the bookkeeping, which the tests drive with answers of their own.
namespace dingosdk::plugins {
// What thunderstore.io answered about one package.
struct ListingAnswer {
    enum class Kind { listed, missing, failed } kind{Kind::failed}; // missing: a 404, the package is not there
    std::string json;                                              // listed: /api/experimental/package/<owner>/<name>/
};
struct ListingVerdict {
    bool runs{};
    std::string why; // when it does not run, for the player
};

class Listings {
public:
    // Answers are kept in `file` (none: only for this run).
    explicit Listings(std::filesystem::path file = {});
    // The verdict for the mod folder `folder` as the answers stand at `now` (seconds since 1970).
    // A package it needs to know about is asked for (take_wanted).
    ListingVerdict verdict(std::string_view folder, std::int64_t now);
    // The packages ("Owner-Name") to look up now: never asked, or last asked long enough ago.
    std::vector<std::string> take_wanted(std::int64_t now);
    // An answer for `package`, as thunderstore.io gave it at `now`.
    void record(std::string_view package, const ListingAnswer& answer, std::int64_t now);

    static constexpr std::int64_t refresh_seconds = 6 * 60 * 60; // a known answer is asked again after this
    static constexpr std::int64_t retry_seconds = 60;            // a lookup that failed is tried again after this

private:
    struct Record {
        enum class State { plugin, not_plugin, rejected, missing } state{};
        std::string categories; // as Thunderstore lists them, for the player
        bool was_listed{};      // Thunderstore has had it: missing now means taken down
        std::int64_t checked{};
    };
    void save();
    std::mutex mutex_;
    std::filesystem::path file_;
    std::map<std::string, Record, std::less<>> records_;  // by "owner-name", lower case
    std::map<std::string, std::int64_t, std::less<>> asked_; // lower case: when it was last handed out to look up
    std::map<std::string, std::string, std::less<>> names_;  // lower case to the folder's own spelling
    std::set<std::string, std::less<>> unreachable_;         // lower case: no answer, and the last lookup failed
};
// The package a mod folder is named as ("Owner-Name"), or nothing.
std::optional<std::string> listing_package(std::string_view folder);
}
