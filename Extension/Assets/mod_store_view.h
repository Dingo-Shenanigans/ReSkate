#pragma once
#include "Launcher/thunderstore.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the in-game Mod Manager (mod_store.h) shows of Thunderstore's listing, worked out from the
// listing alone: no game, disk or network here, so it is tested on its own.
namespace dingosdk::mod_store {
namespace ts = thunderstore;

enum class Sort { popular, updated, newest, rated, name };
inline constexpr std::size_t sort_count = 5;
const char* sort_label(Sort sort);

// Not safe for work: Thunderstore's own mark on the package, or a category of that name. Never
// listed in game, installed or not (as in the launcher).
bool nsfw(const ts::Package& package);

struct Query {
    std::string search;    // in the title, owner and description, any case
    // The categories shown only (a package needs one of them, when there are any) and those hidden (a
    // package in any of them is left out), as the launcher's GET MODS has them (thunderstore.h).
    std::vector<std::string> only, hidden;
    Sort sort{Sort::popular};
};
// The packages that match, pinned first, then in `sort` order. A deprecated package only shows once
// installed.
std::vector<const ts::Package*> search(std::span<const ts::Package> packages, const Query& query, const ts::Installed& installed);

// The categories the listing uses (its NSFW packages left out), with how many packages are in each,
// most first.
std::vector<std::pair<std::string, int>> categories(std::span<const ts::Package> packages);

// BROWSE's front page: a few packages to show big, then rows of them.
struct Shelf {
    std::string title;
    std::vector<const ts::Package*> packages;
};
struct Front {
    std::vector<const ts::Package*> featured;
    std::vector<Shelf> shelves;
};
// `today`: "YYYY-MM-DD", for what counts as recent. Featured: the pinned packages, then the most
// downloaded of those updated in the last 60 days, at most `featured_count`. The shelves: most
// popular, recently updated and new, then one for every category, the biggest first, but mod packs,
// libraries, tools and AI generated. Each holds at most `per_shelf`.
Front front(std::span<const ts::Package> packages, const ts::Installed& installed, std::string_view today,
            std::size_t featured_count = 8, std::size_t per_shelf = 24);

// What pressing a package's button does.
enum class Action { install, update, installed };
Action action_for(const ts::Package& package, const ts::Installed& installed);

// What installing `wanted` brings, needed packages first, each once: the packages to download, and
// the names of those the listing does not have.
struct Plan {
    std::vector<ts::Package> packages;
    std::vector<std::string> unknown;
    std::uint64_t bytes{};   // what they download together (0 for those whose size is not listed)
};
Plan plan(std::span<const ts::Package> wanted, std::span<const ts::Package> listing, const ts::Installed& installed);

// "1.2 MB", "340 KB"; "4.1K", "12M" downloads.
std::string size_text(std::uint64_t bytes);
std::string count_text(std::uint64_t count);
// "2026-10-24T12:00:00Z" as "Oct 24, 2026".
std::string date_text(std::string_view iso);
}
