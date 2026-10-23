#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The Animator Studio's MARKET: animation packs made in the studio and shared on Thunderstore (thunderstore.io/c/reskate),
// browsed, previewed and installed without leaving the game, and the player's own packs published there from the studio.
//
// Only the studio's own packs are listed: a package is one when its manifest points at the studio (studio_url) or its
// description starts with the studio's tag - which the studio's own export writes, and nothing else does. Nothing is
// downloaded from anywhere but thunderstore.io and its hosts, and a downloaded pack goes through the studio's import,
// which takes nothing but the studio's own animations, packs and pictures.
//
// Publishing needs a Thunderstore team's service account token (Thunderstore has no sign-in for other apps): made on
// thunderstore.io under the team's settings, pasted in once, kept encrypted for this Windows user (DPAPI), and sent to
// thunderstore.io only, with the upload itself.
namespace dingosdk::anim_studio::market {
inline constexpr char community[] = "reskate";
inline constexpr char site[] = "https://thunderstore.io";
// The category a pack is listed in: the community has no Animations one (yet).
inline constexpr char category[] = "misc";
// What marks a package as the studio's: its manifest's website_url, and the start of its description.
inline constexpr char studio_url[] = "https://github.com/FourthStudio/ReSkate-Animator-Studio";
inline constexpr char description_tag[] = "Animator Studio pack: ";

struct Pack {
    std::string full_name;      // Owner-Name
    std::string owner, name;    // the team, and the package's name
    std::string title;          // the name, with spaces
    std::string version;        // its newest
    std::string description;    // its short description, without the studio's tag
    std::string icon_url, download_url, package_url;
    std::string updated;        // when its newest version came out ("2026-10-23")
    std::uint64_t downloads{};  // every version's together
    int rating{};
    std::string icon_file;      // its icon once downloaded (UTF-8 path), "" until then
    bool favorite{}, mine{};
    std::string installed;      // the version installed here, "" none
};
struct Detail {
    std::string full_name, version;
    bool loading{}, ok{};
    std::string about;               // its description in full (README)
    std::vector<std::string> tricks; // what is in it, a line each (README)
    std::string error;
};
// A download or an upload in the background, and how far it has got.
struct Job {
    bool running{}, failed{};
    std::string what;    // "Installing Team-Pack", "Publishing ..."
    float fraction{};
    std::string message; // how the last one went
    std::string url;     // a published pack's page
};
struct View {
    bool loading{};
    std::string error;
    std::vector<Pack> packs;
    std::string team; // the linked Thunderstore team, "" none
    Job job;
    Detail detail;
};
View view();
// The listing, fetched in the background: on opening the MARKET (when it is older than a few minutes) or when asked.
void refresh(bool force);
// Each pack's icon downloaded in the background, for the cards.
void want_icons(const std::vector<std::string>& full_names);
void load_detail(const std::string& full_name);
void set_favorite(const std::string& full_name, bool on);
// Downloads the pack and puts it in the library (the studio's import), in the background.
void install(const std::string& full_name);
// Links a Thunderstore team for publishing: its name and a service account token of it.
bool link_account(const std::string& team, const std::string& token, std::string& message);
void unlink_account();
// Builds the pack as a Thunderstore package and publishes it, in the background. `pack` is a pack of the library.
void publish(const std::string& pack);
// The same package written to the exports folder, to upload on thunderstore.io by hand. Returns its path, "" when not.
std::string export_package(const std::string& pack, std::string& message);
// Packs installed with the ReSkate launcher (Mods/<Team-Pack> holding reskate-animations.json): put in the library once
// for each version. Any thread.
void take_installed_mods();
// ---- the package's words (on their own, to be tested) ----
// The package's name on Thunderstore for a pack's name (letters, digits and _ only). The README the studio writes, and
// what it reads back out of one.
inline std::string package_name(const std::string& pack) {
    std::string out;
    for (const char c : pack) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (ok) out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "Animation_Pack";
    return out.substr(0, 100);
}
inline std::string readme(const std::string& title, const std::string& description, const std::vector<std::string>& tricks, const std::string& made_by) {
    std::string out = "# " + title + "\n\n";
    out += (description.empty() ? std::string("An animation pack made in the ReSkate Animator Studio.") : description) + "\n\n";
    out += "## Tricks\n\n";
    for (const auto& t : tricks) out += "- " + t + "\n";
    out += "\n## Install\n\nOpen the Animator Studio in game (F8), go to MARKET and install it there, or install it with the ReSkate launcher. "
           "Then switch the pack on in PACKS.\n";
    if (!made_by.empty()) out += "\nMade by " + made_by + " in the ReSkate Animator Studio.\n";
    return out;
}
inline void read_readme(const std::string& markdown, std::string& about, std::vector<std::string>& tricks) {
    about.clear();
    tricks.clear();
    enum { head, text, list, rest } part = head;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        auto end = markdown.find('\n', start);
        if (end == std::string::npos) end = markdown.size();
        std::string line = markdown.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;
        if (line.starts_with("# ")) { part = text; continue; }
        if (line.starts_with("## ")) { part = line == "## Tricks" ? list : rest; continue; }
        if (part == text) about += line + "\n";
        else if (part == list && line.starts_with("- ") && tricks.size() < 500) tricks.push_back(line.substr(2));
        if (end == markdown.size()) break;
    }
    while (!about.empty() && (about.back() == '\n' || about.back() == ' ')) about.pop_back();
    while (!about.empty() && about.front() == '\n') about.erase(about.begin());
}

// The next version: one past the one published last, else the pack's own.
inline std::string next_version(const std::string& version, const std::string& published) {
    int major = 1, minor = 0, patch = 0;
    if (sscanf_s(version.c_str(), "%d.%d.%d", &major, &minor, &patch) != 3) major = 1, minor = 0, patch = 0;
    if (!published.empty() && published == std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)) ++patch;
    return std::to_string(std::max(0, major)) + "." + std::to_string(std::max(0, minor)) + "." + std::to_string(std::max(0, patch));
}
} // namespace dingosdk::anim_studio::market
