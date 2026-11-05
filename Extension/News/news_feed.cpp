#include "Extension/Profile/profile_internal.h"
#include <cmath>
#include <set>
#include <type_traits>

namespace dingosdk::profile {
using namespace detail;
NewsFeed news_feed(const Snapshot& s) {
    if (!s.extensions.contains("news")) return {};
    return parse_news(object_field(s.extensions, "news"));
}
std::string iso_date(std::string_view text) {
    if (text.size() < 10 || (text.size() > 10 && text[10] != 'T' && text[10] != ' ')) return {};
    int parts[3]{};
    for (std::size_t i = 0; i < 10; ++i) {
        const char c = text[i];
        if (i == 4 || i == 7) {
            if (c != '-') return {};
            continue;
        }
        if (c < '0' || c > '9') return {};
        auto &part = parts[i < 4 ? 0 : i < 7 ? 1 : 2];
        part = part * 10 + (c - '0');
    }
    const auto [year, month, day] = parts;
    constexpr int days[]{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > days[month - 1]) return {};
    if (month == 2 && day == 29 && (year % 4 || (year % 100 == 0 && year % 400))) return {};
    return std::string(text.substr(0, 10));
}
NewsFeed parse_news(const Json& section) {
    NewsFeed feed;
    require(section.is_object() && section.contains("enabled") && section.contains("posts"), "News must have enabled and posts");
    require(section.at("enabled").is_boolean(), "News enabled must be boolean");
    feed.enabled = section.at("enabled").get<bool>();
    const auto& posts = section.at("posts");
    require(posts.is_array() && posts.size() <= 32, "News posts must be an array of at most 32 entries");
    std::set<std::string> ids;
    for (const auto& post : posts) {
        require(post.is_object(), "News post must be an object");
        const auto text = [&](const char* key, std::size_t maximum, bool required = false, bool multiline = false) {
            if (!post.contains(key)) { require(!required, "Required news field missing"); return std::string{}; }
            require(post.at(key).is_string(), "News field must be text");
            const auto value = post.at(key).get<std::string>();
            require(value.size() <= maximum && (!required || !value.empty()), "Invalid news text length");
            for (const unsigned char c : value)
                require((c >= 32 && c != 127) || (multiline && (c == '\n' || c == '\r' || c == '\t')),
                    "Unsupported control character in news text");
            return value;
        };
        NewsPost entry{text("id", 128, true), text("title", 256, true), text("description", 1024),
            text("body", 16384, false, true), text("small_image", 1024), text("large_image", 1024),
            text("fallback_small", 512), text("fallback_large", 512)};
        // Only shown on ReSkate's front page; a date it cannot read is left off rather than
        // turning the whole feed down.
        entry.date = iso_date(text("date", 64));
        require(ids.insert(entry.id).second, "Duplicate news post ID");
        feed.posts.push_back(std::move(entry));
    }
    return feed;
}

}
