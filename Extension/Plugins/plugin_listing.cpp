#include "plugin_listing.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <fstream>

namespace dingosdk::plugins {
namespace {
constexpr std::string_view community = "reskate";

bool word(std::string_view text) {
    return !text.empty() && text.size() <= 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
bool version(std::string_view text) {
    int numbers = 0;
    for (std::size_t at = 0; at <= text.size();) {
        const auto end = std::min(text.find('.', at), text.size());
        const auto number = text.substr(at, end - at);
        if (number.empty() || number.size() > 9 || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        ++numbers;
        at = end + 1;
    }
    return numbers == 3;
}
std::string lowered(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}
std::string text_of(const Json& object, std::string_view key) {
    return object.is_object() && object.contains(key) && object.at(key).is_string() ? object.at(key).string() : std::string();
}
const char* state_name(int state) {
    static constexpr const char* names[] = {"plugin", "not_plugin", "rejected", "missing"};
    return state >= 0 && state < 4 ? names[state] : "missing";
}
}

std::optional<std::string> listing_package(std::string_view folder) {
    const auto first = folder.find('-');
    if (first == std::string_view::npos) return std::nullopt;
    const auto rest = folder.substr(first + 1);
    const auto second = rest.find('-');
    const auto owner = folder.substr(0, first), name = rest.substr(0, second);
    if (!word(owner) || !word(name) || (second != std::string_view::npos && !version(rest.substr(second + 1)))) return std::nullopt;
    return std::string(owner) + '-' + std::string(name);
}

Listings::Listings(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    try {
        std::ifstream in(file_, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.empty()) return;
        const auto root = Json::parse(text, {1 << 20, 4, 65536});
        for (const auto& [package, row] : root.items()) {
            if (!row.is_object() || !listing_package(package)) continue;
            Record record;
            const auto state = text_of(row, "state");
            for (int i = 0; i < 4; ++i)
                if (state == state_name(i)) record.state = static_cast<Record::State>(i);
            record.categories = text_of(row, "categories");
            record.was_listed = row.contains("was_listed") && row.at("was_listed").is_boolean() && row.at("was_listed").get<bool>();
            record.checked = row.contains("checked") && row.at("checked").is_number() ? row.at("checked").get<std::int64_t>() : 0;
            records_[lowered(package)] = record;
        }
    } catch (const std::exception&) {} // none yet, or unreadable: every package is looked up again
}

ListingVerdict Listings::verdict(std::string_view folder, std::int64_t now) {
    const auto package = listing_package(folder);
    if (!package) return {true, {}};
    std::lock_guard lock(mutex_);
    const auto key = lowered(*package);
    names_.try_emplace(key, *package);
    const auto found = records_.find(key);
    if (found == records_.end()) {
        if (!asked_.contains(key)) asked_[key] = 0; // (looked up at the next take_wanted)
        // Offline, or Thunderstore is down: the check is skipped until it answers.
        if (unreachable_.contains(key)) return {true, {}};
        return {false, "waiting to look " + *package + " up on Thunderstore: a mod's plugin runs only when Thunderstore lists it as a plugin"};
    }
    (void)now;
    const auto& record = found->second;
    switch (record.state) {
    case Record::State::plugin: return {true, {}};
    case Record::State::not_plugin:
        return {false, "Thunderstore does not list " + *package + " as a plugin" +
                           (record.categories.empty() ? std::string() : " (it is in " + record.categories + ")") +
                           ", so its plugin does not run. The rest of the mod still loads."};
    case Record::State::rejected:
        return {false, "Thunderstore's moderators rejected " + *package + ", so its plugin does not run."};
    case Record::State::missing:
        if (record.was_listed) return {false, *package + " was taken off Thunderstore, so its plugin does not run."};
        return {true, {}}; // never on Thunderstore: a plugin of the player's own
    }
    return {true, {}};
}

std::vector<std::string> Listings::take_wanted(std::int64_t now) {
    std::lock_guard lock(mutex_);
    std::vector<std::string> wanted;
    for (auto& [key, asked] : asked_) {
        const auto found = records_.find(key);
        const bool due = found == records_.end() ? asked == 0 || now - asked >= retry_seconds
                                                 : now - std::max(found->second.checked, asked) >= refresh_seconds;
        if (!due) continue;
        asked = now;
        const auto name = names_.find(key);
        wanted.push_back(name != names_.end() ? name->second : key);
    }
    return wanted;
}

void Listings::record(std::string_view package, const ListingAnswer& answer, std::int64_t now) {
    if (!listing_package(package)) return;
    Record record;
    record.checked = now;
    std::lock_guard lock(mutex_);
    const auto key = lowered(package);
    // A failure keeps what was known; with nothing known, the plugin runs without the check (verdict).
    if (answer.kind == ListingAnswer::Kind::failed) {
        if (!records_.contains(key)) unreachable_.insert(key);
        return;
    }
    if (const auto known = records_.find(key); known != records_.end()) record.was_listed = known->second.was_listed;
    if (answer.kind == ListingAnswer::Kind::missing) {
        record.state = Record::State::missing;
    } else {
        record.state = Record::State::not_plugin;
        try {
            const auto root = Json::parse(answer.json, {1 << 20, 16, 65536});
            const auto split = package.find('-');
            const auto owner = lowered(package.substr(0, split)), name = lowered(package.substr(split + 1));
            if (!root.is_object() || lowered(text_of(root, "namespace")) != owner || lowered(text_of(root, "name")) != name) return; // not its answer
            record.was_listed = true;
            bool listed{}, plugin{}, rejected{};
            if (root.contains("community_listings") && root.at("community_listings").is_array())
                for (const auto& listing : root.at("community_listings")) {
                    if (lowered(text_of(listing, "community")) != community) continue;
                    listed = true;
                    rejected = rejected || lowered(text_of(listing, "review_status")) == "rejected";
                    if (listing.contains("categories") && listing.at("categories").is_array())
                        for (const auto& category : listing.at("categories")) {
                            if (!category.is_string()) continue;
                            plugin = plugin || lowered(category.string()) == "plugins";
                            record.categories += (record.categories.empty() ? "" : ", ") + category.string();
                        }
                }
            if (!listed) record.categories = "not listed for ReSkate";
            record.state = rejected ? Record::State::rejected : plugin && listed ? Record::State::plugin : Record::State::not_plugin;
        } catch (const std::exception&) {
            return; // an answer that cannot be read keeps what was known
        }
    }
    records_[key] = record;
    unreachable_.erase(key);
    save();
}

void Listings::save() {
    if (file_.empty()) return;
    try {
        auto root = Json::object();
        for (const auto& [key, record] : records_) {
            auto& row = root[key];
            row["state"] = state_name(static_cast<int>(record.state));
            row["categories"] = record.categories;
            row["was_listed"] = record.was_listed;
            row["checked"] = record.checked;
        }
        std::filesystem::create_directories(file_.parent_path());
        auto temporary = std::filesystem::path(file_) += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const auto text = root.dump(2);
            if (!out.write(text.data(), static_cast<std::streamsize>(text.size()))) return;
        }
        std::filesystem::rename(temporary, file_);
    } catch (const std::exception&) {} // kept for this run
}
}
