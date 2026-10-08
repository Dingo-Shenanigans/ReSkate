#include "server_release.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace dingosdk::server {
namespace {
bool sha256_text(std::string_view text) noexcept {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace

bool https_url(std::string_view url) noexcept {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme) || url.size() == scheme.size() || url.size() > 2048) return false;
    return std::all_of(url.begin(), url.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("-._~:/?#[]@!$&'()*+,;=%").find(static_cast<char>(c)) != std::string_view::npos;
    });
}

std::map<std::string, std::string, std::less<>> release_assets(std::string_view release_json) {
    const auto release = Json::parse(release_json);
    if (!release.is_object() || !release.contains("assets") || !release.at("assets").is_array())
        throw std::runtime_error("GitHub's answer is not a release");
    std::map<std::string, std::string, std::less<>> assets;
    for (const auto &asset : release.at("assets"))
        if (asset.is_object()) assets[asset.value("name", "")] = asset.value("browser_download_url", "");
    return assets;
}

std::optional<LinuxRelease> parse_linux_release(std::string_view launcher_json,
                                                const std::map<std::string, std::string, std::less<>> &assets) {
    const auto root = Json::parse(launcher_json);
    if (!root.is_object()) throw std::runtime_error("launcher.json is not a JSON object");
    if (!root.contains("server_linux")) return std::nullopt;
    const auto &entry = root.at("server_linux");
    if (!entry.is_object()) throw std::runtime_error("launcher.json \"server_linux\" is not an object");
    LinuxRelease release;
    release.version = entry.value("version", "");
    release.url = entry.value("url", "");
    release.sha256 = entry.value("sha256", "");
    release.exe_sha256 = entry.value("exe_sha256", "");
    if (entry.contains("size") && entry.at("size").is_number_unsigned()) release.size = entry.at("size").get<std::uint64_t>();
    constexpr std::string_view asset = "asset:";
    if (release.url.starts_with(asset)) {
        const auto found = assets.find(std::string_view(release.url).substr(asset.size()));
        if (found == assets.end()) throw std::runtime_error("the release is missing asset " + release.url.substr(asset.size()));
        release.url = found->second;
    }
    if (!https_url(release.url)) throw std::runtime_error("launcher.json \"server_linux.url\" is not an HTTPS link");
    if (!sha256_text(release.sha256) || !sha256_text(release.exe_sha256))
        throw std::runtime_error("launcher.json \"server_linux\" hashes must be 64 lowercase hex digits");
    if (!release.size || release.size > max_linux_release_bytes)
        throw std::runtime_error("launcher.json \"server_linux.size\" is out of range");
    if (release.version.empty() || release.version.size() > 32 ||
        !std::all_of(release.version.begin(), release.version.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_'; }))
        throw std::runtime_error("launcher.json \"server_linux.version\" is not a version");
    return release;
}

std::string checked_archive_listing(std::string_view listing) {
    std::string top;
    std::size_t entries{};
    for (std::size_t start = 0; start < listing.size();) {
        auto end = listing.find('\n', start);
        if (end == std::string_view::npos) end = listing.size();
        auto line = listing.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.front() == '/' || line.find('\\') != std::string_view::npos || line.size() > 512)
            throw std::runtime_error("the update has a file outside its folder");
        // "./name/..." is how some tars write it.
        if (line.starts_with("./")) line.remove_prefix(2);
        std::string_view first;
        for (std::size_t at = 0; at <= line.size();) {
            auto slash = line.find('/', at);
            if (slash == std::string_view::npos) slash = line.size();
            const auto part = line.substr(at, slash - at);
            if (part == ".." || part == "." || (part.empty() && slash != line.size()))
                throw std::runtime_error("the update has a file outside its folder");
            if (at == 0) first = part;
            at = slash + 1;
        }
        if (first.empty()) throw std::runtime_error("the update has a file outside its folder");
        if (top.empty()) top = first;
        else if (top != first) throw std::runtime_error("the update is not one folder");
        ++entries;
    }
    if (top.empty() || entries < 2) throw std::runtime_error("the update is empty");
    return top;
}
} // namespace dingosdk::server
