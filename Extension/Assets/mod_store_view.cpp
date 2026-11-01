#include "mod_store_view.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <set>

namespace dingosdk::mod_store {
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return result;
}
bool installed_here(const ts::Package& package, const ts::Installed& installed) {
    return installed.contains(ts::folder_for(package.full_name));
}
// Listed at all: never NSFW, and deprecated only when it is installed.
bool listed(const ts::Package& package, const ts::Installed& installed) {
    return !nsfw(package) && (!package.deprecated || installed_here(package, installed));
}
// "YYYY-MM-DD..." as its year, month and day.
bool ymd(std::string_view text, int& year, int& month, int& day) {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') return false;
    const auto number = [&](std::size_t at, std::size_t length, int& out) {
        return std::from_chars(text.data() + at, text.data() + at + length, out).ec == std::errc{};
    };
    return number(0, 4, year) && number(5, 2, month) && number(8, 2, day);
}
bool before(const ts::Package& a, const ts::Package& b, Sort sort) {
    switch (sort) {
    case Sort::popular: if (a.downloads != b.downloads) return a.downloads > b.downloads; break;
    case Sort::updated: if (a.date_updated != b.date_updated) return a.date_updated > b.date_updated; break;
    case Sort::newest: if (a.date_created != b.date_created) return a.date_created > b.date_created; break;
    case Sort::rated: if (a.rating != b.rating) return a.rating > b.rating; break;
    case Sort::name: break;
    }
    return lower(a.title()) < lower(b.title());
}
// The date `days` before "YYYY-MM-DD", in the same form (months taken as 30 days: close enough for
// what counts as recent).
std::string days_before(std::string_view today, int days) {
    int year{}, month{}, day{};
    if (!ymd(today, year, month, day)) return {};
    int total = year * 360 + (month - 1) * 30 + (day - 1) - days;
    if (total < 0) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", total / 360, total % 360 / 30 + 1, total % 30 + 1);
    return text;
}
} // namespace

const char* sort_label(Sort sort) {
    static constexpr std::array<const char*, sort_count> labels{"MOST DOWNLOADED", "LAST UPDATED", "NEWEST", "TOP RATED", "NAME"};
    return labels[static_cast<std::size_t>(sort)];
}

bool nsfw(const ts::Package& package) {
    return package.nsfw || std::any_of(package.categories.begin(), package.categories.end(),
                                       [](const std::string& category) { return lower(category) == "nsfw"; });
}

std::vector<const ts::Package*> search(std::span<const ts::Package> packages, const Query& query, const ts::Installed& installed) {
    const auto wanted = lower(query.search);
    std::vector<const ts::Package*> result;
    for (const auto& package : packages) {
        if (!listed(package, installed)) continue;
        if (!ts::shown_by(package, ts::CategoryFilter{query.only, query.hidden})) continue;
        if (!wanted.empty() && lower(package.title()).find(wanted) == std::string::npos &&
            lower(package.owner).find(wanted) == std::string::npos &&
            lower(package.latest().description).find(wanted) == std::string::npos)
            continue;
        result.push_back(&package);
    }
    std::stable_sort(result.begin(), result.end(), [&](const ts::Package* a, const ts::Package* b) {
        if (a->pinned != b->pinned) return a->pinned;
        return before(*a, *b, query.sort);
    });
    return result;
}

std::vector<std::pair<std::string, int>> categories(std::span<const ts::Package> packages) {
    std::vector<std::pair<std::string, int>> result;
    for (const auto& package : packages) {
        if (nsfw(package)) continue;
        for (const auto& category : package.categories) {
            const auto found = std::find_if(result.begin(), result.end(), [&](const auto& entry) { return entry.first == category; });
            if (found == result.end()) result.emplace_back(category, 1);
            else ++found->second;
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    return result;
}

Front front(std::span<const ts::Package> packages, const ts::Installed& installed, std::string_view today,
            std::size_t featured_count, std::size_t per_shelf) {
    Front result;
    std::vector<const ts::Package*> shown;
    for (const auto& package : packages)
        if (listed(package, installed) && !package.deprecated) shown.push_back(&package);
    const auto sorted = [&](Sort sort, auto keep) {
        std::vector<const ts::Package*> list;
        for (const auto* package : shown)
            if (keep(*package)) list.push_back(package);
        std::stable_sort(list.begin(), list.end(), [&](const ts::Package* a, const ts::Package* b) { return before(*a, *b, sort); });
        if (list.size() > per_shelf) list.resize(per_shelf);
        return list;
    };
    const auto any = [](const ts::Package&) { return true; };

    // Featured: pinned first, then what is popular and still looked after.
    const auto recent = days_before(today, 60);
    std::set<const ts::Package*> taken;
    for (const auto* package : shown)
        if (package->pinned && result.featured.size() < featured_count) result.featured.push_back(package), taken.insert(package);
    for (const auto* package : sorted(Sort::popular, [&](const ts::Package& p) { return recent.empty() || p.date_updated >= recent; }))
        if (result.featured.size() < featured_count && taken.insert(package).second) result.featured.push_back(package);
    for (const auto* package : sorted(Sort::popular, any))
        if (result.featured.size() < featured_count && taken.insert(package).second) result.featured.push_back(package);

    const auto shelf = [&](std::string title, std::vector<const ts::Package*> list) {
        if (!list.empty()) result.shelves.push_back({std::move(title), std::move(list)});
    };
    shelf("MOST POPULAR", sorted(Sort::popular, any));
    shelf("RECENTLY UPDATED", sorted(Sort::updated, any));
    shelf("NEW", sorted(Sort::newest, any));
    // Then every category, its own row, the biggest first: not those that hold what players rarely
    // browse for (packs of other mods, what mods need, tools, AI-made), which are still a search away.
    constexpr std::array<std::string_view, 4> unshelved{"modpacks", "libraries", "ai generated", "tools"};
    for (const auto& [category, count] : categories(packages))
        if (std::find(unshelved.begin(), unshelved.end(), lower(category)) == unshelved.end())
            shelf([&] { std::string title = category; for (auto& c : title) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32); return title; }(),
              sorted(Sort::popular, [&](const ts::Package& p) { return p.in_category(category); }));
    return result;
}

Action action_for(const ts::Package& package, const ts::Installed& installed) {
    if (!installed_here(package, installed)) return Action::install;
    return ts::update_available(package, installed) ? Action::update : Action::installed;
}

Plan plan(std::span<const ts::Package> wanted, std::span<const ts::Package> listing, const ts::Installed& installed) {
    Plan result;
    for (const auto& package : wanted)
        for (auto& needed : ts::with_dependencies(package, listing, installed, &result.unknown))
            if (std::none_of(result.packages.begin(), result.packages.end(),
                             [&](const ts::Package& queued) { return queued.full_name == needed.full_name; })) {
                result.bytes += needed.latest().file_size;
                result.packages.push_back(std::move(needed));
            }
    std::sort(result.unknown.begin(), result.unknown.end());
    result.unknown.erase(std::unique(result.unknown.begin(), result.unknown.end()), result.unknown.end());
    return result;
}

std::string size_text(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1ull << 20));
    else std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return text;
}

std::string count_text(std::uint64_t count) {
    char text[32];
    if (count >= 1000000) std::snprintf(text, sizeof(text), "%.1fM", static_cast<double>(count) / 1e6);
    else if (count >= 1000) std::snprintf(text, sizeof(text), "%.1fK", static_cast<double>(count) / 1e3);
    else std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(count));
    return text;
}

std::string date_text(std::string_view iso) {
    static constexpr std::array<const char*, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int year{}, month{}, day{};
    if (!ymd(iso, year, month, day) || month < 1 || month > 12)
        return std::string(iso.substr(0, std::min<std::size_t>(iso.size(), 10)));
    return std::string(months[static_cast<std::size_t>(month - 1)]) + ' ' + std::to_string(day) + ", " + std::to_string(year);
}
}
