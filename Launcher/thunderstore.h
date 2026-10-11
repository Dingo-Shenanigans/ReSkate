#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Thunderstore (thunderstore.io/c/reskate): the package listing the launcher's
// Mods browser shows. No network here; gui_mods_browse.cpp fetches with
// launcher_update::http_get. ReSkate packages have no dependencies.
//
// A ReSkate package is a mod folder zipped with Thunderstore's manifest.json,
// icon.png and README.md at the top, and installs as Mods/<Namespace-Name>.
namespace dingosdk::thunderstore {

inline constexpr char site[] = "https://thunderstore.io";
inline constexpr char default_community[] = "reskate";

// The community the browser lists: RESKATE_THUNDERSTORE_COMMUNITY when set
// (to try the browser against a community that has packages), else reskate.
std::string community();
// /c/<community>/api/v1/package-listing-index/: a gzipped JSON array of
// gzipped listing chunks. The plain listing below is the fallback.
std::wstring listing_index_url(std::string_view community);
std::wstring listing_url(std::string_view community);
// The community's page on the site.
std::wstring community_page(std::string_view community);

struct Version {
    std::string number;                     // Major.Minor.Patch
    std::string full_name;                  // Namespace-Name-1.2.3
    std::string description;
    std::string icon;                       // https URL of the 256x256 icon
    std::string download_url;
    std::string website_url;
    std::string date_created;
    std::uint64_t downloads{};
    std::uint64_t file_size{};
};

struct Package {
    std::string name;                       // letters, digits and '_'
    std::string owner;                      // the team, which is the package namespace
    std::string full_name;                  // Namespace-Name
    std::string package_url;
    std::string date_created, date_updated; // ISO 8601, so they sort as text
    std::vector<std::string> categories;
    int rating{};
    bool pinned{}, deprecated{}, nsfw{};
    std::uint64_t downloads{};              // every version together
    std::vector<Version> versions;          // newest first, never empty

    const Version& latest() const { return versions.front(); }
    // The name as the site shows it: underscores as spaces.
    std::string title() const;
    bool in_category(std::string_view category) const;
};

// A package's README, as its page on the site shows it:
// /api/experimental/package/<owner>/<name>/<version>/readme/, a JSON object with "markdown".
std::wstring readme_url(const Package& package);
// The markdown in that answer; empty when the package has none.
std::string parse_readme(std::string_view json);
// A README laid out for plain drawing: one entry a line, with markdown's own marks, images and
// HTML taken out. Links keep their text. At most `max_lines` lines; `cut` says more were left out.
struct ReadmeLine {
    enum class Kind { text, heading, bullet, code, rule, gap } kind{};
    std::string text;
    bool operator==(const ReadmeLine&) const = default;
};
struct Readme {
    std::vector<ReadmeLine> lines;
    bool cut{};
};
Readme readme_lines(std::string_view markdown, std::size_t max_lines = 400);

// One listing chunk (or the whole plain listing): a JSON array of packages.
// Rows that are malformed or have no active version are skipped.
std::vector<Package> parse_listing(std::string_view json);
// The listing index: a JSON array of HTTPS chunk URLs.
std::vector<std::wstring> parse_index(std::string_view json);
// Unpacks a gzip stream (the index and its chunks), refusing output over
// `limit` bytes. Input that is not gzip comes back unchanged.
std::string gunzip(std::string_view data, std::size_t limit);

// Below zero, zero or above zero as `a` is older, the same or newer than `b`.
// Components compare as numbers ("1.10.0" is newer than "1.9.0").
int compare_versions(std::string_view a, std::string_view b);

// The Mods folder a package installs into: Namespace-Name, so a manual
// install of a downloaded zip (its version suffix dropped) lands in the same place.
std::string folder_for(std::string_view full_name);

// Installed mod folders and the version their manifest.json gives.
using Installed = std::map<std::string, std::string, std::less<>>;

// True when `package`'s latest version is newer than the installed copy.
bool update_available(const Package& package, const Installed& installed);

} // namespace dingosdk::thunderstore
