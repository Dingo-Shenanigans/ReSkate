#include "patch_notes.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Profile/local_profile.h"
#include <array>
#include <stdexcept>

namespace dingosdk::news {
namespace {
std::string text_of(const Json& release, std::string_view key) {
    return release.contains(key) && release.at(key).is_string() ? release.at(key).string() : std::string();
}
} // namespace

std::vector<Release> parse_releases(std::string_view json, std::size_t most) {
    const auto root = Json::parse(json);
    if (!root.is_array()) throw std::runtime_error("GitHub's releases answer is not a list.");
    std::vector<Release> releases;
    for (const auto& entry : root) {
        if (releases.size() >= most) break;
        if (!entry.is_object()) continue;
        // (GitHub shows drafts only to the repository's own people; one in a copy is left out all the same.)
        if (entry.contains("draft") && entry.at("draft").is_boolean() && entry.at("draft").get<bool>()) continue;
        Release release;
        release.tag = text_of(entry, "tag_name");
        if (release.tag.empty()) continue;
        release.name = text_of(entry, "name");
        if (release.name.empty()) release.name = release.tag;
        release.date = profile::iso_date(text_of(entry, "published_at"));
        if (release.date.empty()) release.date = profile::iso_date(text_of(entry, "created_at"));
        release.lines = thunderstore::readme_lines(text_of(entry, "body"), 300).lines;
        releases.push_back(std::move(release));
    }
    return releases;
}

std::string release_version(const Release& release) {
    const std::string_view tag = release.tag;
    return std::string(tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') && tag[1] >= '0' && tag[1] <= '9' ? tag.substr(1) : tag);
}

std::vector<std::string> release_highlights(const Release& release, std::size_t most) {
    std::vector<std::string> lines;
    for (const auto& line : release.lines) {
        if (lines.size() >= most) break;
        if (line.kind == thunderstore::ReadmeLine::Kind::bullet) lines.push_back(line.text);
    }
    return lines;
}

std::string date_label(std::string_view iso) {
    const auto date = profile::iso_date(iso);
    if (date.empty()) return {};
    static constexpr std::array<const char*, 12> months{"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const int month = (date[5] - '0') * 10 + (date[6] - '0');
    const auto day = date[8] == '0' ? date.substr(9, 1) : date.substr(8, 2);
    return std::string(months[static_cast<std::size_t>(month - 1)]) + " " + day + ", " + date.substr(0, 4);
}
} // namespace dingosdk::news
