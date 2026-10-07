#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// What the Linux server's self-update reads from a release, apart from how it is fetched
// (server_update.cpp): the release's launcher.json entry for the Linux server, and the checks
// on the tarball before anything is unpacked. Portable, so the tests run on Windows too.
namespace dingosdk::server {
// launcher.json's "server_linux": the tarball (pinned by size and SHA-256) and the SHA-256 of
// the ReSkateServer inside it, which a running server compares with its own.
struct LinuxRelease {
    std::string version, url, sha256, exe_sha256;
    std::uint64_t size{};
};
// The largest tarball a server will download.
inline constexpr std::uint64_t max_linux_release_bytes = 512ull * 1024 * 1024;
// GitHub's answer for a release, as the names of its assets and where each downloads from.
// Throws when the answer is not a release.
std::map<std::string, std::string, std::less<>> release_assets(std::string_view release_json);
// Nothing when the release was made before Linux servers updated themselves (no
// "server_linux"). An "asset:<name>" URL becomes that asset's download link. Throws on an
// entry that is there but wrong: no HTTPS link, a hash that is not one, a size out of range.
std::optional<LinuxRelease> parse_linux_release(std::string_view launcher_json,
                                                const std::map<std::string, std::string, std::less<>> &assets);
// `tar -t`'s list of a release tarball: every entry under the one top folder the package has,
// none absolute or with a ".." in it. Returns that folder's name; throws otherwise.
std::string checked_archive_listing(std::string_view listing);
// A link curl may be handed: https, with nothing but URL characters in it.
bool https_url(std::string_view url) noexcept;
} // namespace dingosdk::server
