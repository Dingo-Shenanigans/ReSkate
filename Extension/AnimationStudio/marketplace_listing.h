#pragma once
// The MARKET's listing as Thunderstore gives it, read into packs: on its own (no downloads in it), to be tested.
#include "marketplace.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::anim_studio::market {
// Only thunderstore.io and its hosts are downloaded from (as the launcher does).
inline bool thunderstore_url(std::string_view url) {
    if (!url.starts_with("https://")) return false;
    auto host = url.substr(8);
    host = host.substr(0, host.find_first_of("/?#"));
    return host == "thunderstore.io" || host.ends_with(".thunderstore.io");
}
// One page of Thunderstore's search (what its website lists from, up to date within a minute): the packs on it whose
// description carries the studio's tag, and the next page's address ("" none). A pack whose icon does not say its
// version is left without one, to be asked for (fill_version).
inline std::vector<Pack> read_search(const std::string& text, std::string& next) {
    std::vector<Pack> out;
    next.clear();
    const auto root = Json::parse(text, JsonLimits{8 * 1024 * 1024, 64, 8 * 1024 * 1024});
    if (!root.is_object() || !root.contains("results") || !root.at("results").is_array()) throw std::runtime_error("it is not a listing");
    if (root.contains("next") && root.at("next").is_string()) next = root.at("next").string();
    for (const auto& r : root.at("results")) {
        try {
            if (!r.is_object()) continue;
            // A field missing or empty (null) reads as nothing, not as a pack that cannot be read.
            const auto str = [&](std::string_view key) { return r.contains(key) && r.at(key).is_string() ? r.at(key).string() : std::string{}; };
            const auto num = [&](std::string_view key) { return r.contains(key) && r.at(key).is_number() ? r.at(key).get<double>() : 0.0; };
            const auto yes = [&](std::string_view key) { return r.contains(key) && r.at(key).is_boolean() && r.at(key).get<bool>(); };
            if (yes("is_deprecated") || yes("is_nsfw")) continue;
            const auto description = str("description");
            if (!description.starts_with(description_tag)) continue; // found by the words, but not the studio's
            Pack p;
            p.owner = str("namespace");
            p.name = str("name");
            if (!plain_name(p.owner) || !plain_name(p.name)) continue;
            p.full_name = p.owner + "-" + p.name;
            p.title = p.name;
            std::replace(p.title.begin(), p.title.end(), '_', ' ');
            p.description = description.substr(std::string_view(description_tag).size());
            p.icon_url = str("icon_url");
            if (!thunderstore_url(p.icon_url)) p.icon_url.clear();
            p.version = version_from_icon(p.icon_url, p.full_name);
            if (!p.version.empty()) p.download_url = std::string(site) + "/package/download/" + p.owner + "/" + p.name + "/" + p.version + "/";
            p.package_url = std::string(site) + "/c/" + community + "/p/" + p.owner + "/" + p.name + "/";
            p.updated = str("last_updated").substr(0, 10);
            p.created = str("datetime_created").substr(0, 10);
            if (p.created.empty()) p.created = p.updated;
            p.downloads = static_cast<std::uint64_t>(std::max(0.0, num("download_count")));
            p.rating = static_cast<int>(std::max(0.0, num("rating_count")));
            out.push_back(std::move(p));
        } catch (...) {}
    }
    return out;
}
} // namespace dingosdk::anim_studio::market
