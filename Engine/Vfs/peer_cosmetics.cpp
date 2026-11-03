#include "peer_cosmetics.h"
#include "mod_list.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace dingosdk::mods::peer_cosmetics {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t maximum_store_bytes = 1024 * 1024;
constexpr std::int64_t day_seconds = 24 * 60 * 60;

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}
// A field of the right type, or the fallback: a hand-edited file must not take the rest with it.
std::string text_of(const Json& row, std::string_view key, std::size_t most) {
    if (!row.contains(key) || !row.at(key).is_string()) return {};
    return row.at(key).string().substr(0, most);
}
std::int64_t number_of(const Json& row, std::string_view key) {
    return row.contains(key) && row.at(key).is_number() ? row.at(key).get<std::int64_t>() : 0;
}
Policy clamped(Policy policy) {
    policy.days = std::clamp(policy.days, 1U, 365U);
    policy.max_mb = std::clamp<std::uint64_t>(policy.max_mb, 100, 65536);
    return policy;
}
std::string days_text(std::int64_t seconds) {
    const auto days = seconds / day_seconds;
    return days == 1 ? "1 day" : std::to_string(days) + " days";
}
} // namespace

std::string to_json(const Store& store) {
    const auto policy = clamped(store.policy);
    auto root = Json::object();
    root["schema"] = 1;
    auto rules = Json::object();
    rules["cleanup"] = std::string(cleanup_names[static_cast<std::size_t>(policy.cleanup)]);
    rules["days"] = policy.days;
    rules["max_mb"] = policy.max_mb;
    rules["automatic"] = policy.automatic;
    root["policy"] = std::move(rules);
    auto rows = Json::object();
    for (const auto& [folder, entry] : store.mods) {
        auto row = Json::object();
        row["version"] = entry.version;
        if (!entry.from.empty()) row["from"] = entry.from;
        row["fetched"] = entry.fetched;
        row["seen"] = entry.seen;
        if (entry.remove) row["remove"] = true;
        rows[folder] = std::move(row);
    }
    root["mods"] = std::move(rows);
    return root.dump(2) + "\n";
}

Store from_json(std::string_view text) noexcept {
    Store store;
    try {
        const auto root = Json::parse(text, {.bytes = maximum_store_bytes});
        if (!root.is_object()) return store;
        if (root.contains("policy") && root.at("policy").is_object()) {
            const auto& rules = root.at("policy");
            const auto cleanup = text_of(rules, "cleanup", 32);
            for (std::size_t i = 0; i < cleanup_names.size(); ++i)
                if (cleanup == cleanup_names[i]) store.policy.cleanup = static_cast<Cleanup>(i);
            if (const auto days = number_of(rules, "days"); days > 0) store.policy.days = static_cast<unsigned>(std::min<std::int64_t>(days, 365));
            if (const auto most = number_of(rules, "max_mb"); most > 0) store.policy.max_mb = static_cast<std::uint64_t>(most);
            if (rules.contains("automatic") && rules.at("automatic").is_boolean()) store.policy.automatic = rules.at("automatic").get<bool>();
            store.policy = clamped(store.policy);
        }
        if (root.contains("mods") && root.at("mods").is_object())
            for (const auto& [folder, row] : root.at("mods").items()) {
                if (!valid_mod_name(folder) || !row.is_object()) continue;
                Entry entry;
                entry.version = text_of(row, "version", 32);
                entry.from = text_of(row, "from", 64);
                entry.fetched = number_of(row, "fetched");
                entry.seen = std::max(number_of(row, "seen"), entry.fetched);
                entry.remove = row.contains("remove") && row.at("remove").is_boolean() && row.at("remove").get<bool>();
                store.mods.emplace(folder, std::move(entry));
            }
    } catch (...) {
        return {};
    }
    return store;
}

Store read(const fs::path& mods_root) noexcept {
    try {
        std::ifstream input(mods_root / store_file, std::ios::binary);
        if (!input) return {};
        std::string text;
        text.resize(maximum_store_bytes + 1);
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(input.gcount()));
        if (text.size() > maximum_store_bytes) return {};
        return from_json(text);
    } catch (...) {
        return {};
    }
}

bool write(const fs::path& mods_root, const Store& store) noexcept {
    try {
        const auto target = mods_root / store_file;
        const auto text = to_json(store);
        auto temporary = target;
        temporary += L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())) || !output.flush()) return false;
        }
        if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::error_code ignored;
        fs::remove(temporary, ignored);
    } catch (...) {
    }
    return false;
}

std::vector<Removal> expired(const Store& store, std::int64_t now, const std::map<std::string, std::uint64_t, std::less<>>& sizes) {
    const auto policy = clamped(store.policy);
    // Oldest seen first: the order the size cap takes them in, and the order the log reads best in.
    std::vector<std::pair<std::string, const Entry*>> order;
    for (const auto& [folder, entry] : store.mods) order.emplace_back(folder, &entry);
    std::ranges::stable_sort(order, [](const auto& a, const auto& b) { return a.second->seen < b.second->seen; });
    const auto size_of = [&](const std::string& folder) {
        const auto found = sizes.find(folder);
        return found == sizes.end() ? std::uint64_t{} : found->second;
    };
    std::uint64_t total{};
    for (const auto& [folder, entry] : order)
        if (!entry->remove) total += size_of(folder);
    const auto cap = policy.max_mb * 1024 * 1024;
    std::vector<Removal> out;
    for (const auto& [folder, entry] : order) {
        const auto unused = std::max<std::int64_t>(0, now - entry->seen);
        if (entry->remove) out.push_back({folder, "removed from the menu"});
        else if (policy.cleanup == Cleanup::every_launch) out.push_back({folder, "fetched cosmetics are deleted at every launch"});
        else if (policy.cleanup == Cleanup::unused && unused >= static_cast<std::int64_t>(policy.days) * day_seconds)
            out.push_back({folder, "not seen for " + days_text(unused)});
        else if (policy.cleanup == Cleanup::size && total > cap) {
            total -= size_of(folder);
            out.push_back({folder, "fetched cosmetics are over " + std::to_string(policy.max_mb) + " MB; least recently seen"});
        }
    }
    return out;
}

std::vector<std::string> clean_up(const fs::path& mods_root, std::int64_t now) noexcept {
    std::vector<std::string> lines;
    try {
        auto store = read(mods_root);
        if (store.mods.empty()) return lines;
        const auto before = store;
        std::map<std::string, std::uint64_t, std::less<>> sizes;
        std::error_code error;
        for (auto at = store.mods.begin(); at != store.mods.end();) {
            const auto folder = mods_root / fs::path(at->first);
            if (!fs::is_directory(folder, error)) {
                lines.push_back("Fetched cosmetics: " + at->first + " is gone from Mods, so it is forgotten.");
                at = store.mods.erase(at);
                continue;
            }
            sizes.emplace(at->first, folder_size(folder));
            ++at;
        }
        std::vector<std::string> removed;
        for (const auto& removal : expired(store, now, sizes)) {
            error.clear();
            fs::remove_all(mods_root / fs::path(removal.folder), error);
            if (error) {
                lines.push_back("Fetched cosmetics: " + removal.folder + " could not be deleted (" + error.message() + "); trying again next launch.");
                continue;
            }
            lines.push_back("Fetched cosmetics: deleted " + removal.folder + " (" + removal.why + ").");
            removed.push_back(lower(removal.folder));
            store.mods.erase(removal.folder);
        }
        if (!removed.empty()) {
            // Its row in mods.json goes too, the rest as they were.
            auto list = scan_mods(mods_root.parent_path());
            if (list.issue.empty()) {
                std::vector<ModEntry> kept;
                std::ranges::copy_if(list.entries, std::back_inserter(kept),
                                     [&](const ModEntry& entry) { return std::ranges::find(removed, lower(entry.mod.name)) == removed.end(); });
                try {
                    save_mod_order(mods_root, kept);
                } catch (const std::exception& failure) {
                    lines.push_back(std::string("Fetched cosmetics: mods.json could not be updated: ") + failure.what());
                }
            }
        }
        if (store != before && !write(mods_root, store)) lines.push_back("Fetched cosmetics: " + std::string(store_file) + " could not be written.");
    } catch (const std::exception& failure) {
        lines.push_back(std::string("Fetched cosmetics: the cleanup stopped: ") + failure.what());
    } catch (...) {
        lines.push_back("Fetched cosmetics: the cleanup stopped.");
    }
    return lines;
}

bool only_cosmetics(const fs::path& directory) noexcept {
    try {
        std::error_code error;
        bool items{};
        for (fs::recursive_directory_iterator at(directory, error), end; !error && at != end; at.increment(error)) {
            auto relative = ascii_path(fs::relative(at->path(), directory, error));
            if (relative.empty()) return false;
            relative = lower(relative);
            if (at->is_directory(error)) {
                if (relative == "plugin" || relative == "parks" || relative == "scripts") return false;
                continue;
            }
            if (relative == "reskate-plugin.json" || relative == "reskate-levels.json" || relative == "reskate-music.json") return false;
            const auto extension = lower(ascii_path(at->path().extension()));
            if (extension == ".lua" || extension == ".luau" || extension == ".dll" || extension == ".exe" || extension == ".bat" ||
                extension == ".ps1" || extension == ".cmd")
                return false;
            if (extension != ".toc") continue;
            if (relative == "win32/items.toc") items = true;
            else if (relative != "layout.toc" && relative != "win32/globals.toc") return false;
        }
        return !error && items;
    } catch (...) {
        return false;
    }
}

std::uint64_t folder_size(const fs::path& directory) noexcept {
    std::uint64_t total{};
    try {
        std::error_code error;
        for (fs::recursive_directory_iterator at(directory, error), end; !error && at != end; at.increment(error))
            if (at->is_regular_file(error)) {
                const auto size = at->file_size(error);
                if (!error) total += size;
                error.clear();
            }
    } catch (...) {
    }
    return total;
}

std::int64_t now_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace dingosdk::mods::peer_cosmetics
