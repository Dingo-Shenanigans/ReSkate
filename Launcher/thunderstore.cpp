#include "thunderstore.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Vfs/mod_list.h"

#include <Windows.h>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace dingosdk::thunderstore {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

// Thunderstore's own rule for names and namespaces.
bool plain_name(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
}

std::string text(const Json& row, std::string_view key) {
    return row.contains(key) && row.at(key).is_string() ? row.at(key).string() : std::string();
}

std::uint64_t count(const Json& row, std::string_view key) {
    if (!row.contains(key) || !row.at(key).is_number()) return 0;
    const auto value = row.at(key).get<double>();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

bool flag(const Json& row, std::string_view key) {
    return row.contains(key) && row.at(key).is_boolean() && row.at(key).get<bool>();
}

std::vector<std::string> strings(const Json& row, std::string_view key) {
    std::vector<std::string> result;
    if (!row.contains(key) || !row.at(key).is_array()) return result;
    for (const auto& item : row.at(key))
        if (item.is_string()) result.push_back(item.string());
    return result;
}

bool https(std::string_view url) { return url.starts_with("https://"); }

std::optional<Version> parse_version(const Json& row) {
    if (!row.is_object() || (row.contains("is_active") && !flag(row, "is_active"))) return std::nullopt;
    Version version;
    version.number = text(row, "version_number");
    version.full_name = text(row, "full_name");
    version.description = text(row, "description");
    version.icon = text(row, "icon");
    version.download_url = text(row, "download_url");
    version.website_url = text(row, "website_url");
    version.date_created = text(row, "date_created");
    version.downloads = count(row, "downloads");
    version.file_size = count(row, "file_size");
    if (version.number.empty() || !https(version.download_url)) return std::nullopt;
    if (!https(version.icon)) version.icon.clear();
    return version;
}

std::optional<Package> parse_package(const Json& row) {
    if (!row.is_object()) return std::nullopt;
    Package package;
    package.name = text(row, "name");
    package.owner = text(row, "owner");
    package.full_name = text(row, "full_name");
    package.package_url = text(row, "package_url");
    package.date_created = text(row, "date_created");
    package.date_updated = text(row, "date_updated");
    package.categories = strings(row, "categories");
    package.rating = static_cast<int>(std::min<std::uint64_t>(count(row, "rating_score"), 1u << 30));
    package.pinned = flag(row, "is_pinned");
    package.deprecated = flag(row, "is_deprecated");
    package.nsfw = flag(row, "has_nsfw_content");
    if (!plain_name(package.name) || !plain_name(package.owner) ||
        package.full_name != package.owner + "-" + package.name) return std::nullopt;
    if (!https(package.package_url)) package.package_url.clear();
    if (row.contains("versions") && row.at("versions").is_array())
        for (const auto& item : row.at("versions"))
            if (auto version = parse_version(item)) {
                package.downloads += version->downloads;
                package.versions.push_back(std::move(*version));
            }
    if (package.versions.empty()) return std::nullopt;
    std::stable_sort(package.versions.begin(), package.versions.end(),
        [](const Version& a, const Version& b) { return compare_versions(a.number, b.number) > 0; });
    return package;
}

constexpr JsonLimits listing_limits{256ull * 1024 * 1024, 16, 64ull * 1024 * 1024};

std::uint32_t little32(const unsigned char* bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

} // namespace

std::string community() {
    std::array<char, 128> value{};
    std::size_t length{};
    if (getenv_s(&length, value.data(), value.size(), "RESKATE_THUNDERSTORE_COMMUNITY") == 0 && length > 1) {
        const std::string name(value.data());
        const bool slug = std::all_of(name.begin(), name.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        });
        if (slug) return name;
    }
    return default_community;
}

std::wstring listing_index_url(std::string_view community) {
    return widen(std::string(site) + "/c/" + std::string(community) + "/api/v1/package-listing-index/");
}

std::wstring listing_url(std::string_view community) {
    return widen(std::string(site) + "/c/" + std::string(community) + "/api/v1/package/");
}

std::wstring community_page(std::string_view community) {
    return widen(std::string(site) + "/c/" + std::string(community) + "/");
}

std::string Package::title() const {
    auto result = name;
    std::replace(result.begin(), result.end(), '_', ' ');
    return result;
}

bool Package::in_category(std::string_view category) const {
    return std::find(categories.begin(), categories.end(), category) != categories.end();
}

std::vector<Package> parse_listing(std::string_view json) {
    const auto root = Json::parse(json, listing_limits);
    if (!root.is_array()) fail("The Thunderstore listing is not a JSON array.");
    std::vector<Package> packages;
    for (const auto& row : root)
        if (auto package = parse_package(row)) packages.push_back(std::move(*package));
    return packages;
}

std::wstring readme_url(const Package& package) {
    return widen(std::string(site) + "/api/experimental/package/" + package.owner + "/" + package.name + "/" +
                 package.latest().number + "/readme/");
}

std::string parse_readme(std::string_view json) {
    const auto root = Json::parse(json, JsonLimits{2 * 1024 * 1024, 4, 1024 * 1024});
    if (!root.is_object()) fail("The Thunderstore README answer is not a JSON object.");
    if (!root.contains("markdown") || !root.at("markdown").is_string()) return {};
    return root.at("markdown").string();
}

namespace {

// One line's inline markdown as plain text: images and HTML tags go, links keep their text,
// and the marks for bold, italic and code are dropped.
std::string plain_inline(std::string_view line) {
    std::string out;
    for (std::size_t i = 0; i < line.size();) {
        const char c = line[i];
        // ![alt](url): nothing is shown for an image.   [text](url): the text.
        const bool image = c == '!' && i + 1 < line.size() && line[i + 1] == '[';
        if (image || c == '[') {
            const auto open = i + (image ? 1 : 0);
            const auto close = line.find(']', open);
            if (close != std::string_view::npos && close + 1 < line.size() && line[close + 1] == '(') {
                const auto end = line.find(')', close + 2);
                if (end != std::string_view::npos) {
                    if (!image) out += plain_inline(line.substr(open + 1, close - open - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == '<') {
            const auto end = line.find('>', i + 1);
            if (end != std::string_view::npos) {
                const auto inside = line.substr(i + 1, end - i - 1);
                // <https://...> is a link written out; anything else in brackets is an HTML tag.
                if (inside.starts_with("https://") || inside.starts_with("http://")) out += inside;
                i = end + 1;
                continue;
            }
        }
        if (c == '&') {
            static constexpr std::pair<std::string_view, char> entities[]{{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                                                                          {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
            bool replaced{};
            for (const auto& [name, value] : entities)
                if (line.substr(i).starts_with(name)) {
                    out += value;
                    i += name.size();
                    replaced = true;
                    break;
                }
            if (replaced) continue;
        }
        if (c == '`') { ++i; continue; }
        // ** and __ are bold, ~~ struck through; a lone * is italic. A lone _ is left: names have them.
        if ((c == '*' || c == '_' || c == '~') && i + 1 < line.size() && line[i + 1] == c) { i += 2; continue; }
        if (c == '*') { ++i; continue; }
        if (c == '\\' && i + 1 < line.size() && std::string_view("\\`*_{}[]()#+-.!<>|~").find(line[i + 1]) != std::string_view::npos) {
            out += line[i + 1];
            i += 2;
            continue;
        }
        // Tabs and other controls would only draw as boxes.
        out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
        ++i;
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

} // namespace

Readme readme_lines(std::string_view markdown, std::size_t max_lines) {
    Readme out;
    constexpr std::size_t max_line_bytes = 2000;
    bool fenced{};
    const auto add = [&](ReadmeLine::Kind kind, std::string text) {
        // Blank lines run together into one gap, and none opens the README.
        if (kind == ReadmeLine::Kind::gap && (out.lines.empty() || out.lines.back().kind == ReadmeLine::Kind::gap)) return;
        if (text.size() > max_line_bytes) {
            text.resize(max_line_bytes);
            // (Never half a UTF-8 character.)
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
            if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80)) text.pop_back();
        }
        out.lines.push_back({kind, std::move(text)});
    };
    for (std::size_t at = 0; at <= markdown.size();) {
        if (out.lines.size() >= max_lines) {
            out.cut = markdown.substr(std::min(at, markdown.size())).find_first_not_of(" \t\r\n") != std::string_view::npos;
            break;
        }
        const auto end = std::min(markdown.find('\n', at), markdown.size());
        auto line = markdown.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto indent = std::min(line.find_first_not_of(" \t"), line.size());
        const auto body = line.substr(indent);
        if (body.starts_with("```") || body.starts_with("~~~")) {
            fenced = !fenced;
            continue;
        }
        if (fenced) {
            std::string code(line);
            for (auto& c : code)
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
            add(ReadmeLine::Kind::code, std::move(code));
            continue;
        }
        if (body.empty()) {
            add(ReadmeLine::Kind::gap, {});
            continue;
        }
        // --- *** ___ : a rule. (Also what underlines a heading written the other way; a rule does for it.)
        if (body.size() >= 3 && (body[0] == '-' || body[0] == '*' || body[0] == '_' || body[0] == '=') &&
            body.find_first_not_of(std::string{body[0]} + " ") == std::string_view::npos) {
            add(ReadmeLine::Kind::rule, {});
            continue;
        }
        if (body[0] == '#') {
            const auto level = std::min(body.find_first_not_of('#'), body.size());
            if (level <= 6 && (level == body.size() || body[level] == ' ')) {
                auto text = body.substr(level);
                while (!text.empty() && (text.back() == '#' || text.back() == ' ')) text.remove_suffix(1);
                if (auto plain = plain_inline(text); !plain.empty()) add(ReadmeLine::Kind::heading, std::move(plain));
                continue;
            }
        }
        if (body.size() >= 2 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') {
            if (auto plain = plain_inline(body.substr(2)); !plain.empty()) add(ReadmeLine::Kind::bullet, std::move(plain));
            continue;
        }
        auto text = body;
        while (!text.empty() && text.front() == '>') { // a quotation: its text
            text.remove_prefix(1);
            if (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        }
        if (!text.empty() && text.front() == '|') {
            // A table row: its cells side by side. The row of dashes under the header is not one.
            if (text.find_first_not_of("|-: ") == std::string_view::npos) continue;
            std::string row;
            for (std::size_t cell = 1; cell < text.size();) {
                const auto bar = std::min(text.find('|', cell), text.size());
                if (auto plain = plain_inline(text.substr(cell, bar - cell)); !plain.empty()) row += (row.empty() ? "" : "   ") + plain;
                cell = bar + 1;
            }
            if (!row.empty()) add(ReadmeLine::Kind::text, std::move(row));
            continue;
        }
        if (auto plain = plain_inline(text); !plain.empty()) add(ReadmeLine::Kind::text, std::move(plain));
    }
    while (!out.lines.empty() && out.lines.back().kind == ReadmeLine::Kind::gap) out.lines.pop_back();
    return out;
}

std::vector<std::wstring> parse_index(std::string_view json) {
    const auto root = Json::parse(json, JsonLimits{1024 * 1024, 4, 65536});
    if (!root.is_array()) fail("The Thunderstore listing index is not a JSON array.");
    std::vector<std::wstring> urls;
    for (const auto& item : root) {
        if (!item.is_string() || !https(item.string())) fail("The Thunderstore listing index names a non-HTTPS chunk.");
        urls.push_back(widen(item.string()));
    }
    return urls;
}

std::string gunzip(std::string_view data, std::size_t limit) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 2 || bytes[0] != 0x1f || bytes[1] != 0x8b) {
        if (data.size() > limit) fail("The Thunderstore listing is too large.");
        return std::string(data);
    }
    if (data.size() < 18 || bytes[2] != 8) fail("The Thunderstore listing is not a gzip stream miniz can read.");
    const unsigned flags = bytes[3];
    std::size_t offset = 10;
    const auto need = [&](std::size_t more) { if (offset + more > data.size() - 8) fail("The Thunderstore listing is truncated."); };
    if (flags & 0x04) { // FEXTRA
        need(2);
        const std::size_t extra = bytes[offset] | (bytes[offset + 1] << 8);
        offset += 2;
        need(extra);
        offset += extra;
    }
    for (const unsigned field : {0x08u, 0x10u}) { // FNAME, FCOMMENT: zero-terminated
        if (!(flags & field)) continue;
        while (true) { need(1); if (bytes[offset++] == 0) break; }
    }
    if (flags & 0x02) { need(2); offset += 2; } // FHCRC
    const auto crc = little32(bytes + data.size() - 8);
    const std::size_t size = little32(bytes + data.size() - 4);
    if (size > limit) fail("The Thunderstore listing is too large.");
    std::string output(size, '\0');
    const auto written = tinfl_decompress_mem_to_mem(output.data(), output.size(), bytes + offset,
        data.size() - 8 - offset, 0);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || written != size ||
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(output.data()), output.size()) != crc)
        fail("The Thunderstore listing is damaged (gzip check failed).");
    return output;
}

int compare_versions(std::string_view a, std::string_view b) {
    while (!a.empty() || !b.empty()) {
        const auto take = [](std::string_view& value) {
            const auto dot = value.find('.');
            const auto part = value.substr(0, dot);
            value = dot == std::string_view::npos ? std::string_view() : value.substr(dot + 1);
            return part;
        };
        const auto left = take(a), right = take(b);
        const bool numeric = left.find_first_not_of("0123456789") == std::string_view::npos &&
                             right.find_first_not_of("0123456789") == std::string_view::npos;
        if (numeric) {
            // Leading zeros aside, the longer number is the larger one.
            auto l = left.substr(std::min(left.find_first_not_of('0'), left.size()));
            auto r = right.substr(std::min(right.find_first_not_of('0'), right.size()));
            if (l.size() != r.size()) return l.size() < r.size() ? -1 : 1;
            if (const auto order = l.compare(r)) return order < 0 ? -1 : 1;
        } else if (const auto order = left.compare(right)) {
            return order < 0 ? -1 : 1;
        }
    }
    return 0;
}

std::string folder_for(std::string_view full_name) {
    return std::string(full_name.substr(0, std::min(full_name.size(), mods::maximum_mod_name)));
}

bool update_available(const Package& package, const Installed& installed) {
    const auto found = installed.find(folder_for(package.full_name));
    return found != installed.end() && !found->second.empty() && compare_versions(package.latest().number, found->second) > 0;
}

} // namespace dingosdk::thunderstore
