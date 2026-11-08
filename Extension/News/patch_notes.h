#pragma once
#include "Launcher/thunderstore.h"
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ReSkate's patch notes, for the front page in the pause menu's HUB tab: the releases on ReSkate's
// GitHub page, newest first, each with its notes as GitHub shows them (markdown, laid out as the Mod
// Manager lays out a README).
namespace dingosdk::news {
inline constexpr std::wstring_view releases_url = L"https://api.github.com/repos/Dingo-Shenanigans/ReSkate/releases?per_page=20";

struct Release {
    std::string tag;   // "v2.1.4-2"
    std::string name;  // the release's title; the tag when it has none
    std::string date;  // "YYYY-MM-DD" it was published; empty when GitHub did not say
    std::vector<thunderstore::ReadmeLine> lines;
};
// GitHub's answer to releases_url: its releases in the order given (newest first), drafts left out,
// at most `most` of them and 300 lines of notes each. Throws when the answer is not a list of releases.
std::vector<Release> parse_releases(std::string_view json, std::size_t most = 20);
// The release's version as a tag shows it: its tag without the leading "v".
std::string release_version(const Release& release);
// Its first `most` bullet lines: what the front page lists of the latest release.
std::vector<std::string> release_highlights(const Release& release, std::size_t most);
// "YYYY-MM-DD" as the menus show a date: "OCT 24, 2026". Empty for anything else.
std::string date_label(std::string_view iso);

// The releases as last read: at once from the copy kept on disk
// (%LOCALAPPDATA%\ReSkate\news\releases.json), and from GitHub when that copy is older than 30
// minutes (never in offline mode).
struct PatchNotes {
    std::vector<Release> releases;
    bool fetching{};
    bool read{};        // a copy was read, from disk or GitHub; until then the list says nothing
    std::string error;  // why the last fetch failed; empty when it did not
    unsigned serial{};  // changes with every new copy
};
// Any thread, as often as it likes. Starts a fetch (on a thread of its own) when one is due.
std::shared_ptr<const PatchNotes> patch_notes();
} // namespace dingosdk::news
