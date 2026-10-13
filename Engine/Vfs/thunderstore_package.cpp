#include "thunderstore_package.h"

#include "Engine/Core/Json/json.h"

#include <algorithm>

namespace dingosdk::thunderstore_package {
namespace {

constexpr char site[] = "https://thunderstore.io";

bool word(std::string_view text) {
    return !text.empty() && text.size() <= 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
bool version(std::string_view text) {
    unsigned numbers{};
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
bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        return lower(x) == lower(y);
    });
}
std::wstring wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }   // ASCII only here
std::string text_of(const Json& object, std::string_view key) {
    return object.is_object() && object.contains(key) && object.at(key).is_string() ? object.at(key).string() : std::string();
}
bool flag(const Json& object, std::string_view key) {
    return object.is_object() && object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>();
}

} // namespace

std::optional<Name> parse_name(std::string_view package) {
    const auto first = package.find('-');
    if (first == std::string_view::npos) return std::nullopt;
    const auto rest = package.substr(first + 1);
    const auto second = rest.find('-');
    Name name{std::string(package.substr(0, first)), std::string(rest.substr(0, second)),
              second == std::string_view::npos ? std::string() : std::string(rest.substr(second + 1))};
    if (!word(name.owner) || !word(name.name) || (!name.version.empty() && !version(name.version))) return std::nullopt;
    return name;
}

std::wstring details_url(const Name& name) {
    return wide(std::string(site) + "/api/experimental/package/" + name.owner + '/' + name.name + '/');
}

std::wstring download_url(const Name& name, std::string_view wanted) {
    return wide(std::string(site) + "/package/download/" + name.owner + '/' + name.name + '/' + std::string(wanted) + '/');
}

Choice choose(std::string_view json, const Name& wanted, std::string_view community) {
    Choice choice;
    try {
        const auto root = Json::parse(json);
        if (!root.is_object() || !same(text_of(root, "namespace"), wanted.owner) || !same(text_of(root, "name"), wanted.name)) {
            choice.reason = "Thunderstore does not have that package.";
            return choice;
        }
        if (flag(root, "is_deprecated")) {
            choice.reason = "Its package on Thunderstore is marked deprecated.";
            return choice;
        }
        bool listed{}, map{}, rejected{}, nsfw{};
        if (root.contains("community_listings") && root.at("community_listings").is_array())
            for (const auto& listing : root.at("community_listings")) {
                if (!same(text_of(listing, "community"), community)) continue;
                listed = true;
                rejected = same(text_of(listing, "review_status"), "rejected");
                nsfw = flag(listing, "has_nsfw_content");
                if (listing.contains("categories") && listing.at("categories").is_array())
                    for (const auto& category : listing.at("categories"))
                    {
                        map = map || (category.is_string() && same(category.string(), "Maps"));
                        nsfw = nsfw || (category.is_string() && same(category.string(), "NSFW"));
                    }
            }
        if (!listed) { choice.reason = "Its package is not listed for ReSkate on Thunderstore."; return choice; }
        if (rejected) { choice.reason = "Its package was rejected by Thunderstore's moderators."; return choice; }
        if (nsfw) { choice.reason = "Its package is marked NSFW on Thunderstore."; return choice; }
        if (!map) { choice.reason = "Its package is not listed as a map on Thunderstore."; return choice; }
        const auto newest = root.contains("latest") ? text_of(root.at("latest"), "version_number") : std::string();
        if (!version(newest)) { choice.reason = "Thunderstore gave no version of that package."; return choice; }
        choice.newest = newest;
        // Its own words about itself, kept to printable text of a card's length.
        for (const unsigned char c : text_of(root.at("latest"), "description")) {
            if (choice.description.size() >= 240) { choice.description += "..."; break; }
            choice.description += c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
        }
        {
            // https://<something>.thunderstore.io/...png, and nothing that is not a plain URL.
            const auto icon = text_of(root.at("latest"), "icon");
            constexpr std::string_view scheme = "https://";
            const auto path = icon.find('/', scheme.size());
            const auto host = icon.starts_with(scheme) && path != std::string::npos ? std::string_view(icon).substr(scheme.size(), path - scheme.size())
                                                                                     : std::string_view{};
            const bool plain = icon.size() < 400 && std::all_of(icon.begin(), icon.end(), [](unsigned char c) {
                return c > 0x20 && c < 0x7f && c != '"' && c != '<' && c != '>' && c != '\\' && c != '#' && c != '?' && c != '@';
            });
            if (plain && (host == "thunderstore.io" || host.ends_with(".thunderstore.io"))) choice.icon = icon;
        }
        choice.version = wanted.version.empty() ? newest : wanted.version;
        choice.ok = true;
    } catch (const std::exception&) {
        choice = {};
        choice.reason = "Thunderstore's answer could not be read.";
    }
    return choice;
}

} // namespace dingosdk::thunderstore_package
